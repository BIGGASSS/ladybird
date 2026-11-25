/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16StringBuilder.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ExtensionContext.h>
#include <LibWeb/Extensions/ExtensionRegistry.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWebCommon/Extensions/MatchPattern.h>

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

// The path of a file of an extension, as the browser process finds it: canonical, and relative to the extension's root.
static Optional<String> canonical_package_path(StringView path)
{
    auto canonical = LexicalPath::canonicalized_path(ByteString::formatted("/{}", path));
    auto string = String::from_utf8(canonical.view().substring_view(1));
    if (string.is_error())
        return {};
    return string.release_value();
}

// NB: A path that is not UTF-8 once decoded names no file of an extension.
static Optional<String> resource_path(URL::URL const& url)
{
    return canonical_package_path(URL::percent_decode(url.serialize_path()));
}

// https://w3c.github.io/webappsec-csp/#parse-serialized-policy
// A sandbox policy has to sandbox the page into an opaque origin, which it would not with allow-same-origin.
static bool is_valid_sandbox_policy(StringView policy)
{
    if (!all_of(policy, [](char c) { return is_ascii(c) && (c == '\t' || !is_ascii_control(c)); }))
        return false;
    bool has_sandbox = false;
    for (auto directive : policy.split_view(';')) {
        // NB: Only the whitespace that the CSP parser splits on may separate tokens, so that both see the same directives.
        auto tokens = directive.split_view_if([](char c) { return c == ' ' || c == '\t'; });
        if (tokens.is_empty() || !tokens.first().equals_ignoring_ascii_case("sandbox"sv))
            continue;
        has_sandbox = true;
        if (any_of(tokens, [](auto token) { return token.equals_ignoring_ascii_case("allow-same-origin"sv); }))
            return false;
    }
    return has_sandbox;
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
            // https://developer.chrome.com/docs/extensions/reference/manifest/sandbox
            auto const& manifest = *object.get_object("manifest"sv);
            if (auto sandbox = manifest.get_object("sandbox"sv); sandbox.has_value()) {
                for (auto const& page : string_array(*sandbox, "pages"sv)) {
                    if (auto path = canonical_package_path(page); path.has_value())
                        extension.sandboxed_pages.append(path.release_value());
                }
            }

            // Manifest V3 gives the policy of sandboxed pages in content_security_policy, Manifest V2 in sandbox. A policy
            // that would not sandbox the page into an opaque origin gives way to Chrome's default sandbox.
            Optional<String> sandbox_policy;
            if (auto policies = manifest.get_object("content_security_policy"sv); policies.has_value())
                sandbox_policy = policies->get_string("sandbox"sv).copy();
            if (auto sandbox = manifest.get_object("sandbox"sv); !sandbox_policy.has_value() && sandbox.has_value())
                sandbox_policy = sandbox->get_string("content_security_policy"sv).copy();
            if (!sandbox_policy.has_value() || !is_valid_sandbox_policy(*sandbox_policy))
                sandbox_policy = "sandbox allow-scripts allow-forms allow-popups allow-modals"_string;
            extension.sandbox_policy = sandbox_policy.release_value();

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
    return area == "local"sv || area == "sync"sv || area == "session"sv;
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

    // Tell every context of the extension in this process, in a task of its own, as Chrome fires storage.onChanged
    // after the call that made the change has returned.
    auto area_string = MUST(String::from_utf8(area));
    auto change_record_json = change_record.serialized();
    for_each_context([&](ExtensionContext& context) {
        if (context.extension_id() != extension_id)
            return;
        HTML::queue_global_task(HTML::Task::Source::Unspecified, HTML::relevant_global_object(context.document()),
            GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { context }, area_string, change_record_json), [](auto& context, auto& area, auto& change_record_json) {
                auto& vm = context->realm().vm();
                JS::Value arguments[] = {
                    JS::PrimitiveString::create(vm, "storage"_utf16),
                    JS::PrimitiveString::create(vm, Utf16String::from_utf8(area)),
                    JS::PrimitiveString::create(vm, Utf16String::from_utf8(change_record_json)),
                };
                (void)context->dispatch(arguments);
            }));
    });

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

    auto path = resource_path(url);
    if (!path.has_value())
        return {};
    auto resource = extension->resources.find(*path);
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

