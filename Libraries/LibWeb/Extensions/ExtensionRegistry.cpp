/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16StringBuilder.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ExtensionRegistry.h>
#include <LibWeb/Extensions/ExtensionWorld.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>

namespace Web::Extensions {

extern String const& extension_api_source;

ExtensionRegistry& ExtensionRegistry::the()
{
    static ExtensionRegistry registry;
    return registry;
}

Extension const* ExtensionRegistry::extension(StringView id) const
{
    for (auto const& extension : m_extensions) {
        if (extension.id == id)
            return &extension;
    }
    return nullptr;
}

static Vector<String> string_array(JsonObject const& object, StringView key)
{
    Vector<String> strings;
    if (auto array = object.get_array(key); array.has_value()) {
        array->for_each([&](JsonValue const& value) {
            if (value.is_string())
                strings.append(value.as_string());
        });
    }
    return strings;
}

static RunAt run_at_from_string(StringView run_at)
{
    if (run_at == "document_start"sv)
        return RunAt::DocumentStart;
    if (run_at == "document_end"sv)
        return RunAt::DocumentEnd;
    return RunAt::DocumentIdle;
}

ErrorOr<void> ExtensionRegistry::set_extensions_from_bytes(ReadonlyBytes bytes)
{
    Vector<Extension> extensions;

    if (!bytes.is_empty()) {
        auto json = TRY(JsonValue::from_string(StringView { bytes }));
        if (!json.is_object())
            return Error::from_string_literal("Extension data is not an object");

        auto extension_list = json.as_object().get_array("extensions"sv);
        if (!extension_list.has_value())
            return Error::from_string_literal("Extension data has no extension list");

        TRY(extension_list->try_for_each([&](JsonValue const& value) -> ErrorOr<void> {
            if (!value.is_object())
                return {};
            auto const& object = value.as_object();

            auto id = object.get_string("id"sv);
            if (!id.has_value())
                return {};

            Extension extension;
            extension.id = *id;
            extension.manifest_json = object.get_object("manifest"sv).map([](auto const& manifest) { return manifest.serialized(); }).value_or("{}"_string);
            extension.messages_json = object.get_object("messages"sv).map([](auto const& messages) { return messages.serialized(); }).value_or("{}"_string);
            extension.ui_language = object.get_string("ui_language"sv).value_or("en-US"_string);

            if (auto content_scripts = object.get_array("content_scripts"sv); content_scripts.has_value()) {
                content_scripts->for_each([&](JsonValue const& content_script_value) {
                    if (!content_script_value.is_object())
                        return;
                    auto const& content_script_object = content_script_value.as_object();

                    ContentScript content_script;
                    content_script.matches = string_array(content_script_object, "matches"sv);
                    content_script.exclude_matches = string_array(content_script_object, "exclude_matches"sv);
                    content_script.include_globs = string_array(content_script_object, "include_globs"sv);
                    content_script.exclude_globs = string_array(content_script_object, "exclude_globs"sv);
                    content_script.all_frames = content_script_object.get_bool("all_frames"sv).value_or(false);
                    content_script.match_about_blank = content_script_object.get_bool("match_about_blank"sv).value_or(false);
                    content_script.main_world = content_script_object.get_bool("main_world"sv).value_or(false);
                    content_script.run_at = run_at_from_string(content_script_object.get_string("run_at"sv).value_or({}));

                    if (auto scripts = content_script_object.get_array("js"sv); scripts.has_value()) {
                        scripts->for_each([&](JsonValue const& script) {
                            if (!script.is_object())
                                return;
                            auto path = script.as_object().get_string("path"sv);
                            auto source = script.as_object().get_string("source"sv);
                            if (path.has_value() && source.has_value())
                                content_script.js.append({ *path, Utf16String::from_utf8(*source) });
                        });
                    }

                    if (auto css = content_script_object.get_string("css"sv); css.has_value())
                        content_script.css = Utf16String::from_utf8(*css);

                    extension.content_scripts.append(move(content_script));
                });
            }

            if (auto resources = object.get_object("resources"sv); resources.has_value()) {
                TRY(resources->try_for_each_member([&](String const& path, JsonValue const& resource) -> ErrorOr<void> {
                    if (!resource.is_object())
                        return {};
                    auto data = resource.as_object().get_string("data"sv);
                    auto mime_type = resource.as_object().get_string("mime_type"sv);
                    if (!data.has_value() || !mime_type.has_value())
                        return {};
                    extension.resources.set(path, { TRY(decode_base64(*data)), *mime_type, string_array(resource.as_object(), "matches"sv) });
                    return {};
                }));
            }

            extensions.append(move(extension));
            return {};
        }));
    }

    m_extensions = move(extensions);
    return {};
}

void ExtensionRegistry::set_storage(JsonObject storage)
{
    m_storage.clear();
    storage.for_each_member([&](String const& extension_id, JsonValue const& areas) {
        if (!areas.is_object())
            return;
        auto& extension_storage = m_storage.ensure(extension_id);
        areas.as_object().for_each_member([&](String const& area, JsonValue const& items) {
            if (items.is_object())
                extension_storage.set(area, items.as_object());
        });
    });
}

String ExtensionRegistry::storage_snapshot(StringView extension_id, StringView area) const
{
    auto extension_storage = m_storage.find(extension_id);
    if (extension_storage == m_storage.end())
        return "{}"_string;
    auto items = extension_storage->value.find(area);
    if (items == extension_storage->value.end())
        return "{}"_string;
    return items->value.serialized();
}

static bool is_storage_area(StringView area)
{
    return area == "local"sv || area == "sync"sv;
}

void ExtensionRegistry::apply_storage_changes(StringView extension_id, StringView area, StringView changes_json, ChangeSource source)
{
    if (!extension(extension_id) || !is_storage_area(area))
        return;

    auto changes = JsonValue::from_string(changes_json);
    if (changes.is_error() || !changes.value().is_object())
        return;
    auto const& changes_object = changes.value().as_object();

    auto& items = m_storage.ensure(MUST(String::from_utf8(extension_id))).ensure(MUST(String::from_utf8(area)));

    // The record of what changed, as storage.onChanged gives it: {key: {oldValue, newValue}}.
    JsonObject change_record;
    auto record_change = [&](String const& key, Optional<JsonValue const&> old_value, Optional<JsonValue const&> new_value) {
        JsonObject change;
        if (old_value.has_value())
            change.set("oldValue"sv, *old_value);
        if (new_value.has_value())
            change.set("newValue"sv, *new_value);
        change_record.set(key, move(change));
    };

    if (changes_object.get_bool("clear"sv).value_or(false)) {
        items.for_each_member([&](String const& key, JsonValue const& value) {
            record_change(key, value, {});
        });
        items = {};
    }

    for (auto const& key : string_array(changes_object, "remove"sv)) {
        if (auto old_value = items.get(key); old_value.has_value()) {
            record_change(key, *old_value, {});
            items.remove(key);
        }
    }

    if (auto set = changes_object.get_object("set"sv); set.has_value()) {
        set->for_each_member([&](String const& key, JsonValue const& value) {
            auto old_value = items.get(key);
            if (old_value.has_value() && old_value->serialized() == value.serialized())
                return;
            record_change(key, old_value.map([](auto& value) -> JsonValue const& { return value; }), value);
            items.set(key, value);
        });
    }

    if (change_record.is_empty())
        return;

    // Tell every world of the extension in this process, in a task of its own, as Chrome fires storage.onChanged after
    // the call that made the change has returned.
    auto area_string = MUST(String::from_utf8(area));
    auto change_record_json = change_record.serialized();
    auto documents = HTML::main_thread_event_loop().documents_in_this_event_loop_matching([&](DOM::Document& document) {
        return document.extension_world(extension_id) != nullptr;
    });
    for (auto& document : documents) {
        auto world = document->extension_world(extension_id);
        HTML::queue_global_task(HTML::Task::Source::Unspecified, HTML::relevant_global_object(*document),
            GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { *world }, area_string, change_record_json), [](auto& world, auto& area, auto& change_record_json) {
                world->dispatch_storage_change(area, change_record_json);
            }));
    }

    if (source == ChangeSource::ThisProcess && on_storage_change)
        on_storage_change(MUST(String::from_utf8(extension_id)), area_string, MUST(String::from_utf8(changes_json)));
}

