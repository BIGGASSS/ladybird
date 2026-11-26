/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibWebView/ExtensionManager.h>
#include <LibWebView/ExtensionPackage.h>

// A ZIP archive with a deflated manifest.json, a directory, and a stored file whose name escapes the archive's root.
static constexpr StringView zip_archive = ""
                                          "\x50\x4b\x03\x04\x14\x00\x00\x00\x08\x00\x00\x00\x21\x00\xe4\x1d\xa2\x3e\x2a\x00\x00\x00\x2f\x00\x00"
                                          "\x00\x0d\x00\x00\x00\x6d\x61\x6e\x69\x66\x65\x73\x74\x2e\x6a\x73\x6f\x6e\xab\x56\xca\x4d\xcc\xcb\x4c"
                                          "\x4b\x2d\x2e\x89\x2f\x4b\x2d\x2a\xce\xcc\xcf\x53\xb2\x32\xd6\x51\xca\x4b\xcc\x4d\x55\xb2\x52\x0a\x51"
                                          "\xd2\x51\x82\x0b\x2b\x19\x2a\xd5\x02\x00\x50\x4b\x03\x04\x14\x00\x00\x00\x00\x00\x00\x00\x21\x00\x00"
                                          "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x64\x69\x72\x2f\x50\x4b\x03\x04\x14\x00"
                                          "\x00\x00\x00\x00\x00\x00\x21\x00\x83\x16\xdc\x8c\x01\x00\x00\x00\x01\x00\x00\x00\x0a\x00\x00\x00\x2e"
                                          "\x2e\x2f\x65\x76\x69\x6c\x2e\x6a\x73\x78\x50\x4b\x01\x02\x14\x03\x14\x00\x00\x00\x08\x00\x00\x00\x21"
                                          "\x00\xe4\x1d\xa2\x3e\x2a\x00\x00\x00\x2f\x00\x00\x00\x0d\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                                          "\x80\x01\x00\x00\x00\x00\x6d\x61\x6e\x69\x66\x65\x73\x74\x2e\x6a\x73\x6f\x6e\x50\x4b\x01\x02\x14\x03"
                                          "\x14\x00\x00\x00\x00\x00\x00\x00\x21\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00"
                                          "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x01\x55\x00\x00\x00\x64\x69\x72\x2f\x50\x4b\x01\x02\x14\x03"
                                          "\x14\x00\x00\x00\x00\x00\x00\x00\x21\x00\x83\x16\xdc\x8c\x01\x00\x00\x00\x01\x00\x00\x00\x0a\x00\x00"
                                          "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x80\x01\x77\x00\x00\x00\x2e\x2e\x2f\x65\x76\x69\x6c\x2e\x6a\x73"
                                          "\x50\x4b\x05\x06\x00\x00\x00\x00\x03\x00\x03\x00\xa5\x00\x00\x00\xa0\x00\x00\x00\x00\x00"
                                          ""sv;

static ByteBuffer crx3(ReadonlyBytes crx_id, ReadonlyBytes archive)
{
    // CrxFileHeader { signed_header_data (10000): SignedData { crx_id (1) } }
    ByteBuffer signed_data;
    signed_data.append(0x0a);
    signed_data.append(static_cast<u8>(crx_id.size()));
    signed_data.append(crx_id);

    ByteBuffer header;
    header.append(Array<u8, 3> { 0x82, 0xf1, 0x04 });
    header.append(static_cast<u8>(signed_data.size()));
    header.append(signed_data);

    ByteBuffer crx;
    crx.append("Cr24"sv.bytes());
    crx.append(Array<u8, 4> { 3, 0, 0, 0 });
    crx.append(Array<u8, 4> { static_cast<u8>(header.size()), 0, 0, 0 });
    crx.append(header);
    crx.append(archive);
    return crx;
}

TEST_CASE(read_zip_archive)
{
    auto files = TRY_OR_FAIL(WebView::read_zip_archive(zip_archive.bytes()));
    EXPECT_EQ(files.size(), 2u);
    EXPECT_EQ(StringView { files.get("manifest.json"sv)->bytes() }, R"({"manifest_version":3,"name":"T","version":"1"})"sv);
    EXPECT_EQ(StringView { files.get("evil.js"sv)->bytes() }, "x"sv);
}