Optional<String> ExtensionRegistry::sandbox_policy_for(URL::URL const& url) const
{
    if (url.scheme() != "chrome-extension"sv)
        return {};
    auto const* extension = this->extension(url.serialized_host());
    auto path = resource_path(url);
    if (!extension || !path.has_value() || !extension->sandboxed_pages.contains_slow(*path))
        return {};
    return extension->sandbox_policy;
}

void ExtensionRegistry::load_resource(URL::URL const& url, ResourceCallback callback)
{
    auto path = resource_path(url);
    if (url.scheme() != "chrome-extension"sv || !extension(url.serialized_host()) || !path.has_value() || !on_load_resource) {
        callback->function()({}, {});
        return;
    }

    auto request_id = ++m_next_resource_request_id;
    m_pending_resource_loads.set(request_id, GC::make_root(callback));
    on_load_resource(request_id, MUST(String::from_utf8(url.serialized_host())), path.release_value());
}

void ExtensionRegistry::did_load_resource(u64 request_id, Optional<ReadonlyBytes> data, StringView mime_type)
{
    auto callback = m_pending_resource_loads.take(request_id);
    if (callback.has_value())
        (*callback)->function()(data, mime_type);
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

static JS::ThrowCompletionOr<String> string_argument(JS::VM& vm, size_t index)
{
    return TRY(vm.argument(index).to_utf16_string(vm)).to_well_formed_utf8();
}

static JS::ThrowCompletionOr<JS::Value> storage_get(JS::VM& vm)
{
    auto extension_id = TRY(string_argument(vm, 0));
    auto area = TRY(string_argument(vm, 1));
    return JS::PrimitiveString::create(vm, Utf16String::from_utf8(ExtensionRegistry::the().storage_snapshot(extension_id, area)));
}

static JS::ThrowCompletionOr<JS::Value> storage_apply(JS::VM& vm)
{
    auto extension_id = TRY(string_argument(vm, 0));
    auto area = TRY(string_argument(vm, 1));
    auto changes = TRY(string_argument(vm, 2));
    ExtensionRegistry::the().apply_storage_changes(extension_id, area, changes, ExtensionRegistry::ChangeSource::ThisProcess);
    return JS::js_undefined();
}

// call(contextId, method, argumentsJSON): asks the browser process to do what an API function does, and gives back the
// ID of the call, whose result the context's dispatcher gets.
static JS::ThrowCompletionOr<JS::Value> api_call(JS::VM& vm)
{
    auto context_id = TRY(vm.argument(0).to_double(vm));
    auto method = TRY(string_argument(vm, 1));
    auto arguments = TRY(string_argument(vm, 2));
    auto call_id = ExtensionRegistry::the().call(static_cast<u64>(context_id), method, arguments);
    return JS::Value(static_cast<double>(call_id));
}

// respond(deliveryId, responseJSON): the response to a message that a listener of the context answers asynchronously.
static JS::ThrowCompletionOr<JS::Value> api_respond(JS::VM& vm)
{
    auto delivery_id = TRY(vm.argument(0).to_double(vm));
    auto response = TRY(string_argument(vm, 1));
    if (ExtensionRegistry::the().on_message_response)
        ExtensionRegistry::the().on_message_response(static_cast<u64>(delivery_id), true, response);
    return JS::js_undefined();
}

static void install_api(ExtensionContext& context, Extension const& extension)
{
    auto& realm = context.realm();
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
    native->define_native_function(realm, "call"_utf16_fly_string, api_call, 3, JS::default_attributes);
    native->define_native_function(realm, "respond"_utf16_fly_string, api_respond, 2, JS::default_attributes);

    auto kind = context.kind() == ExtensionContext::Kind::ContentScript ? "content"_utf16 : "page"_utf16;
    auto result = JS::call(vm, factory.value(), JS::js_undefined(),
        native,
        JS::PrimitiveString::create(vm, Utf16String::from_utf8(extension.id)),
        JS::PrimitiveString::create(vm, Utf16String::from_utf8(extension.manifest_json)),
        JS::PrimitiveString::create(vm, Utf16String::from_utf8(extension.messages_json)),
        JS::PrimitiveString::create(vm, Utf16String::from_utf8(extension.ui_language)),
        JS::PrimitiveString::create(vm, kind),
        JS::Value(static_cast<double>(context.id())));
    if (result.is_error()) {
        HTML::report_exception(result, realm);
        return;
    }

    auto& pair = result.value().as_object();
    auto api = MUST(pair.get(0));
    auto dispatch = MUST(pair.get(1));
    context.set_api(api.as_object(), dispatch.as_function());

    realm.global_object().define_direct_property("chrome"_utf16_fly_string, api, JS::Attribute::Writable | JS::Attribute::Configurable);
}

void ExtensionRegistry::inject(DOM::Document& document, Extension const& extension, Vector<ContentScript const*> const& content_scripts)
{
    for (auto const* content_script : content_scripts) {
        GC::Ptr<ExtensionContext> context;
        if (!content_script->main_world) {
            context = document.extension_context(extension.id);
            if (!context) {
                context = ExtensionContext::create_for_content_scripts(document, extension.id);
                document.add_extension_context(*context);
                install_api(*context, extension);
            }
        }

        for (auto const& script : content_script->js) {
            auto filename = ByteString::formatted("chrome-extension://{}/{}", extension.id, script.path);
            if (context) {
                context->run_script(script.source, filename);
            } else {
                auto& settings = document.relevant_settings_object();
                (void)HTML::ClassicScript::create(filename, script.source, settings, settings.api_base_url())->run();
            }
            if (!document.is_fully_active())
                return;
        }
    }
}

void ExtensionRegistry::for_each_context(Function<void(ExtensionContext&)> callback) const
{
    auto documents = HTML::main_thread_event_loop().documents_in_this_event_loop_matching([](DOM::Document& document) {
        return document.has_extension_contexts();
    });
    for (auto& document : documents) {
        for (auto& context : document->extension_contexts()) {
            if (!context->is_detached())
                callback(*context);
        }
    }
}

GC::Ptr<ExtensionContext> ExtensionRegistry::context(u64 context_id) const
{
    GC::Ptr<ExtensionContext> result;
    for_each_context([&](ExtensionContext& context) {
        if (context.id() == context_id)
            result = context;
    });
    return result;
}

u64 ExtensionRegistry::call(u64 context_id, StringView method, StringView arguments)
{
    auto context = this->context(context_id);
    auto call_id = ++m_next_call_id;
    if (!context || !on_api_call) {
        did_complete_call(call_id, context_id, false, "\"The extension context is gone\""sv);
        return call_id;
    }
    on_api_call(context->document().page().client().id(), context_id, call_id, context->extension_id(), MUST(String::from_utf8(method)), MUST(String::from_utf8(arguments)));
    return call_id;
}

void ExtensionRegistry::did_complete_call(u64 call_id, u64 context_id, bool succeeded, StringView result)
{
    auto context = this->context(context_id);
    if (!context)
        return;

    // NB: The result comes in a task of its own, even when the call fails at once, as an API's callback is never called
    //     before the API function returns.
    HTML::queue_global_task(HTML::Task::Source::Unspecified, HTML::relevant_global_object(context->document()),
        GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { *context }, call_id, succeeded, MUST(String::from_utf8(result))), [](auto& context, auto call_id, auto succeeded, auto& result) {
            auto& vm = context->realm().vm();
            JS::Value arguments[] = {
                JS::PrimitiveString::create(vm, "result"_utf16),
                JS::Value(static_cast<double>(call_id)),
                JS::Value(succeeded),
                JS::PrimitiveString::create(vm, Utf16String::from_utf8(result)),
            };
            (void)context->dispatch(arguments);
        }));
}

