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
            result.set("match_about_blank"sv, content_script.get_bool("match_about_blank"sv).value_or(false) || content_script.get_bool("match_origin_as_fallback"sv).value_or(false));
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
    return data;
}

static void warn_about_unsupported_features(InstalledExtension const& extension, JsonObject const& manifest)
{
    Vector<StringView> unsupported;
    if (manifest.has("background"sv))
        unsupported.append("background pages and service workers"sv);
    if (manifest.has("action"sv) || manifest.has("browser_action"sv) || manifest.has("page_action"sv))
        unsupported.append("toolbar buttons and popups"sv);
    if (manifest.has("options_page"sv) || manifest.has("options_ui"sv))
        unsupported.append("options pages"sv);
    if (manifest.has("declarative_net_request"sv))
        unsupported.append("request blocking rules"sv);

    if (!unsupported.is_empty())
        dbgln("Extension '{}' uses features that are not supported, and only its content scripts will run: {}", extension.name, MUST(String::join(", "sv, unsupported)));
}

ExtensionManager::ExtensionManager(ByteString extensions_directory, ByteString storage_path)
    : m_extensions_directory(move(extensions_directory))
    , m_storage_path(move(storage_path))
{
    load_storage();
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
    JsonArray extension_data;
    HashTable<String> ids;

    for (auto const& path : paths) {
        auto data = [&]() -> ErrorOr<JsonObject> {
            auto package = TRY(read_package_file(path));
            return extension_data_for_web_content(package, preferred_languages);
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

        extensions.append(move(extension));
        extension_data.must_append(data.release_value());
    }

    JsonObject root;
    root.set("extensions"sv, move(extension_data));
    auto serialized = root.serialized();

    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(serialized.bytes().size()));
    serialized.bytes().copy_to(Bytes { buffer.data<u8>(), buffer.size() });

    m_extensions = move(extensions);
    m_extensions_buffer = move(buffer);
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

    TRY(load(preferred_languages));

    for (auto const& extension : m_extensions) {
        if (extension.id == package.id)
            return extension;
    }
    return Error::from_string_literal("The extension could not be loaded");
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
        auto serialized = m_storage.serialized();
        TRY(file->write_until_depleted(serialized.bytes()));
        return {};
    }();
    if (result.is_error())
        warnln("Unable to save the storage of extensions to {}: {}", m_storage_path, result.error());
}

void ExtensionManager::apply_storage_changes(StringView extension_id, StringView area, StringView changes_json)
{
    if (area != "local"sv && area != "sync"sv)
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
    schedule_storage_save();
}

}