Optional<WebAccessibleResource const&> ExtensionRegistry::web_accessible_resource(URL::URL const& url, URL::Origin const& requester) const
{
    if (url.scheme() != "chrome-extension"sv)
        return {};

    auto const* extension = this->extension(url.serialized_host());
    if (!extension)
        return {};

    auto path = URL::percent_decode(url.serialize_path());
    auto resource = extension->resources.find(path.view().trim("/"sv, TrimMode::Left));
    if (resource == extension->resources.end())
        return {};

    if (!resource->value.matches.is_empty()) {
        if (requester.is_opaque())
            return {};
        auto requester_url = URL::Parser::basic_parse(ByteString::formatted("{}/", requester.serialize()));
        if (!requester_url.has_value() || !any_of(resource->value.matches, [&](auto const& pattern) { return match_pattern_matches(pattern, *requester_url); }))
            return {};
    }
    return resource->value;
}

bool glob_matches(StringView glob, StringView text)
{
    return text.matches(glob, CaseSensitivity::CaseSensitive);
}

// https://developer.chrome.com/docs/extensions/develop/concepts/match-patterns
bool match_pattern_matches(StringView pattern, URL::URL const& url)
{
    auto scheme = url.scheme();

    if (pattern == "<all_urls>"sv)
        return scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv, "ftp"sv, "data"sv, "file"sv);

    auto scheme_end = pattern.find("://"sv);
    if (!scheme_end.has_value())
        return false;

    auto pattern_scheme = pattern.substring_view(0, *scheme_end);
    auto remainder = pattern.substring_view(*scheme_end + 3);

    if (pattern_scheme == "*"sv) {
        if (!scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv))
            return false;
    } else if (pattern_scheme != scheme) {
        return false;
    }

    auto path_start = remainder.find('/');
    if (!path_start.has_value())
        return false;

    auto pattern_host = remainder.substring_view(0, *path_start);
    auto pattern_path = remainder.substring_view(*path_start);

    if (scheme != "file"sv) {
        // A pattern without a port matches any port.
        auto bracket = pattern_host.find_last(']');
        if (auto colon = pattern_host.find_last(':'); colon.has_value() && (!bracket.has_value() || *colon > *bracket)) {
            auto pattern_port = pattern_host.substring_view(*colon + 1);
            pattern_host = pattern_host.substring_view(0, *colon);
            if (pattern_port != "*"sv) {
                auto port = pattern_port.to_number<u16>();
                if (!port.has_value() || *port != url.port_or_default())
                    return false;
            }
        }

        auto host = url.serialized_host();
        if (pattern_host == "*"sv) {
            // Any host.
        } else if (pattern_host.starts_with("*."sv)) {
            auto domain = pattern_host.substring_view(2);
            if (!host.equals_ignoring_ascii_case(domain)) {
                if (host.length() <= domain.length() || !host.ends_with(domain, CaseSensitivity::CaseInsensitive) || host[host.length() - domain.length() - 1] != '.')
                    return false;
            }
        } else if (!host.equals_ignoring_ascii_case(pattern_host)) {
            return false;
        }
    }

    // The path of a pattern is matched against the URL's path and query.
    StringBuilder path_and_query;
    path_and_query.append(url.serialize_path());
    if (auto query = url.query(); query.has_value()) {
        path_and_query.append('?');
        path_and_query.append(*query);
    }
    return glob_matches(pattern_path, path_and_query.string_view());
}