void ExtensionRegistry::deliver_event(StringView extension_id, StringView name, StringView arguments)
{
    for_each_context([&](ExtensionContext& context) {
        if (context.extension_id() != extension_id || context.kind() != ExtensionContext::Kind::ExtensionPage)
            return;
        HTML::queue_global_task(HTML::Task::Source::Unspecified, HTML::relevant_global_object(context.document()),
            GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { context }, MUST(String::from_utf8(name)), MUST(String::from_utf8(arguments))), [](auto& context, auto& name, auto& arguments) {
                auto& vm = context->realm().vm();
                JS::Value dispatch_arguments[] = {
                    JS::PrimitiveString::create(vm, "event"_utf16),
                    JS::PrimitiveString::create(vm, Utf16String::from_utf8(name)),
                    JS::PrimitiveString::create(vm, Utf16String::from_utf8(arguments)),
                };
                (void)context->dispatch(dispatch_arguments);
            }));
    });
}

ExtensionRegistry::MessageDeliveryResult ExtensionRegistry::deliver_message(u64 delivery_id, StringView extension_id, Optional<PageId> content_scripts_of_page, bool only_top_level_frame, u64 excluded_context_id, StringView message, StringView sender)
{
    Vector<GC::Ref<ExtensionContext>> targets;
    for_each_context([&](ExtensionContext& context) {
        if (context.extension_id() != extension_id || context.id() == excluded_context_id)
            return;
        if (content_scripts_of_page.has_value()) {
            if (context.kind() != ExtensionContext::Kind::ContentScript || context.document().page().client().id() != *content_scripts_of_page)
                return;
            if (only_top_level_frame) {
                auto navigable = context.document().navigable();
                if (!navigable || !navigable->is_top_level_traversable())
                    return;
            }
            targets.append(context);
        } else if (context.kind() == ExtensionContext::Kind::ExtensionPage) {
            targets.append(context);
        }
    });

    // Each listener is told the message. The first response goes back to the sender, but a listener may also say that it
    // will respond later.
    MessageDeliveryResult result;
    for (auto& context : targets) {
        auto& vm = context->realm().vm();
        JS::Value arguments[] = {
            JS::PrimitiveString::create(vm, "message"_utf16),
            JS::Value(static_cast<double>(delivery_id)),
            JS::PrimitiveString::create(vm, Utf16String::from_utf8(message)),
            JS::PrimitiveString::create(vm, Utf16String::from_utf8(sender)),
        };
        auto status = context->dispatch(arguments);
        if (!status.has_value() || !status->is_string())
            continue;

        // NB: Of what the contexts say, the most is what the process says: a response, a promise of one, or a listener.
        auto status_string = status->as_string().utf16_string().to_well_formed_utf8();
        auto context_status = MessageDeliveryStatus::NoListener;
        if (status_string.starts_with_bytes("r"sv))
            context_status = MessageDeliveryStatus::Responded;
        else if (status_string == "async"sv)
            context_status = MessageDeliveryStatus::WillRespond;
        else if (status_string == "none"sv)
            context_status = MessageDeliveryStatus::NoResponse;

        if (to_underlying(context_status) <= to_underlying(result.status))
            continue;
        result.status = context_status;
        if (context_status == MessageDeliveryStatus::Responded)
            result.response = MUST(String::from_utf8(status_string.bytes_as_string_view().substring_view(1)));
    }
    return result;
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

        // The pages of an extension get its API before any of their own scripts run.
        if (stage == RunAt::DocumentStart && document.url().scheme() == "chrome-extension"sv) {
            // NB: A sandboxed page of the extension has an opaque origin, and no API, as in Chrome.
            auto const* extension = this->extension(document.url().serialized_host());
            if (extension && !document.extension_context(extension->id) && document.origin().is_same_origin(document.url().origin())) {
                auto context = ExtensionContext::create_for_extension_page(document, extension->id);
                document.add_extension_context(context);
                install_api(context, *extension);
            }
        }

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