TEST_CASE(read_truncated_zip_archive)
{
    EXPECT(WebView::read_zip_archive(zip_archive.bytes().trim(zip_archive.length() - 1)).is_error());
    EXPECT(WebView::read_zip_archive(zip_archive.bytes().slice(100)).is_error());
}

TEST_CASE(extension_id_from_hash)
{
    Array<u8, 16> hash { 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0xff, 0, 0, 0, 0, 0, 0 };
    EXPECT_EQ(WebView::extension_id_from_hash(hash), "aaabcdefghijklmnopppaaaaaaaaaaaa"sv);
}

TEST_CASE(read_crx3_package)
{
    Array<u8, 16> crx_id { 0xf0, 0x0f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1 };
    auto crx = crx3(crx_id, zip_archive.bytes());
    auto package = TRY_OR_FAIL(WebView::read_extension_package(crx));
    EXPECT_EQ(package.id, "paapaaaaaaaaaaaaaaaaaaaaaaaaaaab"sv);
    EXPECT(package.files.contains("manifest.json"sv));
}

TEST_CASE(read_zip_package_takes_id_from_contents)
{
    // Another archive with the same manifest, whose stored file holds something else.
    auto other_archive = MUST(ByteBuffer::copy(zip_archive.bytes()));
    auto stored_file_offset = StringView { other_archive.bytes() }.find("../evil.jsx"sv);
    VERIFY(stored_file_offset.has_value());
    other_archive[*stored_file_offset + 10] = 'y';

    auto first = TRY_OR_FAIL(WebView::read_extension_package(zip_archive.bytes()));
    auto again = TRY_OR_FAIL(WebView::read_extension_package(zip_archive.bytes()));
    auto other = TRY_OR_FAIL(WebView::read_extension_package(other_archive));
    EXPECT_EQ(first.id.bytes().size(), 32u);
    EXPECT_EQ(first.id, again.id);
    EXPECT_NE(first.id, other.id);
    EXPECT_EQ(StringView { other.files.get("evil.js"sv)->bytes() }, "y"sv);
}

static WebView::ExtensionPackage package_with(Vector<Array<StringView, 2>> files)
{
    WebView::ExtensionPackage package { "abcdefghijklmnopabcdefghijklmnop"_string, {} };
    for (auto const& file : files)
        package.files.set(MUST(String::from_utf8(file[0])), MUST(ByteBuffer::copy(file[1].bytes())));
    return package;
}

TEST_CASE(extension_data_for_web_content)
{
    auto package = package_with({
        { "manifest.json"sv, R"({
            "manifest_version": 3, "name": "__MSG_name__", "version": "1", "default_locale": "en",
            "content_scripts": [{ "matches": ["<all_urls>"], "js": ["/a.js", "./b.js"], "css": ["c.css"], "run_at": "document_start", "world": "MAIN" }],
            "web_accessible_resources": [{ "resources": ["img/*"], "matches": ["<all_urls>"] }]
        })"sv },
        { "_locales/en/messages.json"sv, R"({ "Name": { "message": "English" }, "only_en": { "message": "fallback" } })"sv },
        { "_locales/de/messages.json"sv, R"({ "name": { "message": "Deutsch" } })"sv },
        { "a.js"sv, "var a;"sv },
        { "b.js"sv, "\xEF\xBB\xBFvar b;"sv },
        { "c.css"sv, "a::after { content: '__MSG_@@extension_id__ __MSG_name__'; }"sv },
        { "img/x.png"sv, "png"sv },
        { "secret.txt"sv, "secret"sv },
    });

    Vector<String> languages { "de-DE"_string, "en"_string };
    auto data = TRY_OR_FAIL(WebView::extension_data_for_web_content(package, languages));

    auto const& messages = *data.get_object("messages"sv);
    EXPECT_EQ(messages.get_object("name"sv)->get_string("message"sv), "Deutsch"sv);
    EXPECT_EQ(messages.get_object("only_en"sv)->get_string("message"sv), "fallback"sv);
    EXPECT_EQ(data.get_string("ui_language"sv), "de-DE"sv);

    auto const& content_script = data.get_array("content_scripts"sv)->at(0).as_object();
    EXPECT_EQ(content_script.get_string("run_at"sv), "document_start"sv);
    EXPECT_EQ(content_script.get_bool("main_world"sv), true);
    auto const& scripts = *content_script.get_array("js"sv);
    EXPECT_EQ(scripts.size(), 2u);
    EXPECT_EQ(scripts.at(0).as_object().get_string("path"sv), "a.js"sv);
    EXPECT_EQ(scripts.at(1).as_object().get_string("source"sv), "var b;"sv);
    EXPECT_EQ(content_script.get_string("css"sv), "a::after { content: 'abcdefghijklmnopabcdefghijklmnop Deutsch'; }"sv);

    auto const& resources = *data.get_object("resources"sv);
    EXPECT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources.get_object("img/x.png"sv)->get_string("mime_type"sv), "image/png"sv);
}

