/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16String.h>
#include <LibRegex/ECMAScriptRegex.h>
#include <LibURL/Parser.h>
#include <LibURL/PublicSuffixData.h>
#include <LibWebView/DeclarativeNetRequest.h>

namespace WebView::DeclarativeNetRequest {

Rule::Rule() = default;
Rule::~Rule() = default;

static constexpr Array resource_type_names {
    "main_frame"sv,
    "sub_frame"sv,
    "stylesheet"sv,
    "script"sv,
    "image"sv,
    "font"sv,
    "object"sv,
    "xmlhttprequest"sv,
    "ping"sv,
    "csp_report"sv,
    "media"sv,
    "websocket"sv,
    "webtransport"sv,
    "webbundle"sv,
    "other"sv,
};

Optional<ResourceType> resource_type_from_name(StringView name)
{
    for (size_t i = 0; i < resource_type_names.size(); ++i) {
        if (resource_type_names[i] == name)
            return static_cast<ResourceType>(i);
    }
    return {};
}

static u32 resource_type_bit(ResourceType type)
{
    return 1u << to_underlying(type);
}

static Optional<ActionType> action_type_from_name(StringView name)
{
    if (name == "block"sv)
        return ActionType::Block;
    if (name == "redirect"sv)
        return ActionType::Redirect;
    if (name == "allow"sv)
        return ActionType::Allow;
    if (name == "upgradeScheme"sv)
        return ActionType::UpgradeScheme;
    if (name == "modifyHeaders"sv)
        return ActionType::ModifyHeaders;
    if (name == "allowAllRequests"sv)
        return ActionType::AllowAllRequests;
    return {};
}

// Characters that are part of a token of a URL, which the index finds rules by.
static bool is_token_character(u32 c)
{
    return is_ascii_lower_alpha(c) || is_ascii_digit(c) || c == '%';
}

// https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest#property-RuleCondition-urlFilter
// '^' matches anything but a letter, a digit, or one of _-.%, and the end of the URL.
static bool is_separator(u32 c)
{
    return !(is_ascii_alphanumeric(c) || c == '_' || c == '-' || c == '.' || c == '%');
}

static ErrorOr<Vector<String>, String> string_list(JsonObject const& object, StringView key, bool lowercase)
{
    Vector<String> list;
    auto value = object.get(key);
    if (!value.has_value())
        return list;
    if (!value->is_array())
        return MUST(String::formatted("{} must be a list", key));
    for (auto const& item : value->as_array().values()) {
        if (!item.is_string() || item.as_string().is_empty())
            return MUST(String::formatted("{} must be a list of non-empty strings", key));
        list.append(lowercase ? item.as_string().to_ascii_lowercase() : item.as_string());
    }
    return list;
}

static ErrorOr<u32, String> resource_type_mask(JsonObject const& condition, StringView key)
{
    u32 mask = 0;
    for (auto const& name : TRY(string_list(condition, key, false))) {
        auto type = resource_type_from_name(name);
        if (!type.has_value())
            return MUST(String::formatted("Unknown resource type '{}'", name));
        mask |= resource_type_bit(*type);
    }
    return mask;
}

static bool is_allowed_redirect_scheme(StringView scheme)
{
    return scheme.is_one_of("http"sv, "https"sv, "chrome-extension"sv, "data"sv);
}

static void compute_longest_literal(UrlFilter& filter)
{
    auto body = filter.body.bytes_as_string_view();
    size_t best_start = 0;
    size_t best_length = 0;
    size_t start = 0;
    for (size_t i = 0; i <= body.length(); ++i) {
        if (i == body.length() || body[i] == '*' || body[i] == '^') {
            if (i - start > best_length) {
                best_start = start;
                best_length = i - start;
            }
            start = i + 1;
        }
    }
    filter.longest_literal_start = best_start;
    filter.longest_literal_length = best_length;
}

static ErrorOr<UrlFilter, String> parse_url_filter(StringView filter, bool is_case_sensitive)
{
    if (filter.is_empty())
        return "urlFilter must not be empty"_string;
    if (!all_of(filter, [](char c) { return is_ascii(c); }))
        return "urlFilter must only contain ASCII characters"_string;

    UrlFilter url_filter;
    url_filter.is_case_sensitive = is_case_sensitive;
    if (filter.starts_with("||"sv)) {
        url_filter.left_anchor = UrlFilter::LeftAnchor::Domain;
        filter = filter.substring_view(2);
        if (filter.starts_with('*'))
            return "urlFilter must not start with ||*"_string;
    } else if (filter.starts_with('|')) {
        url_filter.left_anchor = UrlFilter::LeftAnchor::Start;
        filter = filter.substring_view(1);
    }
    if (filter.ends_with('|')) {
        url_filter.right_anchor = true;
        filter = filter.substring_view(0, filter.length() - 1);
    }
    auto lowercase_filter = filter.to_ascii_lowercase_string();
    url_filter.body = MUST(String::from_utf8(is_case_sensitive ? filter : lowercase_filter.bytes_as_string_view()));
    compute_longest_literal(url_filter);
    return url_filter;
}

static ErrorOr<NonnullOwnPtr<regex::ECMAScriptRegex>, String> compile_regex(StringView source, bool is_case_sensitive)
{
    auto pattern = Utf16String::from_utf8(source);
    auto compiled = regex::ECMAScriptRegex::compile(pattern.utf16_view(), { .ignore_case = !is_case_sensitive });
    if (compiled.is_error())
        return compiled.release_error();
    return make<regex::ECMAScriptRegex>(compiled.release_value());
}

bool is_regex_supported(StringView regex, bool is_case_sensitive)
{
    return !compile_regex(regex, is_case_sensitive).is_error();
}

ErrorOr<Rule, String> parse_rule(JsonObject const& json)
{
    Rule rule;
    rule.json = json;

    auto id = json.get_integer<i64>("id"sv);
    if (!id.has_value() || *id < 1)
        return "Rule ID must be a positive integer"_string;
    rule.id = *id;

    if (json.has("priority"sv)) {
        auto priority = json.get_integer<i64>("priority"sv);
        if (!priority.has_value() || *priority < 1)
            return MUST(String::formatted("Rule with id {} must have a positive priority", rule.id));
        rule.priority = *priority;
    }

    auto action = json.get_object("action"sv);
    auto action_type = action.has_value() ? action_type_from_name(action->get_string("type"sv).value_or({})) : OptionalNone {};
    if (!action_type.has_value())
        return MUST(String::formatted("Rule with id {} has an invalid action", rule.id));
    rule.action = *action_type;

    auto condition = json.get_object("condition"sv).value_or({});
    auto is_case_sensitive = condition.get_bool("isUrlFilterCaseSensitive"sv).value_or(false);

    auto url_filter = condition.get_string("urlFilter"sv);
    auto regex_filter = condition.get_string("regexFilter"sv);
    if (url_filter.has_value() && regex_filter.has_value())
        return MUST(String::formatted("Rule with id {} cannot have both a urlFilter and a regexFilter", rule.id));
    if (url_filter.has_value())
        rule.url_filter = TRY(parse_url_filter(*url_filter, is_case_sensitive));
    if (regex_filter.has_value()) {
        auto compiled = compile_regex(*regex_filter, is_case_sensitive);
        if (compiled.is_error())
            return MUST(String::formatted("Rule with id {} has an unsupported regexFilter", rule.id));
        rule.regex_filter = compiled.release_value();
    }

    if (condition.has("domains"sv) && condition.has("initiatorDomains"sv))
        return MUST(String::formatted("Rule with id {} cannot specify both domains and initiatorDomains", rule.id));
    if (condition.has("excludedDomains"sv) && condition.has("excludedInitiatorDomains"sv))
        return MUST(String::formatted("Rule with id {} cannot specify both excludedDomains and excludedInitiatorDomains", rule.id));

    rule.request_domain_list = TRY(string_list(condition, "requestDomains"sv, true));
    for (auto const& domain : rule.request_domain_list)
        rule.request_domains.add(domain);
    for (auto& domain : TRY(string_list(condition, "excludedRequestDomains"sv, true)))
        rule.excluded_request_domains.add(move(domain));
    rule.initiator_domain_list = TRY(string_list(condition, condition.has("domains"sv) ? "domains"sv : "initiatorDomains"sv, true));
    for (auto const& domain : rule.initiator_domain_list)
        rule.initiator_domains.add(domain);
    for (auto& domain : TRY(string_list(condition, condition.has("excludedDomains"sv) ? "excludedDomains"sv : "excludedInitiatorDomains"sv, true)))
        rule.excluded_initiator_domains.add(move(domain));
    rule.request_methods = TRY(string_list(condition, "requestMethods"sv, true));
    rule.excluded_request_methods = TRY(string_list(condition, "excludedRequestMethods"sv, true));
    if (condition.has("resourceTypes"sv))
        rule.resource_types = TRY(resource_type_mask(condition, "resourceTypes"sv));
    rule.excluded_resource_types = TRY(resource_type_mask(condition, "excludedResourceTypes"sv));

    if (auto domain_type = condition.get_string("domainType"sv); domain_type.has_value()) {
        if (*domain_type == "thirdParty"sv)
            rule.is_third_party = true;
        else if (*domain_type == "firstParty"sv)
            rule.is_third_party = false;
        else
            return MUST(String::formatted("Rule with id {} has an invalid domainType", rule.id));
    }

    if (condition.has("tabIds"sv) || condition.has("excludedTabIds"sv) || condition.has("topDomains"sv) || condition.has("excludedTopDomains"sv) || condition.has("responseHeaders"sv) || condition.has("excludedResponseHeaders"sv))
        rule.has_unsupported_condition = true;

    if (rule.action == ActionType::AllowAllRequests) {
        auto frames = resource_type_bit(ResourceType::MainFrame) | resource_type_bit(ResourceType::SubFrame);
        if (!rule.resource_types.has_value() || (*rule.resource_types & ~frames) != 0 || *rule.resource_types == 0)
            return MUST(String::formatted("Rule with id {} can only allow all requests of frames", rule.id));
    }

    if (rule.action == ActionType::Redirect) {
        auto redirect = action->get_object("redirect"sv);
        if (!redirect.has_value())
            return MUST(String::formatted("Rule with id {} does not say where to redirect", rule.id));
        if (auto path = redirect->get_string("extensionPath"sv); path.has_value() && !path->starts_with('/'))
            return MUST(String::formatted("Rule with id {} has an extensionPath that does not start with /", rule.id));
        if (auto url = redirect->get_string("url"sv); url.has_value()) {
            auto parsed = URL::Parser::basic_parse(*url);
            if (!parsed.has_value() || !is_allowed_redirect_scheme(parsed->scheme()))
                return MUST(String::formatted("Rule with id {} has an invalid redirect url", rule.id));
        }
        if (redirect->has("regexSubstitution"sv) && !rule.regex_filter)
            return MUST(String::formatted("Rule with id {} has a regexSubstitution without a regexFilter", rule.id));
        if (!redirect->has("extensionPath"sv) && !redirect->has("url"sv) && !redirect->has("transform"sv) && !redirect->has("regexSubstitution"sv))
            return MUST(String::formatted("Rule with id {} does not say where to redirect", rule.id));
    }

    return rule;
}

// Matches the body of a URL filter against the URL from the given position on.
static bool match_from(StringView body, size_t body_index, StringView text, size_t text_index, bool right_anchor)
{
    Optional<size_t> star_body_index;
    size_t star_text_index = 0;

    while (true) {
        if (body_index < body.length()) {
            auto c = body[body_index];
            if (c == '*') {
                while (body_index < body.length() && body[body_index] == '*')
                    ++body_index;
                if (body_index == body.length())
                    return true;
                star_body_index = body_index;
                star_text_index = text_index;
                continue;
            }
            if (c == '^') {
                if (text_index == text.length()) {
                    ++body_index;
                    continue;
                }
                if (is_separator(text[text_index])) {
                    ++body_index;
                    ++text_index;
                    continue;
                }
            } else if (text_index < text.length() && text[text_index] == c) {
                ++body_index;
                ++text_index;
                continue;
            }
        } else if (!right_anchor || text_index == text.length()) {
            return true;
        }

        if (star_body_index.has_value() && star_text_index < text.length()) {
            ++star_text_index;
            text_index = star_text_index;
            body_index = *star_body_index;
            continue;
        }
        return false;
    }
}

static Vector<StringView> url_tokens(StringView view)
{
    Vector<StringView> tokens;
    size_t i = 0;
    while (i < view.length()) {
        if (!is_token_character(view[i])) {
            ++i;
            continue;
        }
        auto start = i;
        while (i < view.length() && is_token_character(view[i]))
            ++i;
        tokens.append(view.substring_view(start, i - start));
    }
    return tokens;
}

PreparedRequest::PreparedRequest(Request const& request)
    : request(request)
    , text(request.url.serialize(URL::ExcludeFragment::Yes))
    , lowercase_text(text.to_ascii_lowercase())
    , method(request.method.to_ascii_lowercase())
    , host(request.url.serialized_host().to_ascii_lowercase_string())
    , tokens(url_tokens(lowercase_text.bytes_as_string_view()))
{
    auto view = lowercase_text.bytes_as_string_view();
    auto scheme_length = request.url.scheme().length();
    if (!host.is_empty() && view.length() >= scheme_length + 3 && view.substring_view(scheme_length).starts_with("://"sv)) {
        auto authority_start = scheme_length + 3;
        auto after_scheme = view.substring_view(authority_start);
        auto authority_end = after_scheme.find_any_of("/?#"sv).value_or(after_scheme.length());
        auto authority = after_scheme.substring_view(0, authority_end);
        auto host_offset = authority.find_last('@').map([](size_t at) { return at + 1; }).value_or(0);
        if (authority.substring_view(host_offset).starts_with(host))
            host_start = authority_start + host_offset;
    }
    if (request.initiator.has_value() && !request.initiator->is_opaque()) {
        if (auto serialized = request.initiator->host().serialize(); !serialized.is_empty())
            initiator_host = serialized.to_ascii_lowercase().to_byte_string();
    }
}

static bool url_filter_matches(UrlFilter const& filter, PreparedRequest const& prepared)
{
    auto text = filter.is_case_sensitive ? prepared.text.bytes_as_string_view() : prepared.lowercase_text.bytes_as_string_view();
    auto body = filter.body.bytes_as_string_view();

    if (auto literal = filter.longest_literal(); !literal.is_empty() && !text.contains(literal))
        return false;

    switch (filter.left_anchor) {
    case UrlFilter::LeftAnchor::Start:
        return match_from(body, 0, text, 0, filter.right_anchor);
    case UrlFilter::LeftAnchor::Domain: {
        // The filter starts at the start of the host, or of any of its labels.
        auto const& host = prepared.host;
        if (!prepared.host_start.has_value())
            return false;
        for (size_t i = 0; i < host.length(); ++i) {
            if ((i == 0 || host[i - 1] == '.') && match_from(body, 0, text, *prepared.host_start + i, filter.right_anchor))
                return true;
        }
        return false;
    }
    case UrlFilter::LeftAnchor::None:
        for (size_t i = 0; i <= text.length(); ++i) {
            if (match_from(body, 0, text, i, filter.right_anchor))
                return true;
        }
        return false;
    }
    VERIFY_NOT_REACHED();
}

bool DomainSet::contains_host(StringView host) const
{
    // The host is in the set if it, or what is left of it without some of its first labels, is.
    while (true) {
        if (m_domains.contains(host))
            return true;
        auto dot = host.find('.');
        if (!dot.has_value())
            return false;
        host = host.substring_view(*dot + 1);
    }
}

static String registrable_domain(StringView host)
{
    if (auto domain = URL::PublicSuffixData::find_matching_registrable_domain(host, URL::PublicSuffixData::IncludeStarRule::Yes); domain.has_value())
        return domain.release_value();
    return MUST(String::from_utf8(host));
}

static bool rule_matches(Rule const& rule, PreparedRequest& prepared)
{
    auto const& request = prepared.request;
    if (rule.has_unsupported_condition)
        return false;

    // Without resourceTypes or excludedResourceTypes, a rule is for everything but top-level documents.
    auto type_bit = resource_type_bit(request.type);
    if (rule.resource_types.has_value()) {
        if (!(*rule.resource_types & type_bit))
            return false;
    } else if (rule.excluded_resource_types == 0 && request.type == ResourceType::MainFrame) {
        return false;
    }
    if (rule.excluded_resource_types & type_bit)
        return false;

    if (!rule.request_methods.is_empty() && !rule.request_methods.contains_slow(prepared.method))
        return false;
    if (rule.excluded_request_methods.contains_slow(prepared.method))
        return false;

    if (!rule.request_domains.is_empty() && !rule.request_domains.contains_host(prepared.host))
        return false;
    if (rule.excluded_request_domains.contains_host(prepared.host))
        return false;

    auto const& initiator_host = prepared.initiator_host;
    if (!rule.initiator_domains.is_empty() && (!initiator_host.has_value() || !rule.initiator_domains.contains_host(*initiator_host)))
        return false;
    if (initiator_host.has_value() && rule.excluded_initiator_domains.contains_host(*initiator_host))
        return false;

    // A request without an initiator, such as one the user made, is a first-party one.
    if (rule.is_third_party.has_value()) {
        if (!prepared.is_third_party.has_value())
            prepared.is_third_party = initiator_host.has_value() && registrable_domain(prepared.host) != registrable_domain(*initiator_host);
        if (*prepared.is_third_party != *rule.is_third_party)
            return false;
    }

    if (rule.url_filter.has_value() && !url_filter_matches(*rule.url_filter, prepared))
        return false;
    if (rule.regex_filter) {
        if (!prepared.utf16_text.has_value())
            prepared.utf16_text = Utf16String::from_utf8(prepared.text);
        if (rule.regex_filter->test(prepared.utf16_text->utf16_view(), 0) != regex::MatchResult::Match)
            return false;
    }
    return true;
}

bool rule_matches(Rule const& rule, Request const& request)
{
    PreparedRequest prepared { request };
    return rule_matches(rule, prepared);
}

bool rule_wins_over(Rule const& rule, Rule const* other)
{
    if (!other)
        return true;
    if (rule.priority != other->priority)
        return rule.priority > other->priority;
    return to_underlying(rule.action) < to_underlying(other->action);
}

// The tokens of a URL filter that a URL that matches it has as whole tokens: those bounded on both sides by something
// other than a wildcard, or by an anchor.
static Vector<StringView> safe_tokens(UrlFilter const& filter)
{
    Vector<StringView> tokens;
    // NB: The tokens of a URL are lowercase, which those of a case-sensitive filter might not be.
    if (filter.is_case_sensitive)
        return {};
    auto body = filter.body.bytes_as_string_view();

    size_t i = 0;
    while (i < body.length()) {
        if (!is_token_character(body[i])) {
            ++i;
            continue;
        }
        auto start = i;
        while (i < body.length() && is_token_character(body[i]))
            ++i;

        auto bounded_on_left = start == 0 ? filter.left_anchor != UrlFilter::LeftAnchor::None : body[start - 1] != '*';
        auto bounded_on_right = i == body.length() ? filter.right_anchor : body[i] != '*';
        if (bounded_on_left && bounded_on_right)
            tokens.append(body.substring_view(start, i - start));
    }
    return tokens;
}

RuleIndex::RuleIndex(Vector<Rule> rules)
    : m_rules(move(rules))
{
    // Each rule goes under the least common of its tokens, so that as few rules as possible are checked per token.
    auto candidate_tokens = [](Rule const& rule) -> Vector<Vector<StringView>> {
        if (rule.url_filter.has_value()) {
            auto tokens = safe_tokens(*rule.url_filter);
            if (tokens.is_empty())
                return {};
            return { move(tokens) };
        }
        if (rule.regex_filter || rule.request_domain_list.is_empty())
            return {};
        // A rule for request domains goes under a token of each of them, which the URL of a request to it has.
        Vector<Vector<StringView>> per_domain;
        for (auto const& domain : rule.request_domain_list) {
            auto tokens = url_tokens(domain.bytes_as_string_view());
            if (tokens.is_empty())
                return {};
            per_domain.append(move(tokens));
        }
        return per_domain;
    };

    HashMap<StringView, size_t> token_counts;
    for (auto const& rule : m_rules) {
        m_present_actions |= action_type_mask(rule.action);
        for (auto const& tokens : candidate_tokens(rule)) {
            for (auto token : tokens)
                token_counts.ensure(token, [] { return 0; })++;
        }
    }

    for (size_t index = 0; index < m_rules.size(); ++index) {
        auto groups = candidate_tokens(m_rules[index]);
        if (!groups.is_empty()) {
            for (auto const& tokens : groups) {
                auto best = tokens.first();
                for (auto token : tokens) {
                    if (token_counts.get(token).value_or(0) < token_counts.get(best).value_or(0) || (token_counts.get(token) == token_counts.get(best) && token.length() > best.length()))
                        best = token;
                }
                auto& bucket = m_rules_by_token.ensure(MUST(String::from_utf8(best)), [] { return Vector<size_t> {}; });
                if (bucket.is_empty() || bucket.last() != index)
                    bucket.append(index);
            }
            continue;
        }
        if (!m_rules[index].initiator_domain_list.is_empty()) {
            for (auto const& domain : m_rules[index].initiator_domain_list) {
                auto& bucket = m_rules_by_initiator_domain.ensure(domain, [] { return Vector<size_t> {}; });
                if (bucket.is_empty() || bucket.last() != index)
                    bucket.append(index);
            }
            continue;
        }
        m_unindexed_rules.append(index);
    }
}

Rule const* RuleIndex::best_match(PreparedRequest& prepared, u32 action_mask, HashTable<i64> const* disabled_rule_ids) const
{
    if ((m_present_actions & action_mask) == 0)
        return nullptr;

    auto query = ++m_query_count;
    Rule const* best = nullptr;

    auto consider = [&](size_t index) {
        auto const& rule = m_rules[index];
        if (rule.last_query == query)
            return;
        rule.last_query = query;
        if (disabled_rule_ids && disabled_rule_ids->contains(rule.id))
            return;
        if (!(action_mask & action_type_mask(rule.action)) || !rule_wins_over(rule, best))
            return;
        if (rule_matches(rule, prepared))
            best = &rule;
    };

    for (auto token : prepared.tokens) {
        if (auto rules = m_rules_by_token.get(token); rules.has_value()) {
            for (auto index : *rules)
                consider(index);
        }
    }
    if (prepared.initiator_host.has_value() && !m_rules_by_initiator_domain.is_empty()) {
        StringView host = *prepared.initiator_host;
        while (true) {
            if (auto rules = m_rules_by_initiator_domain.get(host); rules.has_value()) {
                for (auto index : *rules)
                    consider(index);
            }
            auto dot = host.find('.');
            if (!dot.has_value())
                break;
            host = host.substring_view(*dot + 1);
        }
    }
    for (auto index : m_unindexed_rules)
        consider(index);
    return best;
}

Rule const* RuleIndex::best_match(Request const& request, ReadonlySpan<ActionType> actions) const
{
    u32 action_mask = 0;
    for (auto action : actions)
        action_mask |= action_type_mask(action);
    if ((m_present_actions & action_mask) == 0)
        return nullptr;
    PreparedRequest prepared { request };
    return best_match(prepared, action_mask);
}

static Optional<String> transformed_query(Optional<StringView> query, JsonObject const& query_transform)
{
    Vector<String> remove_params;
    if (auto list = query_transform.get_array("removeParams"sv); list.has_value()) {
        for (auto const& name : list->values()) {
            if (name.is_string())
                remove_params.append(URL::percent_encode(name.as_string(), URL::PercentEncodeSet::Component, URL::SpaceAsPlus::Yes));
        }
    }

    struct QueryParameter {
        String name;
        Optional<String> value;
    };
    bool changed = false;
    Vector<QueryParameter> parameters;
    if (query.has_value() && !query->is_empty()) {
        for (auto parameter : query->split_view('&')) {
            auto separator = parameter.find('=');
            auto name = separator.has_value() ? parameter.substring_view(0, *separator) : parameter;
            Optional<String> value;
            if (separator.has_value())
                value = MUST(String::from_utf8(parameter.substring_view(*separator + 1)));
            if (remove_params.contains_slow(name)) {
                changed = true;
                continue;
            }
            parameters.append({ MUST(String::from_utf8(name)), move(value) });
        }
    }

    struct AddOrReplace {
        String key;
        String value;
        bool replace_only { false };
        bool used { false };
    };
    Vector<AddOrReplace> additions;
    if (auto list = query_transform.get_array("addOrReplaceParams"sv); list.has_value()) {
        for (auto const& entry : list->values()) {
            if (!entry.is_object())
                continue;
            auto key = entry.as_object().get_string("key"sv);
            auto value = entry.as_object().get_string("value"sv);
            if (!key.has_value() || !value.has_value())
                continue;
            additions.append({
                URL::percent_encode(*key, URL::PercentEncodeSet::Component, URL::SpaceAsPlus::Yes),
                URL::percent_encode(*value, URL::PercentEncodeSet::Component, URL::SpaceAsPlus::Yes),
                entry.as_object().get_bool("replaceOnly"sv).value_or(false),
                false,
            });
        }
    }
    for (auto& parameter : parameters) {
        for (auto& addition : additions) {
            if (!addition.used && addition.key == parameter.name) {
                parameter.value = addition.value;
                addition.used = true;
                changed = true;
                break;
            }
        }
    }
    for (auto& addition : additions) {
        if (!addition.used && !addition.replace_only) {
            parameters.append({ move(addition.key), move(addition.value) });
            changed = true;
        }
    }

    if (!changed)
        return query.map([](StringView q) { return MUST(String::from_utf8(q)); });
    if (parameters.is_empty())
        return {};
    StringBuilder builder;
    for (auto const& parameter : parameters) {
        if (!builder.is_empty())
            builder.append('&');
        builder.append(parameter.name);
        if (parameter.value.has_value()) {
            builder.append('=');
            builder.append(*parameter.value);
        }
    }
    return MUST(builder.to_string());
}

// https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest#type-URLTransform
static Optional<URL::URL> transformed_url(URL::URL url, JsonObject const& transform)
{
    if (auto scheme = transform.get_string("scheme"sv); scheme.has_value()) {
        if (!scheme->bytes_as_string_view().is_one_of("http"sv, "https"sv))
            return {};
        url.set_scheme(*scheme);
    }
    if (auto host = transform.get_string("host"sv); host.has_value()) {
        auto parsed = URL::Parser::parse_host(*host);
        if (!parsed.has_value())
            return {};
        url.set_host(*parsed);
    }
    if (auto port = transform.get_string("port"sv); port.has_value()) {
        if (port->is_empty()) {
            url.set_port({});
        } else {
            auto parsed_port = port->to_number<u16>();
            if (!parsed_port.has_value())
                return {};
            url.set_port(parsed_port);
        }
    }
    if (auto query = transform.get_string("query"sv); query.has_value())
        url.set_query(query->is_empty() ? Optional<StringView> {} : query->bytes_as_string_view().trim("?"sv, TrimMode::Left));
    else if (auto query_transform = transform.get_object("queryTransform"sv); query_transform.has_value()) {
        auto query = transformed_query(url.query(), *query_transform);
        url.set_query(query.has_value() ? Optional<StringView> { query->bytes_as_string_view() } : OptionalNone {});
    }
    if (auto fragment = transform.get_string("fragment"sv); fragment.has_value())
        url.set_fragment(fragment->is_empty() ? Optional<StringView> {} : fragment->bytes_as_string_view().trim("#"sv, TrimMode::Left));
    if (auto username = transform.get_string("username"sv); username.has_value())
        url.set_username(*username);
    if (auto password = transform.get_string("password"sv); password.has_value())
        url.set_password(*password);

    // NB: Only the path component is replaced; an empty string clears it, and leading '//' cannot replace the host.
    if (auto path = transform.get_string("path"sv); path.has_value()) {
        if (path->is_empty()) {
            Array root_segment { ""sv };
            url.set_path(root_segment);
        } else {
            StringBuilder encoded;
            encoded.append("http://placeholder.invalid"sv);
            if (!path->starts_with('/'))
                encoded.append('/');
            for (auto byte : path->bytes_as_string_view()) {
                if (byte == '?')
                    encoded.append("%3F"sv);
                else if (byte == '#')
                    encoded.append("%23"sv);
                else
                    encoded.append(byte);
            }
            auto reparsed = URL::Parser::basic_parse(encoded.string_view());
            if (!reparsed.has_value())
                return {};
            Vector<StringView> segments;
            for (auto segment : reparsed->path_segments())
                segments.append(segment);
            url.set_path(segments);
        }
    }
    return url;
}

static Optional<URL::URL> substituted_url(Rule const& rule, URL::URL const& request_url, StringView substitution)
{
    auto text = Utf16String::from_utf8(request_url.serialize(URL::ExcludeFragment::Yes));
    if (rule.regex_filter->exec(text.utf16_view(), 0) != regex::MatchResult::Match)
        return {};

    auto match_start = rule.regex_filter->capture_slot(0);
    auto match_end = rule.regex_filter->capture_slot(1);
    if (match_start < 0 || match_end < match_start)
        return {};

    StringBuilder builder;
    builder.append(text.utf16_view().substring_view(0, match_start));
    for (size_t i = 0; i < substitution.length(); ++i) {
        if (substitution[i] == '\\' && i + 1 < substitution.length()) {
            if (substitution[i + 1] == '\\') {
                builder.append('\\');
                ++i;
                continue;
            }
            if (is_ascii_digit(substitution[i + 1])) {
                auto group = substitution[i + 1] - '0';
                ++i;
                if (static_cast<unsigned>(group) > rule.regex_filter->capture_count())
                    continue;
                auto start = rule.regex_filter->capture_slot(group * 2);
                auto end = rule.regex_filter->capture_slot(group * 2 + 1);
                if (start >= 0 && end >= start)
                    builder.append(text.utf16_view().substring_view(start, end - start));
                continue;
            }
        }
        builder.append(substitution[i]);
    }
    builder.append(text.utf16_view().substring_view(match_end));
    return URL::Parser::basic_parse(builder.string_view());
}

Optional<URL::URL> redirect_url(Rule const& rule, URL::URL const& request_url, StringView extension_id)
{
    auto redirect = rule.json.get_object("action"sv)->get_object("redirect"sv);
    if (!redirect.has_value())
        return {};
    if (auto path = redirect->get_string("extensionPath"sv); path.has_value()) {
        if (!path->starts_with('/'))
            return {};
        auto parsed = URL::Parser::basic_parse(ByteString::formatted("chrome-extension://{}{}", extension_id, *path));
        if (!parsed.has_value() || parsed->scheme() != "chrome-extension"sv || parsed->serialized_host() != extension_id)
            return {};
        return parsed;
    }
    Optional<URL::URL> result;
    if (auto url = redirect->get_string("url"sv); url.has_value())
        result = URL::Parser::basic_parse(*url);
    else if (auto transform = redirect->get_object("transform"sv); transform.has_value())
        result = transformed_url(request_url, *transform);
    else if (auto substitution = redirect->get_string("regexSubstitution"sv); substitution.has_value() && rule.regex_filter)
        result = substituted_url(rule, request_url, *substitution);
    if (!result.has_value() || !is_allowed_redirect_scheme(result->scheme()))
        return {};
    return result;
}

}
