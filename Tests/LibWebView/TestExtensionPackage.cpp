/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
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
