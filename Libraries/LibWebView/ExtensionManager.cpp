/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/MimeData.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebView/ExtensionManager.h>

namespace WebView {

static StringView without_byte_order_mark(ReadonlyBytes bytes)
{
    StringView text { bytes };
    if (text.starts_with("\xEF\xBB\xBF"sv))
        return text.substring_view(3);
    return text;
}

static ErrorOr<JsonObject> parse_json_object(ReadonlyBytes bytes)
{
    auto json = TRY(JsonValue::from_string(without_byte_order_mark(bytes)));
    if (!json.is_object())
        return Error::from_string_literal("JSON is not an object");
    return move(json.as_object());
}

// Paths in a manifest are relative to the extension's root, and may start with "/" or "./".
static ByteString normalized_package_path(StringView path)
{
    auto canonical = LexicalPath::canonicalized_path(ByteString::formatted("/{}", path));
    return canonical.substring(1);
}

static Optional<ByteBuffer const&> package_file(ExtensionPackage const& package, StringView path)
{
    auto normalized_path = normalized_package_path(path);
    return package.files.get(normalized_path.view());
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

static JsonArray json_string_array(Vector<String> const& strings)
{
    JsonArray array;
    for (auto const& string : strings)
        array.must_append(string);
    return array;
}

// https://developer.chrome.com/docs/extensions/reference/api/i18n#locales
static JsonObject messages_for_locale(ExtensionPackage const& package, StringView locale)
{
    auto path = ByteString::formatted("_locales/{}/messages.json", locale);
    auto file = package.files.get(path.view());
    if (!file.has_value())
        return {};

    auto messages = parse_json_object(file->bytes());
    if (messages.is_error())
        return {};

    // Message names are case insensitive.
    JsonObject result;
    messages.value().for_each_member([&](String const& name, JsonValue const& message) {
        if (message.is_object())
            result.set(name.to_ascii_lowercase(), message);
    });
    return result;
}

static JsonObject localized_messages(ExtensionPackage const& package, JsonObject const& manifest, ReadonlySpan<String> preferred_languages)
{
    auto default_locale = manifest.get_string("default_locale"sv);
    if (!default_locale.has_value())
        return {};

    // The messages of the default locale are there for whatever the user's locale does not translate.
    auto messages = messages_for_locale(package, *default_locale);

    for (auto const& language : preferred_languages) {
        auto locale = language.to_byte_string().replace("-"sv, "_"sv);
        Vector<ByteString> candidates { locale };
        if (auto separator = locale.find('_'); separator.has_value())
            candidates.append(locale.substring(0, *separator));

        for (auto const& candidate : candidates) {
            auto path = ByteString::formatted("_locales/{}/messages.json", candidate);
            if (!package.files.contains(path.view()))
                continue;
            messages_for_locale(package, candidate).for_each_member([&](String const& name, JsonValue const& message) {
                messages.set(name, message);
            });
            return messages;
        }
    }

    return messages;
}

// Replaces the __MSG_name__ placeholders that manifests and style sheets may contain.
static String localize(StringView text, JsonObject const& messages, StringView extension_id)
{
    StringBuilder builder;
    size_t offset = 0;
    while (offset < text.length()) {
        auto start = text.find("__MSG_"sv, offset);
        if (!start.has_value())
            break;
        auto end = text.find("__"sv, *start + 6);
        if (!end.has_value())
            break;

        builder.append(text.substring_view(offset, *start - offset));
        auto name = text.substring_view(*start + 6, *end - *start - 6);

        if (name == "@@extension_id"sv) {
            builder.append(extension_id);
        } else if (name == "@@bidi_dir"sv) {
            builder.append("ltr"sv);
        } else if (name == "@@bidi_reversed_dir"sv) {
            builder.append("rtl"sv);
        } else if (name == "@@bidi_start_edge"sv) {
            builder.append("left"sv);
        } else if (name == "@@bidi_end_edge"sv) {
            builder.append("right"sv);
        } else if (auto message = messages.get_object(name.to_ascii_lowercase_string()); message.has_value()) {
            builder.append(message->get_string("message"sv).value_or({}));
        }

        offset = *end + 2;
    }
    builder.append(text.substring_view(offset));
    return MUST(builder.to_string());
}

static ErrorOr<String> text_file(ExtensionPackage const& package, StringView path)
{
    auto file = package_file(package, path);
    if (!file.has_value())
        return Error::from_string_view("Extension is missing a file"sv);
    return String::from_utf8_with_replacement_character(without_byte_order_mark(file->bytes()));
}

ErrorOr<JsonObject> extension_data_for_web_content(ExtensionPackage const& package, ReadonlySpan<String> preferred_languages)
{
    auto manifest = TRY(parse_json_object(package_file(package, "manifest.json"sv)->bytes()));

    auto manifest_version = manifest.get_integer<i64>("manifest_version"sv);
    if (manifest_version != 2 && manifest_version != 3)
        return Error::from_string_literal("Extension has an unsupported manifest version");
    if (!manifest.get_string("name"sv).has_value())
        return Error::from_string_literal("Extension has no name");

    auto messages = localized_messages(package, manifest, preferred_languages);

    JsonArray content_scripts;
    if (auto entries = manifest.get_array("content_scripts"sv); entries.has_value()) {
        for (auto const& entry : entries->values()) {
            if (!entry.is_object())
                continue;
            auto const& content_script = entry.as_object();

            auto matches = string_array(content_script, "matches"sv);
            if (matches.is_empty())
                continue;

            JsonArray scripts;
            for (auto const& path : string_array(content_script, "js"sv)) {
                JsonObject script;
                script.set("path"sv, MUST(String::from_byte_string(normalized_package_path(path))));
                script.set("source"sv, TRY(text_file(package, path)));
                scripts.must_append(move(script));
            }

            StringBuilder css;
            for (auto const& path : string_array(content_script, "css"sv)) {
                if (!css.is_empty())
                    css.append('\n');
                css.append(localize(TRY(text_file(package, path)), messages, package.id));
            }

            JsonObject result;
            result.set("matches"sv, json_string_array(matches));
            result.set("exclude_matches"sv, json_string_array(string_array(content_script, "exclude_matches"sv)));
            result.set("include_globs"sv, json_string_array(string_array(content_script, "include_globs"sv)));
            result.set("exclude_globs"sv, json_string_array(string_array(content_script, "exclude_globs"sv)));
            result.set("all_frames"sv, content_script.get_bool("all_frames"sv).value_or(false));
            result.set("match_about_blank"sv, content_script.get_bool("match_about_blank"sv).value_or(false));
            result.set("match_origin_as_fallback"sv, content_script.get_bool("match_origin_as_fallback"sv).value_or(false));
            result.set("run_at"sv, content_script.get_string("run_at"sv).value_or("document_idle"_string));
            result.set("main_world"sv, content_script.get_string("world"sv) == "MAIN"sv);
            result.set("js"sv, move(scripts));
            result.set("css"sv, MUST(css.to_string()));
            content_scripts.must_append(move(result));
        }
    }

    // https://developer.chrome.com/docs/extensions/reference/manifest/web-accessible-resources
    // Manifest V2 lists the files any page can use. Manifest V3 lists them along with the pages that can use them.
    struct WebAccessibleResources {
        Vector<ByteString> resources;
        Vector<String> matches;
    };
    Vector<WebAccessibleResources> web_accessible_resources;
    if (auto entries = manifest.get_array("web_accessible_resources"sv); entries.has_value()) {
        for (auto const& entry : entries->values()) {
            if (entry.is_string()) {
                web_accessible_resources.append({ { normalized_package_path(entry.as_string()) }, {} });
            } else if (entry.is_object()) {
                WebAccessibleResources rule;
                for (auto const& pattern : string_array(entry.as_object(), "resources"sv))
                    rule.resources.append(normalized_package_path(pattern));
                rule.matches = string_array(entry.as_object(), "matches"sv);
                if (rule.matches.is_empty())
                    continue;
                web_accessible_resources.append(move(rule));
            }
        }
    }

    JsonObject resources;
    for (auto const& file : package.files) {
        Optional<Vector<String>> matches;
        for (auto const& rule : web_accessible_resources) {
            if (!any_of(rule.resources, [&](auto const& pattern) { return file.key.bytes_as_string_view().matches(pattern, CaseSensitivity::CaseSensitive); }))
                continue;
            if (rule.matches.is_empty()) {
                matches = Vector<String> {};
                break;
            }
            if (!matches.has_value())
                matches = Vector<String> {};
            matches->extend(rule.matches);
        }
        if (!matches.has_value())
            continue;

        JsonObject resource;
        resource.set("data"sv, TRY(encode_base64(file.value)));
        resource.set("mime_type"sv, MUST(String::from_utf8(Core::guess_mime_type_based_on_filename(file.key))));
        resource.set("matches"sv, json_string_array(*matches));
        resources.set(file.key, move(resource));
    }

    // runtime.getManifest() gives the manifest as the user sees it, with its strings translated.
    for (auto key : { "name"sv, "short_name"sv, "description"sv }) {
        if (auto value = manifest.get_string(key); value.has_value())
            manifest.set(key, localize(*value, messages, package.id));
    }

    JsonObject data;
    data.set("id"sv, package.id);
    data.set("manifest"sv, manifest);
    data.set("messages"sv, move(messages));
    data.set("ui_language"sv, preferred_languages.is_empty() ? "en-US"_string : preferred_languages.first());
    data.set("content_scripts"sv, move(content_scripts));
    data.set("resources"sv, move(resources));

    // Processes ask the browser what to do with the requests of pages if an extension may have rules for them.
    auto permissions = string_array(manifest, "permissions"sv);
    Vector<String> host_permissions;
    if (manifest_version == 3) {
        for (auto const& pattern : string_array(manifest, "host_permissions"sv)) {
            if (Web::Extensions::is_valid_match_pattern(pattern))
                host_permissions.append(pattern);
        }
    } else {
        for (auto const& permission : permissions) {
            if (Web::Extensions::is_valid_match_pattern(permission))
                host_permissions.append(permission);
        }
    }
    data.set("host_permissions"sv, json_string_array(host_permissions));
    data.set("network_rules"sv, permissions.contains_slow("declarativeNetRequest"_string) || permissions.contains_slow("declarativeNetRequestWithHostAccess"_string));
    return data;
}

static void warn_about_unsupported_features(InstalledExtension const& extension, JsonObject const& manifest)
{
    Vector<StringView> unsupported;
    auto permissions = string_array(manifest, "permissions"sv);
    if (permissions.contains_slow("webRequest"_string) || permissions.contains_slow("webRequestBlocking"_string))
        unsupported.append("request interception"sv);

    if (!unsupported.is_empty())
        dbgln("Extension '{}' uses features that are not supported yet: {}", extension.name, MUST(String::join(", "sv, unsupported)));
}

ExtensionManager::ExtensionManager(ByteString extensions_directory, ByteString storage_path)
    : m_extensions_directory(move(extensions_directory))
    , m_storage_path(move(storage_path))
    , m_content_scripts_path(LexicalPath::join(LexicalPath { m_storage_path }.parent().string(), "ExtensionContentScripts.json"sv).string())
    , m_versions_path(LexicalPath::join(LexicalPath { m_storage_path }.parent().string(), "ExtensionVersions.json"sv).string())
    , m_network_rules(LexicalPath::join(LexicalPath { m_storage_path }.parent().string(), "ExtensionNetworkRules.json"sv).string(), [this](StringView extension_id, StringView path) -> Optional<ByteBuffer> {
        auto package = m_packages.get(extension_id);
        if (!package.has_value())
            return {};
        auto normalized_path = normalized_package_path(path);
        auto file = package->files.get(normalized_path.view());
        if (!file.has_value())
            return {};
        return MUST(ByteBuffer::copy(*file));
    })
{
    load_storage();
    load_content_scripts();
    load_known_versions();
}

ExtensionManager::~ExtensionManager()
{
    if (m_storage_save_timer && m_storage_save_timer->is_active())
        save_storage();
}

static ErrorOr<ExtensionPackage> read_package_file(StringView path)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
    auto bytes = TRY(file->read_until_eof());
    return read_extension_package(bytes);
}

static bool manifest_has_scripting_permission(JsonObject const& manifest)
{
    return string_array(manifest, "permissions"sv).contains_slow("scripting"_string);
}

static ErrorOr<Optional<bool>, String> optional_bool_field(JsonObject const& object, StringView key)
{
    auto value = object.get(key);
    if (!value.has_value() || value->is_null())
        return Optional<bool> {};
    if (!value->is_bool())
        return MUST(String::formatted("'{}' must be a boolean", key));
    return value->as_bool();
}

static ErrorOr<Optional<String>, String> optional_string_field(JsonObject const& object, StringView key)
{
    auto value = object.get(key);
    if (!value.has_value() || value->is_null())
        return Optional<String> {};
    if (!value->is_string())
        return MUST(String::formatted("'{}' must be a string", key));
    return Optional<String> { value->as_string() };
}

static ErrorOr<Optional<Vector<String>>, String> optional_string_array_field(JsonObject const& object, StringView key)
{
    auto value = object.get(key);
    if (!value.has_value() || value->is_null())
        return Optional<Vector<String>> {};
    if (!value->is_array())
        return MUST(String::formatted("'{}' must be an array", key));
    Vector<String> result;
    for (auto const& element : value->as_array().values()) {
        if (!element.is_string())
            return MUST(String::formatted("'{}' entries must be strings", key));
        result.append(element.as_string());
    }
    return result;
}

static ErrorOr<void, String> validate_script_id(StringView id)
{
    if (id.is_empty())
        return "Script's ID must not be empty"_string;
    if (id.starts_with('_'))
        return MUST(String::formatted("Script's ID '{}' must not start with '_'", id));
    return {};
}

static bool match_pattern_has_wildcard_path(StringView pattern)
{
    if (pattern == "<all_urls>"sv)
        return true;
    auto scheme_end = pattern.find("://"sv);
    if (!scheme_end.has_value())
        return false;
    auto path_start = pattern.find('/', *scheme_end + 3);
    if (!path_start.has_value())
        return false;
    return pattern.substring_view(*path_start) == "/*"sv;
}

static ErrorOr<void, String> validate_match_patterns(ReadonlySpan<String> patterns, StringView field_name)
{
    for (auto const& pattern : patterns) {
        if (!Web::Extensions::is_valid_match_pattern(pattern))
            return MUST(String::formatted("Invalid match pattern '{}' in '{}'", pattern, field_name));
    }
    return {};
}

static ErrorOr<ByteString, String> validate_script_file_path(ExtensionPackage const& package, StringView raw_path)
{
    if (raw_path.is_empty() || raw_path.contains('\\'))
        return MUST(String::formatted("Invalid file path '{}' for content script", raw_path));
    for (auto part : raw_path.split_view('/')) {
        if (part == ".."sv)
            return MUST(String::formatted("Invalid file path '{}' for content script", raw_path));
    }
    auto normalized = normalized_package_path(raw_path);
    if (normalized.is_empty() || !package.files.contains(normalized.view()))
        return MUST(String::formatted("Could not load file '{}' for content script", raw_path));
    return normalized;
}

static ErrorOr<Vector<String>, String> validate_script_file_paths(ExtensionPackage const& package, ReadonlySpan<String> raw_paths)
{
    Vector<String> paths;
    for (auto const& raw_path : raw_paths)
        paths.append(MUST(String::from_byte_string(TRY(validate_script_file_path(package, raw_path)))));
    return paths;
}

static ErrorOr<JsonObject, String> resolve_content_script_for_web_content(ExtensionPackage const& package, JsonObject const& messages, auto const& script)
{
    JsonArray js_scripts;
    for (auto const& path : script.js) {
        auto source = text_file(package, path);
        if (source.is_error())
            return MUST(String::formatted("Could not load file '{}' for content script", path));
        JsonObject js_entry;
        js_entry.set("path"sv, path);
        js_entry.set("source"sv, source.release_value());
        js_scripts.must_append(move(js_entry));
    }

    StringBuilder css;
    for (auto const& path : script.css) {
        auto source = text_file(package, path);
        if (source.is_error())
            return MUST(String::formatted("Could not load file '{}' for content script", path));
        if (!css.is_empty())
            css.append('\n');
        css.append(localize(source.release_value(), messages, package.id));
    }

    JsonObject result;
    result.set("matches"sv, json_string_array(script.matches));
    result.set("exclude_matches"sv, json_string_array(script.exclude_matches));
    result.set("include_globs"sv, JsonArray {});
    result.set("exclude_globs"sv, JsonArray {});
    result.set("all_frames"sv, script.all_frames);
    result.set("match_about_blank"sv, false);
    result.set("match_origin_as_fallback"sv, script.match_origin_as_fallback);
    result.set("run_at"sv, script.run_at);
    result.set("main_world"sv, script.world == "MAIN"sv);
    result.set("dynamic"sv, true);
    result.set("js"sv, move(js_scripts));
    result.set("css"sv, MUST(css.to_string()));
    return result;
}

static JsonObject serialize_registered_content_script(auto const& script)
{
    JsonObject object;
    object.set("id"sv, script.id);
    object.set("matches"sv, json_string_array(script.matches));
    if (!script.exclude_matches.is_empty())
        object.set("excludeMatches"sv, json_string_array(script.exclude_matches));
    if (!script.js.is_empty())
        object.set("js"sv, json_string_array(script.js));
    if (!script.css.is_empty())
        object.set("css"sv, json_string_array(script.css));
    object.set("allFrames"sv, script.all_frames);
    object.set("matchOriginAsFallback"sv, script.match_origin_as_fallback);
    object.set("persistAcrossSessions"sv, script.persist_across_sessions);
    object.set("runAt"sv, script.run_at);
    object.set("world"sv, script.world);
    return object;
}

template<typename Script>
static ErrorOr<Script, String> parse_registered_content_script(JsonObject const& object, ExtensionPackage const& package, Optional<Script const&> existing)
{
    auto id = TRY(optional_string_field(object, "id"sv));
    if (!id.has_value())
        return "Script must have an ID"_string;
    TRY(validate_script_id(*id));

    Script script = existing.has_value() ? *existing : Script {};
    script.id = move(*id);

    if (auto matches = TRY(optional_string_array_field(object, "matches"sv)); matches.has_value()) {
        if (matches->is_empty())
            return "Script must specify at least one match pattern"_string;
        TRY(validate_match_patterns(*matches, "matches"sv));
        script.matches = move(*matches);
    } else if (!existing.has_value()) {
        return "Script must specify 'matches'"_string;
    }

    if (auto exclude_matches = TRY(optional_string_array_field(object, "excludeMatches"sv)); exclude_matches.has_value()) {
        TRY(validate_match_patterns(*exclude_matches, "excludeMatches"sv));
        script.exclude_matches = move(*exclude_matches);
    }

    if (auto js = TRY(optional_string_array_field(object, "js"sv)); js.has_value())
        script.js = TRY(validate_script_file_paths(package, *js));
    if (auto css = TRY(optional_string_array_field(object, "css"sv)); css.has_value())
        script.css = TRY(validate_script_file_paths(package, *css));

    if (auto all_frames = TRY(optional_bool_field(object, "allFrames"sv)); all_frames.has_value())
        script.all_frames = *all_frames;
    if (auto match_origin_as_fallback = TRY(optional_bool_field(object, "matchOriginAsFallback"sv)); match_origin_as_fallback.has_value())
        script.match_origin_as_fallback = *match_origin_as_fallback;
    if (auto persist_across_sessions = TRY(optional_bool_field(object, "persistAcrossSessions"sv)); persist_across_sessions.has_value())
        script.persist_across_sessions = *persist_across_sessions;

    if (auto run_at = TRY(optional_string_field(object, "runAt"sv)); run_at.has_value()) {
        if (!run_at->bytes_as_string_view().is_one_of("document_start"sv, "document_end"sv, "document_idle"sv))
            return MUST(String::formatted("Invalid runAt value '{}'", *run_at));
        script.run_at = move(*run_at);
    }

    if (auto world = TRY(optional_string_field(object, "world"sv)); world.has_value()) {
        if (!world->bytes_as_string_view().is_one_of("ISOLATED"sv, "MAIN"sv))
            return MUST(String::formatted("Invalid world value '{}'", *world));
        script.world = move(*world);
    }

    if (script.js.is_empty() && script.css.is_empty())
        return "At least a js or css file must be specified"_string;

    if (script.match_origin_as_fallback) {
        for (auto const& pattern : script.matches) {
            if (!match_pattern_has_wildcard_path(pattern))
                return "matchOriginAsFallback can only be specified with paths of '/*'"_string;
        }
    }

    return script;
}

template<typename Script>
static ErrorOr<Core::AnonymousBuffer, String> serialize_extensions_buffer(
    Vector<InstalledExtension> const& extensions,
    HashMap<String, JsonObject> const& base_extension_data,
    HashMap<String, ExtensionPackage> const& packages,
    HashMap<String, JsonObject> const& messages,
    HashMap<String, Vector<Script>> const& registered_content_scripts)
{
    JsonArray extension_data;
    for (auto const& extension : extensions) {
        auto base_data = base_extension_data.get(extension.id);
        if (!base_data.has_value())
            continue;
        JsonObject data = *base_data;
        if (auto registered = registered_content_scripts.get(extension.id); registered.has_value() && !registered->is_empty()) {
            auto package = packages.get(extension.id);
            auto ext_messages = messages.get(extension.id);
            if (package.has_value() && ext_messages.has_value()) {
                JsonArray content_scripts = data.get_array("content_scripts"sv).value_or({});
                for (auto const& script : *registered)
                    content_scripts.must_append(TRY(resolve_content_script_for_web_content(*package, *ext_messages, script)));
                data.set("content_scripts"sv, move(content_scripts));
            }
        }
        extension_data.must_append(move(data));
    }

    JsonObject root;
    root.set("extensions"sv, move(extension_data));
    auto serialized = root.serialized();

    auto buffer = Core::AnonymousBuffer::create_with_size(serialized.bytes().size());
    if (buffer.is_error())
        return MUST(String::formatted("Unable to allocate extension buffer: {}", buffer.error()));
    if (!serialized.bytes().is_empty())
        serialized.bytes().copy_to(Bytes { buffer.value().data<u8>(), buffer.value().size() });
    return buffer.release_value();
}

static JsonObject persisted_entry_for(String const& version, auto const& scripts)
{
    JsonArray persisted_scripts;
    for (auto const& script : scripts) {
        if (script.persist_across_sessions)
            persisted_scripts.must_append(serialize_registered_content_script(script));
    }
    JsonObject entry;
    entry.set("version"sv, version);
    entry.set("scripts"sv, move(persisted_scripts));
    return entry;
}

ErrorOr<void> ExtensionManager::load(ReadonlySpan<String> preferred_languages)
{
    TRY(Core::Directory::create(m_extensions_directory, Core::Directory::CreateDirectories::Yes));

    Vector<ByteString> paths;
    TRY(Core::Directory::for_each_entry(m_extensions_directory, Core::DirIterator::SkipParentAndBaseDir, [&](auto const& entry, auto const&) -> ErrorOr<IterationDecision> {
        auto lowercase_name = entry.name.to_lowercase();
        if (lowercase_name.ends_with(".crx"sv) || lowercase_name.ends_with(".zip"sv))
            paths.append(LexicalPath::join(m_extensions_directory, entry.name).string());
        return IterationDecision::Continue;
    }));
    quick_sort(paths);

    Vector<InstalledExtension> extensions;
    HashMap<String, ExtensionPackage> packages;
    HashMap<String, JsonObject> manifests;
    HashMap<String, JsonObject> messages;
    HashMap<String, JsonObject> base_extension_data;
    HashMap<String, Vector<RegisteredContentScript>> registered_content_scripts;
    JsonObject next_persisted = m_persisted_content_scripts;
    bool persisted_dirty = false;
    HashMap<String, PersistedPendingInstallEvent> next_persisted_pending = m_persisted_pending_install_events;
    bool persisted_pending_dirty = false;
    HashMap<String, InstallEvent> pending_install_events = m_pending_install_events;
    HashTable<String> ids;

    for (auto const& path : paths) {
        Optional<ExtensionPackage> package;
        auto data = [&]() -> ErrorOr<JsonObject> {
            package = TRY(read_package_file(path));
            return extension_data_for_web_content(*package, preferred_languages);
        }();
        if (data.is_error()) {
            warnln("Unable to load the extension {}: {}", path, data.error());
            continue;
        }

        auto const& manifest = *data.value().get_object("manifest"sv);
        InstalledExtension extension {
            .id = *data.value().get_string("id"sv),
            .name = *manifest.get_string("name"sv),
            .version = manifest.get_string("version"sv).value_or({}),
            .path = path,
        };

        if (ids.set(extension.id) != HashSetResult::InsertedNewEntry) {
            warnln("Unable to load the extension {}: another extension has the same ID", path);
            continue;
        }

        warn_about_unsupported_features(extension, manifest);
        dbgln("Loaded the extension '{}' {} ({})", extension.name, extension.version, extension.id);

        Vector<RegisteredContentScript> scripts;
        HashTable<String> script_ids;
        auto has_scripting = manifest_has_scripting_permission(manifest);
        auto stored = next_persisted.get_object(extension.id);
        if (!has_scripting) {
            if (stored.has_value()) {
                next_persisted.remove(extension.id);
                persisted_dirty = true;
            }
        } else {
            if (stored.has_value()) {
                auto stored_version = stored->get_string("version"sv);
                if (!stored_version.has_value() || *stored_version != extension.version) {
                    next_persisted.remove(extension.id);
                    persisted_dirty = true;
                } else if (auto stored_scripts = stored->get_array("scripts"sv); stored_scripts.has_value()) {
                    bool entry_changed = false;
                    for (auto const& value : stored_scripts->values()) {
                        if (!value.is_object()) {
                            entry_changed = true;
                            continue;
                        }
                        auto parsed = parse_registered_content_script<RegisteredContentScript>(value.as_object(), *package, {});
                        if (parsed.is_error() || !parsed.value().persist_across_sessions || script_ids.set(parsed.value().id) != HashSetResult::InsertedNewEntry) {
                            entry_changed = true;
                            continue;
                        }
                        scripts.append(parsed.release_value());
                    }
                    if (entry_changed) {
                        if (scripts.is_empty())
                            next_persisted.remove(extension.id);
                        else
                            next_persisted.set(extension.id, persisted_entry_for(extension.version, scripts));
                        persisted_dirty = true;
                    }
                } else {
                    next_persisted.remove(extension.id);
                    persisted_dirty = true;
                }
            }

            // Session-only dynamic content scripts persist while the browser runs, unless the extension version changed.
            auto previous_version = extension_version(extension.id);
            if (previous_version.has_value() && *previous_version == extension.version) {
                if (auto existing_scripts = m_registered_content_scripts.get(extension.id); existing_scripts.has_value()) {
                    for (auto const& existing_script : *existing_scripts) {
                        if (existing_script.persist_across_sessions)
                            continue;
                        if (script_ids.contains(existing_script.id))
                            continue;
                        auto revalidated = parse_registered_content_script<RegisteredContentScript>(serialize_registered_content_script(existing_script), *package, {});
                        if (revalidated.is_error())
                            continue;
                        script_ids.set(revalidated.value().id);
                        scripts.append(revalidated.release_value());
                    }
                }
            }
        }

        if (!scripts.is_empty())
            registered_content_scripts.set(extension.id, move(scripts));

        Optional<InstallEvent> install_event;
        if (auto existing_pending = m_pending_install_events.get(extension.id); existing_pending.has_value() && extension_version(extension.id) == extension.version) {
            install_event = *existing_pending;
        } else if (auto persisted_pending = m_persisted_pending_install_events.get(extension.id); persisted_pending.has_value()) {
            if (persisted_pending->target_version == extension.version)
                install_event = persisted_pending->event;
            else if (auto known_version = m_known_versions.get(extension.id); known_version.has_value())
                install_event = { "update"_string, *known_version };
            else
                install_event = { "update"_string, persisted_pending->target_version };
        } else if (auto known_version = m_known_versions.get(extension.id); !known_version.has_value()) {
            install_event = { "install"_string, {} };
        } else if (*known_version != extension.version) {
            install_event = { "update"_string, *known_version };
        }
        if (install_event.has_value()) {
            pending_install_events.set(extension.id, *install_event);
            auto existing_persisted = next_persisted_pending.get(extension.id);
            if (!existing_persisted.has_value()
                || existing_persisted->target_version != extension.version
                || existing_persisted->event.reason != install_event->reason
                || existing_persisted->event.previous_version != install_event->previous_version) {
                next_persisted_pending.set(extension.id, { extension.version, *install_event });
                persisted_pending_dirty = true;
            }
        }

        packages.set(extension.id, package.release_value());
        manifests.set(extension.id, manifest);
        messages.set(extension.id, *data.value().get_object("messages"sv));
        base_extension_data.set(extension.id, data.release_value());
        extensions.append(move(extension));
    }

    auto buffer = serialize_extensions_buffer(extensions, base_extension_data, packages, messages, registered_content_scripts);
    if (buffer.is_error())
        return Error::from_string_literal("Unable to serialize extension data");

    if (persisted_dirty) {
        if (auto saved = save_content_scripts(next_persisted); saved.is_error())
            warnln("Unable to save registered content scripts to {}: {}", m_content_scripts_path, saved.error());
    }
    if (persisted_pending_dirty) {
        if (auto saved = save_known_versions(m_known_versions, next_persisted_pending); saved.is_error())
            warnln("Unable to save extension versions to {}: {}", m_versions_path, saved.error());
    }

    m_extensions = move(extensions);
    m_packages = move(packages);
    m_manifests = move(manifests);
    m_messages = move(messages);
    m_base_extension_data = move(base_extension_data);
    m_registered_content_scripts = move(registered_content_scripts);
    m_persisted_content_scripts = move(next_persisted);
    m_persisted_pending_install_events = move(next_persisted_pending);
    m_pending_install_events = move(pending_install_events);

    Vector<String> extension_ids;
    for (auto const& extension : m_extensions)
        extension_ids.append(extension.id);
    m_network_rules.set_extensions(extension_ids, [this](StringView id) -> JsonObject const& { return *m_manifests.get(id); });
    m_extensions_buffer = buffer.release_value();
    return {};
}

ErrorOr<InstalledExtension> ExtensionManager::install(StringView path, ReadonlySpan<String> preferred_languages)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
    auto bytes = TRY(file->read_until_eof());