static Optional<URL::URL> url_for_matching(ContentScript const& content_script, DOM::Document const& document)
{
    auto const& url = document.url();
    if (!url.scheme().is_one_of("about"sv, "blob"sv, "data"sv))
        return url;

    // Frames such as about:blank, whose URL says nothing of where they came from, match by their origin, if the content
    // script asks for that. Without it, a data: URL matches as itself.
    if (!content_script.match_about_blank && url.scheme() == "data"sv)
        return url;
    if (!content_script.match_about_blank)
        return {};
    auto const& origin = document.origin();
    if (origin.is_opaque())
        return {};
    return URL::Parser::basic_parse(ByteString::formatted("{}/", origin.serialize()));
}

bool ExtensionRegistry::content_script_matches(ContentScript const& content_script, DOM::Document const& document) const
{
    if (!content_script.all_frames) {
        auto navigable = document.navigable();
        if (!navigable || !navigable->is_top_level_traversable())
            return false;
    }

    auto url = url_for_matching(content_script, document);
    if (!url.has_value())
        return false;

    if (!any_of(content_script.matches, [&](auto const& pattern) { return match_pattern_matches(pattern, *url); }))
        return false;
    if (any_of(content_script.exclude_matches, [&](auto const& pattern) { return match_pattern_matches(pattern, *url); }))
        return false;

    auto const& serialized_url = url->serialize();
    if (!content_script.include_globs.is_empty() && !any_of(content_script.include_globs, [&](auto const& glob) { return glob_matches(glob, serialized_url); }))
        return false;
    if (any_of(content_script.exclude_globs, [&](auto const& glob) { return glob_matches(glob, serialized_url); }))
        return false;

    return true;
}

Utf16String ExtensionRegistry::style_sheet_for_document(DOM::Document const& document) const
{
    if (m_extensions.is_empty() || document.is_decoded_svg() || !document.browsing_context())
        return {};

    Utf16StringBuilder builder;
    for (auto const& extension : m_extensions) {
        for (auto const& content_script : extension.content_scripts) {
            if (content_script.css.is_empty() || !content_script_matches(content_script, document))
                continue;
            if (!builder.is_empty())
                builder.append_ascii('\n');
            builder.append(content_script.css);
        }
    }
    return builder.to_string();
}