TEST_CASE(extension_data_requires_content_script_files)
{
    auto package = package_with({
        { "manifest.json"sv, R"({ "manifest_version": 3, "name": "x", "content_scripts": [{ "matches": ["<all_urls>"], "js": ["missing.js"] }] })"sv },
    });
    EXPECT(WebView::extension_data_for_web_content(package, {}).is_error());
}

TEST_CASE(extension_data_requires_supported_manifest_version)
{
    auto package = package_with({ { "manifest.json"sv, R"({ "manifest_version": 1, "name": "x" })"sv } });
    EXPECT(WebView::extension_data_for_web_content(package, {}).is_error());
}

// A ZIP archive that stores the given files, as the extension manager reads them from the extensions directory.
static ByteBuffer stored_zip(Vector<Array<StringView, 2>> files)
{
    auto append_u16 = [](ByteBuffer& buffer, u16 value) {
        buffer.append(static_cast<u8>(value));
        buffer.append(static_cast<u8>(value >> 8));
    };
    auto append_u32 = [&](ByteBuffer& buffer, u32 value) {
        append_u16(buffer, static_cast<u16>(value));
        append_u16(buffer, static_cast<u16>(value >> 16));
    };

    ByteBuffer archive;
    ByteBuffer central_directory;
    for (auto const& file : files) {
        auto offset = static_cast<u32>(archive.size());
        auto name = file[0];
        auto contents = file[1];

        append_u32(archive, 0x04034b50);
        append_u16(archive, 20);
        append_u16(archive, 0);
        append_u16(archive, 0);
        append_u32(archive, 0);
        append_u32(archive, 0);
        append_u32(archive, contents.length());
        append_u32(archive, contents.length());
        append_u16(archive, name.length());
        append_u16(archive, 0);
        archive.append(name.bytes());
        archive.append(contents.bytes());

        append_u32(central_directory, 0x02014b50);
        append_u16(central_directory, 20);
        append_u16(central_directory, 20);
        append_u16(central_directory, 0);
        append_u16(central_directory, 0);
        append_u32(central_directory, 0);
        append_u32(central_directory, 0);
        append_u32(central_directory, contents.length());
        append_u32(central_directory, contents.length());
        append_u16(central_directory, name.length());
        append_u16(central_directory, 0);
        append_u16(central_directory, 0);
        append_u16(central_directory, 0);
        append_u16(central_directory, 0);
        append_u32(central_directory, 0);
        append_u32(central_directory, offset);
        central_directory.append(name.bytes());
    }

    auto central_directory_offset = static_cast<u32>(archive.size());
    archive.append(central_directory);
    append_u32(archive, 0x06054b50);
    append_u16(archive, 0);
    append_u16(archive, 0);
    append_u16(archive, files.size());
    append_u16(archive, files.size());
    append_u32(archive, central_directory.size());
    append_u32(archive, central_directory_offset);
    append_u16(archive, 0);
    return archive;
}

