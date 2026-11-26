/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibRegex/ECMAScriptRegex.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebView/Export.h>

// https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest
namespace WebView::DeclarativeNetRequest {

enum class ResourceType : u8 {
    MainFrame,
    SubFrame,
    Stylesheet,
    Script,
    Image,
    Font,
    Object,
    XMLHttpRequest,
    Ping,
    CSPReport,
    Media,
    WebSocket,
    WebTransport,
    WebBundle,
    Other,
};
WEBVIEW_API Optional<ResourceType> resource_type_from_name(StringView);

// In the order in which actions of rules of the same priority win over each other.
enum class ActionType : u8 {
    Allow,
    AllowAllRequests,
    Block,
    UpgradeScheme,
    Redirect,
    ModifyHeaders,
};

struct Request {
    URL::URL url;
    String method;
    ResourceType type { ResourceType::Other };
    Optional<URL::Origin> initiator;
    Optional<String> initiator_extension_id {};
};

// Domains, which a host is in if it is one of them or a subdomain of one.
class WEBVIEW_API DomainSet {
public:
    void add(String domain) { m_domains.set(move(domain)); }
    bool is_empty() const { return m_domains.is_empty(); }
    bool contains_host(StringView host) const;

private:
    HashTable<String> m_domains;
};

struct UrlFilter {
    enum class LeftAnchor : u8 {
        None,
        Start,
        Domain,
    };
    LeftAnchor left_anchor { LeftAnchor::None };
    bool right_anchor { false };
    bool is_case_sensitive { false };
    String body;
    size_t longest_literal_start { 0 };
    size_t longest_literal_length { 0 };

    StringView longest_literal() const
    {
        return body.bytes_as_string_view().substring_view(longest_literal_start, longest_literal_length);
    }
};

struct WEBVIEW_API Rule {
    AK_MAKE_NONCOPYABLE(Rule);
    AK_MAKE_DEFAULT_MOVABLE(Rule);

public:
    Rule();
    ~Rule();

    i64 id { 0 };
    i64 priority { 1 };
    ActionType action { ActionType::Block };
    // What the extension gave, which it gets back when it asks for its rules.
    JsonObject json;

    Optional<UrlFilter> url_filter;
    OwnPtr<regex::ECMAScriptRegex> regex_filter;

    // NB: The index finds rules for request or initiator domains by their names, so it keeps them listed too.
    Vector<String> request_domain_list;
    DomainSet request_domains;
    DomainSet excluded_request_domains;
    Vector<String> initiator_domain_list;
    DomainSet initiator_domains;
    DomainSet excluded_initiator_domains;
    Optional<u32> resource_types;
    u32 excluded_resource_types { 0 };
    Vector<String> request_methods;
    Vector<String> excluded_request_methods;
    Optional<bool> is_third_party;
    // A condition that cannot be told here, such as of tabs or response headers, keeps the rule from matching.
    bool has_unsupported_condition { false };

    // Of a match during a query, so that a rule found by several of the request's tokens is checked once.
    mutable u64 last_query { 0 };
};

// What every rule checks of a request, worked out once per request across all extensions and rulesets.
struct WEBVIEW_API PreparedRequest {
    explicit PreparedRequest(Request const&);

    Request const& request;
    String text;
    String lowercase_text;
    String method;
    ByteString host;
    Optional<size_t> host_start;
    Optional<ByteString> initiator_host;
    Vector<StringView> tokens;
    Optional<bool> is_third_party;
    Optional<Utf16String> utf16_text;
};

constexpr u32 action_type_mask(ActionType action)
{
    return 1u << to_underlying(action);
}

// Parses a rule as an extension gives it, or gives an error saying what is wrong with it.
WEBVIEW_API ErrorOr<Rule, String> parse_rule(JsonObject const&);
WEBVIEW_API bool is_regex_supported(StringView regex, bool is_case_sensitive);

WEBVIEW_API bool rule_matches(Rule const&, Request const&);

// The rules of an extension, indexed by the tokens of their URL filters, so that a request is checked against the few
// rules that could match it rather than all of them.
class WEBVIEW_API RuleIndex {
    AK_MAKE_NONCOPYABLE(RuleIndex);
    AK_MAKE_DEFAULT_MOVABLE(RuleIndex);

public:
    RuleIndex() = default;
    explicit RuleIndex(Vector<Rule>);

    size_t size() const { return m_rules.size(); }
    Vector<Rule> const& rules() const { return m_rules; }
    bool has_any_action(u32 action_mask) const { return (m_present_actions & action_mask) != 0; }

    // The rule that wins for the request: the one of the highest priority, and of those, the action that comes first.
    // Only the actions given are considered.
    Rule const* best_match(Request const&, ReadonlySpan<ActionType>) const;
    Rule const* best_match(PreparedRequest&, u32 action_mask, HashTable<i64> const* disabled_rule_ids = nullptr) const;

private:
    Vector<Rule> m_rules;
    HashMap<String, Vector<size_t>> m_rules_by_token;
    HashMap<String, Vector<size_t>> m_rules_by_initiator_domain;
    Vector<size_t> m_unindexed_rules;
    u32 m_present_actions { 0 };
    mutable u64 m_query_count { 0 };
};

// Which rule of several indices wins: the one of the highest priority, and of those, the action that comes first.
WEBVIEW_API bool rule_wins_over(Rule const&, Rule const* other);

// https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest#type-Redirect
WEBVIEW_API Optional<URL::URL> redirect_url(Rule const&, URL::URL const& request_url, StringView extension_id);

}