static JS::ThrowCompletionOr<JS::Value> storage_get(JS::VM& vm)
{
    auto extension_id = TRY(vm.argument(0).to_utf16_string(vm)).to_well_formed_utf8();
    auto area = TRY(vm.argument(1).to_utf16_string(vm)).to_well_formed_utf8();
    return JS::PrimitiveString::create(vm, Utf16String::from_utf8(ExtensionRegistry::the().storage_snapshot(extension_id, area)));
}

static JS::ThrowCompletionOr<JS::Value> storage_apply(JS::VM& vm)
{
    auto extension_id = TRY(vm.argument(0).to_utf16_string(vm)).to_well_formed_utf8();
    auto area = TRY(vm.argument(1).to_utf16_string(vm)).to_well_formed_utf8();
    auto changes = TRY(vm.argument(2).to_utf16_string(vm)).to_well_formed_utf8();
    ExtensionRegistry::the().apply_storage_changes(extension_id, area, changes, ExtensionRegistry::ChangeSource::ThisProcess);
    return JS::js_undefined();
}

static void install_api(ExtensionWorld& world, Extension const& extension)
{
    auto& realm = world.realm();
    auto& vm = realm.vm();
    auto& settings = HTML::principal_realm_settings_object(realm);

    auto factory_script = HTML::ClassicScript::create_in_realm("chrome-extension-api.js", Utf16String::from_utf8(extension_api_source), settings, realm, settings.api_base_url());
    auto factory = factory_script->run();
    if (factory.is_abrupt() || !factory.value().is_function()) {
        dbgln("Unable to create the extension API for {}", extension.id);
        return;
    }

    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    auto native = JS::Object::create(realm, realm.intrinsics().object_prototype());
    native->define_native_function(realm, "storageGet"_utf16_fly_string, storage_get, 2, JS::default_attributes);
    native->define_native_function(realm, "storageApply"_utf16_fly_string, storage_apply, 3, JS::default_attributes);

    auto result = JS::call(vm, factory.value(), JS::js_undefined(),
        native,
        JS::PrimitiveString::create(vm, Utf16String::from_utf8(extension.id)),
        JS::PrimitiveString::create(vm, Utf16String::from_utf8(extension.manifest_json)),
        JS::PrimitiveString::create(vm, Utf16String::from_utf8(extension.messages_json)),
        JS::PrimitiveString::create(vm, Utf16String::from_utf8(extension.ui_language)));
    if (result.is_error()) {
        HTML::report_exception(result, realm);
        return;
    }

    auto& pair = result.value().as_object();
    auto api = MUST(pair.get(0));
    auto dispatch_storage_change = MUST(pair.get(1));
    world.set_api(api.as_object(), dispatch_storage_change.as_function());

    realm.global_object().define_direct_property("chrome"_utf16_fly_string, api, JS::Attribute::Writable | JS::Attribute::Configurable);
}

void ExtensionRegistry::inject(DOM::Document& document, Extension const& extension, Vector<ContentScript const*> const& content_scripts)
{
    for (auto const* content_script : content_scripts) {
        GC::Ptr<ExtensionWorld> world;
        if (!content_script->main_world) {
            world = document.extension_world(extension.id);
            if (!world) {
                world = ExtensionWorld::create(document, extension.id);
                document.add_extension_world(*world);
                install_api(*world, extension);
            }
        }

        for (auto const& script : content_script->js) {
            auto filename = ByteString::formatted("chrome-extension://{}/{}", extension.id, script.path);
            if (world) {
                world->run_script(script.source, filename);
            } else {
                auto& settings = document.relevant_settings_object();
                (void)HTML::ClassicScript::create(filename, script.source, settings, settings.api_base_url())->run();
            }
            if (!document.is_fully_active())
                return;
        }
    }
}

void ExtensionRegistry::run_content_scripts(DOM::Document& document, RunAt run_at)
{
    if (m_extensions.is_empty())
        return;

    auto target_stage = to_underlying(run_at) + 1;
    if (document.extension_content_script_stage() >= target_stage)
        return;

    auto window = document.window();
    if (document.is_decoded_svg() || !window || &window->associated_document() != &document || !document.browsing_context() || !document.is_fully_active() || document.is_scripting_disabled())
        return;

    while (document.extension_content_script_stage() < target_stage) {
        auto stage = static_cast<RunAt>(document.extension_content_script_stage());
        // NB: Advance first, as a content script may do something that asks for content scripts to run again.
        document.set_extension_content_script_stage(document.extension_content_script_stage() + 1);

        for (auto const& extension : m_extensions) {
            Vector<ContentScript const*> content_scripts;
            for (auto const& content_script : extension.content_scripts) {
                if (content_script.run_at == stage && !content_script.js.is_empty() && content_script_matches(content_script, document))
                    content_scripts.append(&content_script);
            }
            if (content_scripts.is_empty())
                continue;

            inject(document, extension, content_scripts);
            if (!document.is_fully_active())
                return;
        }
    }
}

}