    // Make sure we can run the extension before it goes anywhere.
    auto package = TRY(read_extension_package(bytes));
    TRY(extension_data_for_web_content(package, preferred_languages));

    // Files are named after the extension, so that packages that happen to have the same name do not replace each other.
    TRY(Core::Directory::create(m_extensions_directory, Core::Directory::CreateDirectories::Yes));
    auto destination = LexicalPath::join(m_extensions_directory, ByteString::formatted("{}.crx", package.id)).string();

    // A new version of an installed extension replaces the old one.
    for (auto const& extension : m_extensions) {
        if (extension.id == package.id && extension.path != destination)
            TRY(FileSystem::remove(extension.path, FileSystem::RecursionMode::Disallowed));
    }

    if (LexicalPath::canonicalized_path(destination) != LexicalPath::canonicalized_path(path)) {
        auto destination_file = TRY(Core::File::open(destination, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        TRY(destination_file->write_until_depleted(bytes));
    }

    auto previous_version = extension_version(package.id);
    if (!previous_version.has_value())
        previous_version = m_known_versions.get(package.id).copy();
    TRY(load(preferred_languages));
    m_network_rules.mark_installed(package.id);

    // Explicitly installing over an existing extension always triggers runtime.onInstalled with reason "update",
    // even if the previous version's first background load had not yet delivered its install event or had the same version.
    if (previous_version.has_value())
        m_pending_install_events.set(package.id, { "update"_string, *previous_version });
    if (auto pending = m_pending_install_events.get(package.id); pending.has_value()) {
        if (auto current_version = extension_version(package.id); current_version.has_value()) {
            auto next_persisted_pending = m_persisted_pending_install_events;
            next_persisted_pending.set(package.id, { *current_version, *pending });
            if (auto saved = save_known_versions(m_known_versions, next_persisted_pending); saved.is_error())
                warnln("Unable to save extension versions to {}: {}", m_versions_path, saved.error());
            m_persisted_pending_install_events = move(next_persisted_pending);
        }
    }

    // Reinstalling or updating an extension clears its previously registered dynamic content scripts.
    if (m_registered_content_scripts.contains(package.id) || m_persisted_content_scripts.has(package.id)) {
        if (auto cleared = commit_content_scripts(package.id, {}); cleared.is_error())
            warnln("Unable to clear registered content scripts for {}: {}", package.id, cleared.error());
    }

    for (auto const& extension : m_extensions) {
        if (extension.id == package.id)
            return extension;
    }
    return Error::from_string_literal("The extension could not be loaded");
}

static constexpr auto GENERATED_BACKGROUND_PAGE = "_generated_background_page.html"sv;

Optional<JsonObject const&> ExtensionManager::manifest(StringView extension_id) const
{
    return m_manifests.get(extension_id);
}

// https://developer.chrome.com/docs/extensions/reference/manifest/background
Optional<URL::URL> ExtensionManager::background_page_url(StringView extension_id) const
{
    auto manifest = this->manifest(extension_id);
    if (!manifest.has_value())
        return {};
    auto background = manifest->get_object("background"sv);
    if (!background.has_value())
        return {};

    // A Manifest V2 background page is the extension's own. Otherwise, there is a page made to run its scripts or service
    // worker, as there are no extension service workers yet.
    auto page = background->get_string("page"sv);
    if (!page.has_value() && !background->has("service_worker"sv) && !background->has("scripts"sv))
        return {};
    return URL::Parser::basic_parse(ByteString::formatted("chrome-extension://{}/{}", extension_id, page.has_value() ? normalized_package_path(*page) : ByteString { GENERATED_BACKGROUND_PAGE }));
}

// https://developer.chrome.com/docs/extensions/reference/api/action
Vector<ExtensionManager::Action> ExtensionManager::actions() const
{
    Vector<Action> actions;
    for (auto const& extension : m_extensions) {
        auto const& manifest = *m_manifests.get(extension.id);
        auto action = manifest.get_object("action"sv);
        if (!action.has_value())
            action = manifest.get_object("browser_action"sv);
        if (!action.has_value())
            action = manifest.get_object("page_action"sv);
        if (!action.has_value())
            continue;

        auto const& package = *m_packages.get(extension.id);

        // The icon is a path, or paths by size, of which the one closest to what a toolbar shows is taken.
        Optional<ByteString> icon_path;
        auto pick_icon = [&](JsonValue const& icons) {
            if (icons.is_string()) {
                icon_path = normalized_package_path(icons.as_string());
                return;
            }
            if (!icons.is_object())
                return;
            Optional<u32> best_size;
            icons.as_object().for_each_member([&](String const& size_string, JsonValue const& path) {
                auto size = size_string.to_number<u32>();
                if (!size.has_value() || !path.is_string())
                    return;
                auto is_better = !best_size.has_value() || (*best_size < 32 ? *size > *best_size : (*size >= 32 && *size < *best_size));
                if (is_better) {
                    best_size = size;
                    icon_path = normalized_package_path(path.as_string());
                }
            });
        };
        if (auto default_icon = action->get("default_icon"sv); default_icon.has_value())
            pick_icon(*default_icon);
        if (!icon_path.has_value()) {
            if (auto icons = manifest.get("icons"sv); icons.has_value())
                pick_icon(*icons);
        }

        Optional<ByteBuffer> icon;
        if (icon_path.has_value()) {
            if (auto file = package.files.get(icon_path->view()); file.has_value())
                icon = MUST(ByteBuffer::copy(*file));
        }

        Optional<URL::URL> popup_url;
        if (auto popup = action->get_string("default_popup"sv); popup.has_value() && !popup->is_empty())
            popup_url = URL::Parser::basic_parse(ByteString::formatted("chrome-extension://{}/{}", extension.id, normalized_package_path(*popup)));

        auto title = action->get_string("default_title"sv);
        actions.append({
            .extension_id = extension.id,
            .title = title.has_value() ? localize(*title, *m_messages.get(extension.id), extension.id) : extension.name,
            .icon = move(icon),
            .popup_url = move(popup_url),
        });
    }
    return actions;
}

static ErrorOr<Core::AnonymousBuffer> buffer_with(ReadonlyBytes bytes)
{
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(bytes.size()));
    if (!bytes.is_empty())
        bytes.copy_to(Bytes { buffer.data<u8>(), buffer.size() });
    return buffer;
}

static String generated_background_page(JsonObject const& background)
{
    StringBuilder builder;
    builder.append("<!DOCTYPE html>\n<meta charset=\"utf-8\">\n"sv);

    auto script_tag = [&](StringView path, bool is_module) {
        builder.appendff("<script{} src=\"/{}\"></script>\n", is_module ? " type=\"module\""sv : ""sv, escape_html_entities(normalized_package_path(path)));
    };
    if (auto service_worker = background.get_string("service_worker"sv); service_worker.has_value())
        script_tag(*service_worker, background.get_string("type"sv) == "module"sv);
    for (auto const& script : string_array(background, "scripts"sv))
        script_tag(script, background.get_string("type"sv) == "module"sv);
    return MUST(builder.to_string());
}

Optional<ExtensionManager::Resource> ExtensionManager::resource(StringView extension_id, StringView path) const
{
    auto package = m_packages.get(extension_id);
    if (!package.has_value())
        return {};
    auto normalized_path = normalized_package_path(path);

    if (normalized_path == GENERATED_BACKGROUND_PAGE) {
        auto background = m_manifests.get(extension_id)->get_object("background"sv);
        if (!background.has_value())
            return {};
        auto page = generated_background_page(*background);
        auto buffer = buffer_with(page.bytes());
        if (buffer.is_error())
            return {};
        return Resource { buffer.release_value(), "text/html"_string };
    }

    auto file = package->files.get(normalized_path.view());
    if (!file.has_value())
        return {};
    auto buffer = buffer_with(*file);
    if (buffer.is_error())
        return {};
    return Resource { buffer.release_value(), MUST(String::from_utf8(Core::guess_mime_type_based_on_filename(path))) };
}

void ExtensionManager::load_storage()
{
    auto file = Core::File::open(m_storage_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return;

    auto bytes = file.value()->read_until_eof();
    if (bytes.is_error())
        return;

    if (auto storage = parse_json_object(bytes.value()); !storage.is_error())
        m_storage = storage.release_value();
    else
        warnln("Unable to read the storage of extensions from {}: {}", m_storage_path, storage.error());
}

void ExtensionManager::schedule_storage_save()
{
    if (!m_storage_save_timer) {
        m_storage_save_timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(1), [this] {
            save_storage();
        });
    }
    if (!m_storage_save_timer->is_active())
        m_storage_save_timer->start();
}

void ExtensionManager::save_storage()
{
    if (m_storage_save_timer)
        m_storage_save_timer->stop();

    auto result = [&]() -> ErrorOr<void> {
        TRY(Core::Directory::create(LexicalPath { m_storage_path }.parent(), Core::Directory::CreateDirectories::Yes));
        auto file = TRY(Core::File::open(m_storage_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        // The session storage area of an extension lasts as long as the browser runs.
        JsonObject persistent_storage;
        m_storage.for_each_member([&](String const& extension_id, JsonValue const& areas) {
            if (!areas.is_object())
                return;
            JsonObject persistent_areas = areas.as_object();
            persistent_areas.remove("session"sv);
            persistent_storage.set(extension_id, move(persistent_areas));
        });
        auto serialized = persistent_storage.serialized();
        TRY(file->write_until_depleted(serialized.bytes()));
        return {};
    }();
    if (result.is_error())
        warnln("Unable to save the storage of extensions to {}: {}", m_storage_path, result.error());
}

void ExtensionManager::apply_storage_changes(StringView extension_id, StringView area, StringView changes_json)
{
    if (area != "local"sv && area != "sync"sv && area != "session"sv)
        return;
    if (!any_of(m_extensions, [&](auto const& extension) { return extension.id == extension_id; }))
        return;

    auto changes = JsonValue::from_string(changes_json);
    if (changes.is_error() || !changes.value().is_object())
        return;
    auto const& changes_object = changes.value().as_object();

    auto extension_storage = m_storage.get_object(extension_id).value_or({});
    auto items = extension_storage.get_object(area).value_or({});

    if (changes_object.get_bool("clear"sv).value_or(false))
        items = {};
    for (auto const& key : string_array(changes_object, "remove"sv))
        items.remove(key);
    if (auto set = changes_object.get_object("set"sv); set.has_value()) {
        set->for_each_member([&](String const& key, JsonValue const& value) {
            items.set(key, value);
        });
    }

    extension_storage.set(area, move(items));
    m_storage.set(extension_id, move(extension_storage));
    if (area != "session"sv)
        schedule_storage_save();
}

Optional<String> ExtensionManager::extension_version(StringView extension_id) const
{
    for (auto const& extension : m_extensions) {
        if (extension.id == extension_id)
            return extension.version;
    }
    return {};
}

Optional<ExtensionManager::InstallEvent> ExtensionManager::pending_install_event(StringView extension_id) const
{
    return m_pending_install_events.get(extension_id).copy();
}

void ExtensionManager::mark_install_event_delivered(StringView extension_id, StringView delivered_version)
{
    auto current_version = extension_version(extension_id);
    if (!current_version.has_value() || *current_version != delivered_version)
        return;
    m_pending_install_events.remove(extension_id);
    bool has_persisted_pending = m_persisted_pending_install_events.contains(extension_id);
    auto known = m_known_versions.get(extension_id);
    if (!has_persisted_pending && known.has_value() && *known == *current_version)
        return;
    auto next_known_versions = m_known_versions;
    next_known_versions.set(MUST(String::from_utf8(extension_id)), *current_version);
    auto next_persisted_pending = m_persisted_pending_install_events;
    next_persisted_pending.remove(extension_id);
    if (auto saved = save_known_versions(next_known_versions, next_persisted_pending); saved.is_error())
        warnln("Unable to save extension versions to {}: {}", m_versions_path, saved.error());
    m_known_versions = move(next_known_versions);
    m_persisted_pending_install_events = move(next_persisted_pending);
}

void ExtensionManager::load_known_versions()
{
    auto file = Core::File::open(m_versions_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return;

    auto bytes = file.value()->read_until_eof();
    if (bytes.is_error())
        return;

    auto stored = parse_json_object(bytes.value());
    if (stored.is_error()) {
        warnln("Unable to read extension versions from {}: {}", m_versions_path, stored.error());
        return;
    }

    stored.value().for_each_member([&](String const& extension_id, JsonValue const& value) {
        if (value.is_string()) {
            m_known_versions.set(extension_id, value.as_string());
            return;
        }
        if (!value.is_object())
            return;
        auto const& entry = value.as_object();
        if (auto version = entry.get_string("version"sv); version.has_value())
            m_known_versions.set(extension_id, *version);
        if (auto pending = entry.get_object("pending"sv); pending.has_value()) {
            auto target_version = pending->get_string("targetVersion"sv);
            auto reason = pending->get_string("reason"sv);
            if (target_version.has_value() && reason.has_value()) {
                m_persisted_pending_install_events.set(extension_id, {
                    .target_version = *target_version,
                    .event = { *reason, pending->get_string("previousVersion"sv).copy() },
                });
            }
        }
    });
}

ErrorOr<void> ExtensionManager::save_known_versions(HashMap<String, String> const& versions, HashMap<String, PersistedPendingInstallEvent> const& pending) const
{
    TRY(Core::Directory::create(LexicalPath { m_versions_path }.parent(), Core::Directory::CreateDirectories::Yes));
    auto temp_path = ByteString::formatted("{}.tmp", m_versions_path);
    JsonObject object;
    HashTable<String> all_ids;
    for (auto const& [extension_id, _] : versions)
        all_ids.set(extension_id);
    for (auto const& [extension_id, _] : pending)
        all_ids.set(extension_id);
    for (auto const& extension_id : all_ids) {
        auto version = versions.get(extension_id);
        auto pending_entry = pending.get(extension_id);
        if (pending_entry.has_value()) {
            JsonObject entry;
            if (version.has_value())
                entry.set("version"sv, *version);
            JsonObject pending_object;
            pending_object.set("targetVersion"sv, pending_entry->target_version);
            pending_object.set("reason"sv, pending_entry->event.reason);
            if (pending_entry->event.previous_version.has_value())
                pending_object.set("previousVersion"sv, *pending_entry->event.previous_version);
            entry.set("pending"sv, move(pending_object));
            object.set(extension_id, move(entry));
        } else if (version.has_value()) {
            object.set(extension_id, *version);
        }
    }
    auto write_result = [&]() -> ErrorOr<void> {
        auto file = TRY(Core::File::open(temp_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        auto serialized = object.serialized();
        TRY(file->write_until_depleted(serialized.bytes()));
        return {};
    }();
    if (write_result.is_error()) {
        (void)FileSystem::remove(temp_path, FileSystem::RecursionMode::Disallowed);
        return write_result.release_error();
    }
    auto rename_result = FileSystem::move_file(m_versions_path, temp_path);
    if (rename_result.is_error()) {
        (void)FileSystem::remove(temp_path, FileSystem::RecursionMode::Disallowed);
        return rename_result.release_error();
    }
    return {};
}

bool ExtensionManager::extension_has_scripting_permission(StringView extension_id) const
{
    auto manifest = m_manifests.get(extension_id);
    return manifest.has_value() && manifest_has_scripting_permission(*manifest);
}

void ExtensionManager::load_content_scripts()
{
    auto file = Core::File::open(m_content_scripts_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return;

    auto bytes = file.value()->read_until_eof();
    if (bytes.is_error())
        return;

    if (auto stored = parse_json_object(bytes.value()); !stored.is_error())
        m_persisted_content_scripts = stored.release_value();
    else
        warnln("Unable to read registered content scripts from {}: {}", m_content_scripts_path, stored.error());
}

ErrorOr<void, String> ExtensionManager::save_content_scripts(JsonObject const& persisted) const
{
    auto result = [&]() -> ErrorOr<void> {
        TRY(Core::Directory::create(LexicalPath { m_content_scripts_path }.parent(), Core::Directory::CreateDirectories::Yes));
        auto temp_path = ByteString::formatted("{}.tmp", m_content_scripts_path);
        auto write_result = [&]() -> ErrorOr<void> {
            auto file = TRY(Core::File::open(temp_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
            auto serialized = persisted.serialized();
            TRY(file->write_until_depleted(serialized.bytes()));
            return {};
        }();
        if (write_result.is_error()) {
            (void)FileSystem::remove(temp_path, FileSystem::RecursionMode::Disallowed);
            return write_result.release_error();
        }
        auto rename_result = FileSystem::move_file(m_content_scripts_path, temp_path);
        if (rename_result.is_error()) {
            (void)FileSystem::remove(temp_path, FileSystem::RecursionMode::Disallowed);
            return rename_result.release_error();
        }
        return {};
    }();
    if (result.is_error())
        return MUST(String::formatted("Unable to save registered content scripts: {}", result.error()));
    return {};
}

ErrorOr<Core::AnonymousBuffer, String> ExtensionManager::build_extensions_buffer(HashMap<String, Vector<RegisteredContentScript>> const& registered_content_scripts) const
{
    return serialize_extensions_buffer(m_extensions, m_base_extension_data, m_packages, m_messages, registered_content_scripts);
}

ErrorOr<void, String> ExtensionManager::commit_content_scripts(StringView extension_id, Vector<RegisteredContentScript> next_scripts)
{
    auto version = extension_version(extension_id);
    if (!version.has_value())
        return "The extension is not available"_string;

    auto id_string = MUST(String::from_utf8(extension_id));

    HashMap<String, Vector<RegisteredContentScript>> next_registered = m_registered_content_scripts;
    if (next_scripts.is_empty())
        next_registered.remove(id_string);
    else
        next_registered.set(id_string, move(next_scripts));

    auto next_buffer = TRY(build_extensions_buffer(next_registered));

    JsonObject next_persisted = m_persisted_content_scripts;
    auto const* scripts_for_extension = next_registered.get(id_string).ptr();
    bool has_persisted = scripts_for_extension && any_of(*scripts_for_extension, [](auto const& script) { return script.persist_across_sessions; });
    if (has_persisted)
        next_persisted.set(id_string, persisted_entry_for(*version, *scripts_for_extension));
    else
        next_persisted.remove(id_string);

    if (next_persisted.serialized() != m_persisted_content_scripts.serialized())
        TRY(save_content_scripts(next_persisted));

    m_registered_content_scripts = move(next_registered);
    m_persisted_content_scripts = move(next_persisted);
    m_extensions_buffer = move(next_buffer);
    return {};
}

ErrorOr<void, String> ExtensionManager::register_content_scripts(StringView extension_id, JsonArray const& scripts)
{
    if (!extension_has_scripting_permission(extension_id))
        return "Extension does not have scripting permission"_string;
    auto package = m_packages.get(extension_id);
    if (!package.has_value())
        return "The extension is not available"_string;

    Vector<RegisteredContentScript> next_scripts;
    HashTable<String> seen_ids;
    if (auto existing = m_registered_content_scripts.get(extension_id); existing.has_value()) {
        for (auto const& script : *existing) {
            seen_ids.set(script.id);
            next_scripts.append(script);
        }
    }

    for (auto const& value : scripts.values()) {
        if (!value.is_object())
            return "Content scripts must be objects"_string;
        auto parsed = TRY(parse_registered_content_script<RegisteredContentScript>(value.as_object(), *package, {}));
        if (seen_ids.set(parsed.id) != HashSetResult::InsertedNewEntry)
            return MUST(String::formatted("Duplicate script ID '{}'", parsed.id));
        next_scripts.append(move(parsed));
    }

    return commit_content_scripts(extension_id, move(next_scripts));
}

ErrorOr<void, String> ExtensionManager::update_content_scripts(StringView extension_id, JsonArray const& scripts)
{
    if (!extension_has_scripting_permission(extension_id))
        return "Extension does not have scripting permission"_string;
    auto package = m_packages.get(extension_id);
    if (!package.has_value())
        return "The extension is not available"_string;

    Vector<RegisteredContentScript> next_scripts;
    if (auto existing = m_registered_content_scripts.get(extension_id); existing.has_value())
        next_scripts = *existing;

    HashTable<String> updated_ids;
    for (auto const& value : scripts.values()) {
        if (!value.is_object())
            return "Content scripts must be objects"_string;
        auto id = TRY(optional_string_field(value.as_object(), "id"sv));
        if (!id.has_value())
            return "Script must have an ID"_string;
        TRY(validate_script_id(*id));
        if (updated_ids.set(*id) != HashSetResult::InsertedNewEntry)
            return MUST(String::formatted("Duplicate script ID '{}'", *id));

        auto index = next_scripts.find_first_index_if([&](auto const& script) { return script.id == *id; });
        if (!index.has_value())
            return MUST(String::formatted("Nonexistent script ID '{}'", *id));

        next_scripts[*index] = TRY(parse_registered_content_script<RegisteredContentScript>(value.as_object(), *package, next_scripts[*index]));
    }

    return commit_content_scripts(extension_id, move(next_scripts));
}

ErrorOr<JsonArray, String> ExtensionManager::registered_content_scripts(StringView extension_id, Optional<JsonObject const&> filter) const
{
    if (!extension_has_scripting_permission(extension_id))
        return "Extension does not have scripting permission"_string;

    Optional<HashTable<String>> filter_ids;
    if (filter.has_value()) {
        if (auto ids = TRY(optional_string_array_field(*filter, "ids"sv)); ids.has_value()) {
            filter_ids = HashTable<String> {};
            for (auto const& id : *ids)
                filter_ids->set(id);
        }
    }

    JsonArray result;
    if (auto existing = m_registered_content_scripts.get(extension_id); existing.has_value()) {
        for (auto const& script : *existing) {
            if (filter_ids.has_value() && !filter_ids->contains(script.id))
                continue;
            result.must_append(serialize_registered_content_script(script));
        }
    }
    return result;
}

ErrorOr<void, String> ExtensionManager::unregister_content_scripts(StringView extension_id, Optional<JsonObject const&> filter)
{
    if (!extension_has_scripting_permission(extension_id))
        return "Extension does not have scripting permission"_string;
    if (!m_packages.contains(extension_id))
        return "The extension is not available"_string;

    Optional<Vector<String>> filter_ids;
    if (filter.has_value())
        filter_ids = TRY(optional_string_array_field(*filter, "ids"sv));

    if (!filter_ids.has_value())
        return commit_content_scripts(extension_id, {});

    Vector<RegisteredContentScript> existing_scripts;
    if (auto existing = m_registered_content_scripts.get(extension_id); existing.has_value())
        existing_scripts = *existing;

    HashTable<String> existing_ids;
    for (auto const& script : existing_scripts)
        existing_ids.set(script.id);

    HashTable<String> to_remove;
    for (auto const& id : *filter_ids) {
        TRY(validate_script_id(id));
        if (!existing_ids.contains(id))
            return MUST(String::formatted("Nonexistent script ID '{}'", id));
        to_remove.set(id);
    }

    Vector<RegisteredContentScript> next_scripts;
    for (auto& script : existing_scripts) {
        if (!to_remove.contains(script.id))
            next_scripts.append(move(script));
    }

    return commit_content_scripts(extension_id, move(next_scripts));
}

}