TEST_CASE(extension_manager_reads_actions_and_background_pages)
{
    Test::TemporaryDirectory directory { "TestExtensionManager"sv };
    auto extensions_directory = directory.path().append("Extensions"sv).string();

    auto with_popup = stored_zip({
        { "manifest.json"sv, R"({ "manifest_version": 3, "name": "__MSG_name__", "default_locale": "en",
            "action": { "default_title": "__MSG_title__", "default_popup": "/popup.html", "default_icon": { "16": "small.png", "48": "large.png", "32": "medium.png" } },
            "background": { "service_worker": "bg.js", "type": "module" } })"sv },
        { "_locales/en/messages.json"sv, R"({ "name": { "message": "With Popup" }, "title": { "message": "Open it" } })"sv },
        { "popup.html"sv, "<p>popup</p>"sv },
        { "bg.js"sv, "var x;"sv },
        { "small.png"sv, "small"sv },
        { "medium.png"sv, "medium"sv },
        { "large.png"sv, "large"sv },
    });
    auto without_popup = stored_zip({
        { "manifest.json"sv, R"({ "manifest_version": 2, "name": "Without Popup", "browser_action": {}, "background": { "page": "bg.html" } })"sv },
        { "bg.html"sv, "<script src=bg.js></script>"sv },
    });

    MUST(Core::Directory::create(extensions_directory, Core::Directory::CreateDirectories::Yes));
    auto write = [&](StringView name, ReadonlyBytes bytes) {
        auto file = MUST(Core::File::open(LexicalPath::join(extensions_directory, name).string(), Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(bytes));
    };
    write("a.zip"sv, with_popup);
    write("b.zip"sv, without_popup);

    WebView::ExtensionManager manager { extensions_directory, directory.path().append("Storage.json"sv).string() };
    TRY_OR_FAIL(manager.load({}));
    EXPECT_EQ(manager.extensions().size(), 2u);

    auto actions = manager.actions();
    EXPECT_EQ(actions.size(), 2u);
    auto const& popup_action = actions[0].title == "Open it"sv ? actions[0] : actions[1];
    auto const& plain_action = actions[0].title == "Open it"sv ? actions[1] : actions[0];

    EXPECT_EQ(popup_action.title, "Open it"sv);
    EXPECT_EQ(StringView { popup_action.icon->bytes() }, "medium"sv);
    EXPECT_EQ(popup_action.popup_url->serialize(), MUST(String::formatted("chrome-extension://{}/popup.html", popup_action.extension_id)));
    EXPECT_EQ(plain_action.title, "Without Popup"sv);
    EXPECT(!plain_action.icon.has_value());
    EXPECT(!plain_action.popup_url.has_value());

    auto background_page_url = manager.background_page_url(popup_action.extension_id);
    EXPECT_EQ(background_page_url->serialize(), MUST(String::formatted("chrome-extension://{}/_generated_background_page.html", popup_action.extension_id)));
    auto background_page = manager.resource(popup_action.extension_id, "/_generated_background_page.html"sv);
    EXPECT_EQ(background_page->mime_type, "text/html"sv);
    EXPECT(StringView { background_page->data.bytes() }.contains("<script type=\"module\" src=\"/bg.js\"></script>"sv));

    EXPECT_EQ(manager.background_page_url(plain_action.extension_id)->serialize(), MUST(String::formatted("chrome-extension://{}/bg.html", plain_action.extension_id)));

    auto popup = manager.resource(popup_action.extension_id, "popup.html"sv);
    EXPECT_EQ(StringView { popup->data.bytes() }, "<p>popup</p>"sv);
    EXPECT_EQ(popup->mime_type, "text/html"sv);
    EXPECT(!manager.resource(popup_action.extension_id, "missing.js"sv).has_value());
    EXPECT(!manager.resource("unknown"sv, "popup.html"sv).has_value());

    EXPECT_EQ(manager.manifest(popup_action.extension_id)->get_string("name"sv), "With Popup"sv);
}

