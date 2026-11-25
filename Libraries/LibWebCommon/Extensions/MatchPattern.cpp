/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/MatchPattern.h>

namespace Web::Extensions {

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

}
