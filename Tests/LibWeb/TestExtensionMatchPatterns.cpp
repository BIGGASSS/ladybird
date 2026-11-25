/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/Extensions/ExtensionRegistry.h>

static bool matches(StringView pattern, StringView url_string)
{
    auto url = URL::Parser::basic_parse(url_string);
    VERIFY(url.has_value());
    return Web::Extensions::match_pattern_matches(pattern, *url);
}

TEST_CASE(all_urls)
{
    EXPECT(matches("<all_urls>"sv, "https://example.com/"sv));
    EXPECT(matches("<all_urls>"sv, "file:///tmp/a.html"sv));
    EXPECT(!matches("<all_urls>"sv, "about:blank"sv));
    EXPECT(!matches("<all_urls>"sv, "chrome-extension://abc/x.html"sv));
}

TEST_CASE(scheme)
{
    EXPECT(matches("*://example.com/*"sv, "http://example.com/"sv));
    EXPECT(matches("*://example.com/*"sv, "https://example.com/a"sv));
    EXPECT(!matches("*://example.com/*"sv, "ftp://example.com/a"sv));
    EXPECT(!matches("https://example.com/*"sv, "http://example.com/a"sv));
    EXPECT(matches("file:///tmp/*"sv, "file:///tmp/a.html"sv));
}

TEST_CASE(host)
{
    EXPECT(matches("https://*/*"sv, "https://anything.test/"sv));
    EXPECT(matches("https://*.example.com/*"sv, "https://example.com/"sv));
    EXPECT(matches("https://*.example.com/*"sv, "https://a.b.example.com/"sv));
    EXPECT(!matches("https://*.example.com/*"sv, "https://notexample.com/"sv));
    EXPECT(!matches("https://example.com/*"sv, "https://www.example.com/"sv));
    EXPECT(matches("https://EXAMPLE.com/*"sv, "https://example.com/"sv));
}

TEST_CASE(port)
{
    EXPECT(matches("http://localhost/*"sv, "http://localhost:8080/"sv));
    EXPECT(matches("http://localhost:8080/*"sv, "http://localhost:8080/"sv));
    EXPECT(!matches("http://localhost:8080/*"sv, "http://localhost:9090/"sv));
    EXPECT(matches("http://localhost:*/*"sv, "http://localhost:9090/"sv));
}

TEST_CASE(path_and_query)
{
    EXPECT(matches("https://example.com/foo*"sv, "https://example.com/foobar"sv));
    EXPECT(!matches("https://example.com/foo"sv, "https://example.com/foobar"sv));
    EXPECT(matches("https://example.com/*?q=*"sv, "https://example.com/search?q=x"sv));
    EXPECT(!matches("https://example.com/Foo*"sv, "https://example.com/foo"sv));
}

TEST_CASE(invalid_patterns)
{
    EXPECT(!matches("example.com"sv, "https://example.com/"sv));
    EXPECT(!matches("https://example.com"sv, "https://example.com/"sv));
}

TEST_CASE(globs)
{
    EXPECT(Web::Extensions::glob_matches("https://*.example.com/*"sv, "https://a.example.com/x"sv));
    EXPECT(Web::Extensions::glob_matches("*/fo?/*"sv, "https://a/foo/b"sv));
    EXPECT(!Web::Extensions::glob_matches("*/foo"sv, "https://a/foo/b"sv));
}