TEST_CASE(extension_manager_registered_content_scripts)
{
    Test::TemporaryDirectory directory { "TestExtensionScripting"sv };
    auto extensions_directory = directory.path().append("Extensions"sv).string();
    auto storage_path = directory.path().append("Storage.json"sv).string();

    auto with_scripting = stored_zip({
        { "manifest.json"sv, R"({
            "manifest_version": 3,
            "name": "Cosmetic Blocker",
            "version": "1.0",
            "default_locale": "en",
            "permissions": ["scripting"],
            "host_permissions": ["https://allowed.test/*"]
        })"sv },
        { "_locales/en/messages.json"sv, R"({ "tag": { "message": "hidden" } })"sv },
        { "cosmetic.css"sv, ".ad { display: none; content: '__MSG_tag__'; }"sv },
        { "inject.js"sv, "window.__injected = 1;"sv },
        { "extra.js"sv, "window.__extra = 2;"sv },
    });
    auto without_scripting = stored_zip({
        { "manifest.json"sv, R"({
            "manifest_version": 3,
            "name": "No Scripting",
            "version": "1.0",
            "host_permissions": ["<all_urls>"]
        })"sv },
        { "inject.js"sv, "window.__nope = 1;"sv },
    });

    MUST(Core::Directory::create(extensions_directory, Core::Directory::CreateDirectories::Yes));
    auto write = [&](StringView name, ReadonlyBytes bytes) {
        auto file = MUST(Core::File::open(LexicalPath::join(extensions_directory, name).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(file->write_until_depleted(bytes));
    };
    write("a.zip"sv, with_scripting);
    write("b.zip"sv, without_scripting);

    WebView::ExtensionManager manager { extensions_directory, storage_path };
    TRY_OR_FAIL(manager.load({}));

    auto scripting_id = manager.extensions()[0].name == "Cosmetic Blocker"sv ? manager.extensions()[0].id : manager.extensions()[1].id;
    auto no_scripting_id = manager.extensions()[0].name == "Cosmetic Blocker"sv ? manager.extensions()[1].id : manager.extensions()[0].id;

    EXPECT(manager.extension_has_scripting_permission(scripting_id));
    EXPECT(!manager.extension_has_scripting_permission(no_scripting_id));

    auto parse_array = [](StringView json) -> JsonArray {
        return MUST(JsonValue::from_string(json)).as_array();
    };
    auto parse_object = [](StringView json) -> JsonObject {
        return MUST(JsonValue::from_string(json)).as_object();
    };

    // Permission gating.
    EXPECT(manager.register_content_scripts(no_scripting_id, parse_array(R"([{"id":"s1","matches":["<all_urls>"],"js":["inject.js"]}])"sv)).is_error());
    EXPECT(manager.registered_content_scripts(no_scripting_id, {}).is_error());

    // Validation errors.
    EXPECT(manager.register_content_scripts(scripting_id, parse_array(R"([{"id":"_reserved","matches":["<all_urls>"],"css":["cosmetic.css"]}])"sv)).is_error());
    EXPECT(manager.register_content_scripts(scripting_id, parse_array(R"([{"id":"no-files","matches":["<all_urls>"]}])"sv)).is_error());
    EXPECT(manager.register_content_scripts(scripting_id, parse_array(R"([{"id":"missing-file","matches":["<all_urls>"],"css":["missing.css"]}])"sv)).is_error());
    EXPECT(manager.register_content_scripts(scripting_id, parse_array(R"([{"id":"bad-pattern","matches":["chrome-extension://*/*"],"css":["cosmetic.css"]}])"sv)).is_error());
    EXPECT(manager.register_content_scripts(scripting_id, parse_array(R"([{"id":"bad-fallback","matches":["https://allowed.test/page"],"css":["cosmetic.css"],"matchOriginAsFallback":true}])"sv)).is_error());
    EXPECT(manager.register_content_scripts(scripting_id, parse_array(R"([{"id":"bad-world","matches":["<all_urls>"],"js":["inject.js"],"world":"OTHER"}])"sv)).is_error());
    EXPECT(manager.register_content_scripts(scripting_id, parse_array(R"([{"id":"dup","matches":["<all_urls>"],"js":["inject.js"]},{"id":"dup","matches":["<all_urls>"],"css":["cosmetic.css"]}])"sv)).is_error());

    // Register one persisted script and one session-only script.
    auto registered = manager.register_content_scripts(scripting_id, parse_array(R"([
        {"id":"cosmetic","matches":["<all_urls>"],"css":["/cosmetic.css"],"runAt":"document_start","allFrames":true},
        {"id":"session-only","matches":["https://allowed.test/*"],"js":["./inject.js"],"world":"MAIN","persistAcrossSessions":false}
    ])"sv));
    EXPECT(!registered.is_error());

    // Duplicate ID across calls fails atomically.
    EXPECT(manager.register_content_scripts(scripting_id, parse_array(R"([
        {"id":"brand-new","matches":["<all_urls>"],"js":["extra.js"]},
        {"id":"cosmetic","matches":["<all_urls>"],"js":["inject.js"]}
    ])"sv)).is_error());

    auto listed = manager.registered_content_scripts(scripting_id, {});
    EXPECT(!listed.is_error());
    EXPECT_EQ(listed.value().size(), 2u);
    EXPECT_EQ(listed.value().at(0).as_object().get_string("id"sv), "cosmetic"sv);
    EXPECT_EQ(listed.value().at(0).as_object().get_array("css"sv)->at(0).as_string(), "cosmetic.css"sv);
    EXPECT_EQ(listed.value().at(0).as_object().get_bool("persistAcrossSessions"sv), true);
    EXPECT_EQ(listed.value().at(1).as_object().get_string("id"sv), "session-only"sv);
    EXPECT_EQ(listed.value().at(1).as_object().get_string("world"sv), "MAIN"sv);
    EXPECT_EQ(listed.value().at(1).as_object().get_bool("persistAcrossSessions"sv), false);

    // Filtered listing.
    auto filtered = manager.registered_content_scripts(scripting_id, parse_object(R"({"ids":["session-only","unknown"]})"sv));
    EXPECT(!filtered.is_error());
    EXPECT_EQ(filtered.value().size(), 1u);
    EXPECT_EQ(filtered.value().at(0).as_object().get_string("id"sv), "session-only"sv);

    auto empty_filtered = manager.registered_content_scripts(scripting_id, parse_object(R"({"ids":[]})"sv));
    EXPECT(!empty_filtered.is_error());
    EXPECT_EQ(empty_filtered.value().size(), 0u);

    // Verify extensions_buffer includes host_permissions, localized dynamic CSS, and dynamic flag.
    auto buffer_json = MUST(JsonValue::from_string(StringView { manager.extensions_buffer().bytes() })).as_object();
    auto const& ext_array = *buffer_json.get_array("extensions"sv);
    bool checked_buffer = false;
    for (auto const& ext_value : ext_array.values()) {
        auto const& ext_obj = ext_value.as_object();
        if (ext_obj.get_string("id"sv) != scripting_id)
            continue;
        checked_buffer = true;
        EXPECT_EQ(ext_obj.get_array("host_permissions"sv)->size(), 1u);
        EXPECT_EQ(ext_obj.get_array("host_permissions"sv)->at(0).as_string(), "https://allowed.test/*"sv);
        auto const& scripts = *ext_obj.get_array("content_scripts"sv);
        EXPECT_EQ(scripts.size(), 2u);
        EXPECT_EQ(scripts.at(0).as_object().get_bool("dynamic"sv), true);
        EXPECT_EQ(scripts.at(0).as_object().get_string("css"sv), ".ad { display: none; content: 'hidden'; }"sv);
        EXPECT_EQ(scripts.at(1).as_object().get_bool("main_world"sv), true);
    }
    EXPECT(checked_buffer);

    // Update existing script preserves unspecified fields and rejects unknown IDs.
    EXPECT(manager.update_content_scripts(scripting_id, parse_array(R"([{"id":"nope","js":["extra.js"]}])"sv)).is_error());
    EXPECT(!manager.update_content_scripts(scripting_id, parse_array(R"([{"id":"cosmetic","js":["extra.js"],"excludeMatches":["https://allowed.test/skip*"]}])"sv)).is_error());
    auto after_update = manager.registered_content_scripts(scripting_id, parse_object(R"({"ids":["cosmetic"]})"sv)).release_value();
    EXPECT_EQ(after_update.at(0).as_object().get_array("css"sv)->at(0).as_string(), "cosmetic.css"sv);
    EXPECT_EQ(after_update.at(0).as_object().get_array("js"sv)->at(0).as_string(), "extra.js"sv);
    EXPECT_EQ(after_update.at(0).as_object().get_array("excludeMatches"sv)->at(0).as_string(), "https://allowed.test/skip*"sv);

    // Reload in a fresh ExtensionManager: persisted script survives, session-only script does not.
    {
        WebView::ExtensionManager reloaded { extensions_directory, storage_path };
        TRY_OR_FAIL(reloaded.load({}));
        auto persisted = reloaded.registered_content_scripts(scripting_id, {}).release_value();
        EXPECT_EQ(persisted.size(), 1u);
        EXPECT_EQ(persisted.at(0).as_object().get_string("id"sv), "cosmetic"sv);
    }

    // Unregister rejects nonexistent IDs, treats empty ids array as no-op, and clears all when filter omitted.
    EXPECT(manager.unregister_content_scripts(scripting_id, parse_object(R"({"ids":["nonexistent"]})"sv)).is_error());
    EXPECT(!manager.unregister_content_scripts(scripting_id, parse_object(R"({"ids":[]})"sv)).is_error());
    EXPECT_EQ(manager.registered_content_scripts(scripting_id, {}).release_value().size(), 2u);

    EXPECT(!manager.unregister_content_scripts(scripting_id, parse_object(R"({"ids":["session-only"]})"sv)).is_error());
    EXPECT_EQ(manager.registered_content_scripts(scripting_id, {}).release_value().size(), 1u);

    EXPECT(!manager.unregister_content_scripts(scripting_id, {}).is_error());
    EXPECT_EQ(manager.registered_content_scripts(scripting_id, {}).release_value().size(), 0u);

    // Version upgrade clears persisted dynamic scripts (and persists the reset).
    auto upgraded = stored_zip({
        { "manifest.json"sv, R"({
            "manifest_version": 3,
            "name": "Cosmetic Blocker",
            "version": "2.0",
            "permissions": ["scripting"],
            "host_permissions": ["https://allowed.test/*"]
        })"sv },
        { "cosmetic.css"sv, ".ad { display: none; }"sv },
    });
    // Use CRX3 with fixed ID so the extension ID stays identical across version upgrades.
    Array<u8, 16> fixed_id { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    auto v1_crx = crx3(fixed_id, with_scripting);
    auto v2_crx = crx3(fixed_id, upgraded);
    Test::TemporaryDirectory upgrade_dir { "TestExtensionScriptingUpgrade"sv };
    auto up_ext_dir = upgrade_dir.path().append("Extensions"sv).string();
    auto up_storage = upgrade_dir.path().append("Storage.json"sv).string();
    MUST(Core::Directory::create(up_ext_dir, Core::Directory::CreateDirectories::Yes));
    auto write_up = [&](ReadonlyBytes bytes) {
        auto file = MUST(Core::File::open(LexicalPath::join(up_ext_dir, "ext.crx"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(file->write_until_depleted(bytes));
    };
    write_up(v1_crx);
    {
        WebView::ExtensionManager up_manager { up_ext_dir, up_storage };
        TRY_OR_FAIL(up_manager.load({}));
        auto id = up_manager.extensions()[0].id;
        EXPECT(!up_manager.register_content_scripts(id, parse_array(R"([{"id":"v1","matches":["<all_urls>"],"css":["cosmetic.css"]}])"sv)).is_error());
    }
    write_up(v2_crx);
    {
        WebView::ExtensionManager up_manager { up_ext_dir, up_storage };
        TRY_OR_FAIL(up_manager.load({}));
        auto id = up_manager.extensions()[0].id;
        EXPECT_EQ(up_manager.registered_content_scripts(id, {}).release_value().size(), 0u);
    }
    // Reinstalling v1 after v2 should NOT resurrect stale v1 scripts.
    write_up(v1_crx);
    {
        WebView::ExtensionManager up_manager { up_ext_dir, up_storage };
        TRY_OR_FAIL(up_manager.load({}));
        auto id = up_manager.extensions()[0].id;
        EXPECT_EQ(up_manager.registered_content_scripts(id, {}).release_value().size(), 0u);
    }
}
