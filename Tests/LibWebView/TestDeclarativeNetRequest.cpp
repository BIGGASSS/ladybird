/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <LibCore/Directory.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/DeclarativeNetRequest.h>
#include <LibWebView/ExtensionNetworkRules.h>

using namespace WebView::DeclarativeNetRequest;
using Web::Extensions::NetworkRequestOutcome;

static URL::URL url(StringView string)
{
    auto result = URL::Parser::basic_parse(string);
    VERIFY(result.has_value());
    return result.release_value();
}

static Rule rule(StringView json)
{
    auto object = MUST(JsonValue::from_string(json));
    auto result = parse_rule(object.as_object());
    if (result.is_error())
        FAIL(result.error());
    return result.release_value();
}

static Request request(StringView request_url, ResourceType type = ResourceType::Script, Optional<StringView> initiator = {}, StringView method = "get"sv)
{
    Request request { url(request_url), MUST(String::from_utf8(method)), type, {} };
    if (initiator.has_value())
        request.initiator = url(*initiator).origin();
    return request;
}

static bool filter_matches(StringView filter, StringView request_url)
{
    auto json = MUST(String::formatted(R"({{ "id": 1, "action": {{ "type": "block" }}, "condition": {{ "urlFilter": "{}" }} }})", filter));
    return rule_matches(rule(json), request(request_url));
}

TEST_CASE(parse_rule_errors)
{
    auto parse = [](StringView json) { return parse_rule(MUST(JsonValue::from_string(json)).as_object()); };
    EXPECT(parse(R"({ "id": 0, "action": { "type": "block" } })"sv).is_error());
    EXPECT(parse(R"({ "id": 1, "action": { "type": "nope" } })"sv).is_error());
    EXPECT(parse(R"({ "id": 1, "priority": 0, "action": { "type": "block" } })"sv).is_error());
    EXPECT(parse(R"({ "id": 1, "action": { "type": "block" }, "condition": { "urlFilter": "a", "regexFilter": "b" } })"sv).is_error());
    EXPECT(parse(R"({ "id": 1, "action": { "type": "block" }, "condition": { "urlFilter": "||*a" } })"sv).is_error());
    EXPECT(parse(R"({ "id": 1, "action": { "type": "block" }, "condition": { "resourceTypes": ["bogus"] } })"sv).is_error());
    EXPECT(parse(R"({ "id": 1, "action": { "type": "allowAllRequests" }, "condition": { "resourceTypes": ["script"] } })"sv).is_error());
    EXPECT(parse(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "extensionPath": "noslash.js" } } })"sv).is_error());
    EXPECT(parse(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "url": "file:///etc/passwd" } } })"sv).is_error());
    EXPECT(parse(R"json({ "id": 1, "action": { "type": "redirect", "redirect": { "url": "javascript:alert(1)" } } })json"sv).is_error());
    EXPECT(parse(R"({ "id": 1, "action": { "type": "redirect", "redirect": {} } })"sv).is_error());
    EXPECT(!parse(R"({ "id": 1, "action": { "type": "allowAllRequests" }, "condition": { "resourceTypes": ["main_frame"] } })"sv).is_error());
}

TEST_CASE(url_filter_syntax)
{
    EXPECT(filter_matches("||example.com^"sv, "https://example.com/ads.js"sv));
    EXPECT(filter_matches("||example.com^"sv, "https://cdn.example.com/x"sv));
    EXPECT(!filter_matches("||example.com^"sv, "https://notexample.com/x"sv));
    EXPECT(!filter_matches("||example.com^"sv, "https://example.community/x"sv));
    EXPECT(!filter_matches("||example.com^"sv, "https://example.com@other.test/x"sv));
    EXPECT(filter_matches("||other.test^"sv, "https://example.com@other.test/x"sv));
    EXPECT(filter_matches("||example.com"sv, "https://example.com"sv));
    EXPECT(filter_matches("|https://a.test/"sv, "https://a.test/x"sv));
    EXPECT(!filter_matches("|a.test/"sv, "https://a.test/x"sv));
    EXPECT(filter_matches("/ads/*.js|"sv, "https://a.test/ads/banner.js"sv));
    EXPECT(!filter_matches("/ads/*.js|"sv, "https://a.test/ads/banner.js?x"sv));
    EXPECT(filter_matches("ADS"sv, "https://a.test/ads"sv));
    EXPECT(filter_matches("banner^"sv, "https://a.test/banner"sv));
    EXPECT(filter_matches("banner^"sv, "https://a.test/banner?x"sv));
    EXPECT(!filter_matches("banner^"sv, "https://a.test/bannerx"sv));
    EXPECT(filter_matches("*^b|"sv, "https://a.test/x/b"sv));
    EXPECT(filter_matches("a^*|"sv, "https://a.test/a"sv));
    EXPECT(!filter_matches("*a*a*a*a*a*a*a*a*a*a*b"sv, "https://a.test/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv));
}

TEST_CASE(conditions)
{
    auto third_party = rule(R"({ "id": 1, "action": { "type": "block" }, "condition": { "domainType": "thirdParty", "requestDomains": ["tracker.test"] } })"sv);
    EXPECT(rule_matches(third_party, request("https://x.tracker.test/a"sv, ResourceType::Image, "https://site.test/"sv)));
    EXPECT(!rule_matches(third_party, request("https://x.tracker.test/a"sv, ResourceType::Image, "https://www.tracker.test/"sv)));
    EXPECT(!rule_matches(third_party, request("https://other.test/a"sv, ResourceType::Image, "https://site.test/"sv)));

    auto initiators = rule(R"({ "id": 1, "action": { "type": "block" }, "condition": { "initiatorDomains": ["site.test"], "excludedInitiatorDomains": ["ok.site.test"] } })"sv);
    EXPECT(rule_matches(initiators, request("https://a.test/"sv, ResourceType::Script, "https://www.site.test/"sv)));
    EXPECT(!rule_matches(initiators, request("https://a.test/"sv, ResourceType::Script, "https://ok.site.test/"sv)));
    EXPECT(!rule_matches(initiators, request("https://a.test/"sv)));

    // Without resourceTypes or excludedResourceTypes, a rule is for everything but top-level documents.
    auto any_type = rule(R"({ "id": 1, "action": { "type": "block" }, "condition": { "urlFilter": "a.test" } })"sv);
    EXPECT(rule_matches(any_type, request("https://a.test/"sv, ResourceType::SubFrame)));
    EXPECT(!rule_matches(any_type, request("https://a.test/"sv, ResourceType::MainFrame)));

    // With excludedResourceTypes alone, main_frame is included unless explicitly excluded.
    auto exclude_image_only = rule(R"({ "id": 1, "action": { "type": "block" }, "condition": { "urlFilter": "a.test", "excludedResourceTypes": ["image"] } })"sv);
    EXPECT(rule_matches(exclude_image_only, request("https://a.test/"sv, ResourceType::MainFrame)));
    EXPECT(!rule_matches(exclude_image_only, request("https://a.test/"sv, ResourceType::Image)));

    auto methods = rule(R"({ "id": 1, "action": { "type": "block" }, "condition": { "requestMethods": ["post"] } })"sv);
    EXPECT(rule_matches(methods, request("https://a.test/"sv, ResourceType::XMLHttpRequest, {}, "POST"sv)));
    EXPECT(!rule_matches(methods, request("https://a.test/"sv, ResourceType::XMLHttpRequest, {}, "get"sv)));

    auto regex = rule(R"({ "id": 1, "action": { "type": "block" }, "condition": { "regexFilter": "^https://a\\.test/[0-9]+$" } })"sv);
    EXPECT(rule_matches(regex, request("https://a.test/123"sv)));
    EXPECT(!rule_matches(regex, request("https://a.test/abc"sv)));

    auto tabs = rule(R"({ "id": 1, "action": { "type": "block" }, "condition": { "tabIds": [1] } })"sv);
    EXPECT(!rule_matches(tabs, request("https://a.test/"sv)));
    auto excluded_tabs = rule(R"({ "id": 1, "action": { "type": "block" }, "condition": { "excludedTabIds": [1] } })"sv);
    EXPECT(!rule_matches(excluded_tabs, request("https://a.test/"sv)));

    auto legacy_domains = rule(R"({ "id": 1, "action": { "type": "block" }, "condition": { "domains": ["site.test"], "excludedDomains": ["ok.site.test"] } })"sv);
    EXPECT(rule_matches(legacy_domains, request("https://a.test/"sv, ResourceType::Script, "https://www.site.test/"sv)));
    EXPECT(!rule_matches(legacy_domains, request("https://a.test/"sv, ResourceType::Script, "https://ok.site.test/"sv)));

    auto top_domains = rule(R"({ "id": 1, "action": { "type": "block" }, "condition": { "topDomains": ["site.test"] } })"sv);
    EXPECT(!rule_matches(top_domains, request("https://a.test/"sv)));
}

TEST_CASE(index_picks_the_winning_rule)
{
    Vector<Rule> rules;
    rules.append(rule(R"({ "id": 1, "action": { "type": "block" }, "condition": { "urlFilter": "||ads.test^" } })"sv));
    rules.append(rule(R"({ "id": 2, "action": { "type": "allow" }, "condition": { "urlFilter": "||ads.test/ok^" } })"sv));
    rules.append(rule(R"({ "id": 3, "priority": 2, "action": { "type": "block" }, "condition": { "urlFilter": "||ads.test/ok/bad" } })"sv));
    rules.append(rule(R"({ "id": 4, "action": { "type": "block" }, "condition": { "requestDomains": ["double-tracker.test"] } })"sv));
    rules.append(rule(R"({ "id": 5, "action": { "type": "block" }, "condition": { "initiatorDomains": ["news.test"] } })"sv));
    rules.append(rule(R"({ "id": 6, "action": { "type": "block" }, "condition": { "urlFilter": "*" } })"sv));
    RuleIndex index { move(rules) };

    Array all_actions { ActionType::Allow, ActionType::AllowAllRequests, ActionType::Block, ActionType::UpgradeScheme, ActionType::Redirect };
    auto best = [&](StringView request_url, Optional<StringView> initiator = {}) -> i64 {
        auto const* rule = index.best_match(request(request_url, ResourceType::Script, initiator), all_actions);
        return rule ? rule->id : 0;
    };
    EXPECT_EQ(best("https://ads.test/x"sv), 1);
    // Of rules of the same priority, allowing wins.
    EXPECT_EQ(best("https://ads.test/ok/x"sv), 2);
    EXPECT_EQ(best("https://ads.test/ok/bad"sv), 3);
    EXPECT_EQ(best("https://sub.double-tracker.test/x"sv), 4);
    EXPECT_EQ(best("https://nothing.test/"sv, "https://sub.news.test/"sv), 5);
    EXPECT_EQ(best("https://nothing.test/"sv), 6);

    Array allow_only { ActionType::Allow };
    auto const* allowed = index.best_match(request("https://ads.test/ok/x"sv), allow_only);
    EXPECT(allowed && allowed->id == 2);
}

TEST_CASE(redirects)
{
    auto extension_path = rule(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "extensionPath": "/noop.js" } } })"sv);
    EXPECT_EQ(redirect_url(extension_path, url("https://a.test/ad.js"sv), "abc"sv)->serialize(), "chrome-extension://abc/noop.js"sv);

    auto remove_params = rule(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "transform": { "queryTransform": { "removeParams": ["utm_source"] } } } } })"sv);
    EXPECT_EQ(redirect_url(remove_params, url("https://a.test/p?utm_source=x&id=1&flag#f"sv), "abc"sv)->serialize(), "https://a.test/p?id=1&flag#f"sv);
    EXPECT_EQ(redirect_url(remove_params, url("https://a.test/p?utm_source=x"sv), "abc"sv)->serialize(), "https://a.test/p"sv);

    auto encode_params = rule(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "transform": { "queryTransform": { "removeParams": ["a b"], "addOrReplaceParams": [{ "key": "c d", "value": "x y&z=1#h" }] } } } } })"sv);
    EXPECT_EQ(redirect_url(encode_params, url("https://a.test/p?a+b=1&keep=2"sv), "abc"sv)->serialize(), "https://a.test/p?keep=2&c+d=x+y%26z%3D1%23h"sv);

    auto repeated_params = rule(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "transform": { "queryTransform": { "addOrReplaceParams": [{ "key": "k", "value": "1" }, { "key": "k", "value": "2" }, { "key": "k", "value": "3" }, { "key": "m", "value": "9", "replaceOnly": true }] } } } } })"sv);
    EXPECT_EQ(redirect_url(repeated_params, url("https://a.test/p?k=a&k=b"sv), "abc"sv)->serialize(), "https://a.test/p?k=1&k=2&k=3"sv);

    auto unmatched_replace_only = rule(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "transform": { "queryTransform": { "addOrReplaceParams": [{ "key": "missing", "value": "1", "replaceOnly": true }] } } } } })"sv);
    EXPECT_EQ(redirect_url(unmatched_replace_only, url("https://a.test/p?a=1&&b=2"sv), "abc"sv)->serialize(), "https://a.test/p?a=1&&b=2"sv);

    auto substitution = rule(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "regexSubstitution": "https://b.test/\\1" } }, "condition": { "regexFilter": "^https://a\\.test/(.*)$" } })"sv);
    EXPECT_EQ(redirect_url(substitution, url("https://a.test/x/y"sv), "abc"sv)->serialize(), "https://b.test/x/y"sv);

    auto partial_substitution = rule(R"json({ "id": 1, "action": { "type": "redirect", "redirect": { "regexSubstitution": "/new/\\1" } }, "condition": { "regexFilter": "/old/([^/?]+)" } })json"sv);
    EXPECT_EQ(redirect_url(partial_substitution, url("https://a.test/prefix/old/item/suffix?q=1"sv), "abc"sv)->serialize(), "https://a.test/prefix/new/item/suffix?q=1"sv);

    auto bad_substitution = rule(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "regexSubstitution": "file:///\\1" } }, "condition": { "regexFilter": "^https://a\\.test/(.*)$" } })"sv);
    EXPECT(!redirect_url(bad_substitution, url("https://a.test/etc/passwd"sv), "abc"sv).has_value());

    auto upgrade_host = rule(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "transform": { "scheme": "https", "host": "new.test" } } } })"sv);
    EXPECT_EQ(redirect_url(upgrade_host, url("http://old.test/a?b"sv), "abc"sv)->serialize(), "https://new.test/a?b"sv);

    auto clear_path = rule(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "transform": { "path": "" } } } })"sv);
    EXPECT_EQ(redirect_url(clear_path, url("https://a.test/old/path?q=1#f"sv), "abc"sv)->serialize(), "https://a.test/?q=1#f"sv);

    auto double_slash_path = rule(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "transform": { "path": "//evil.test/x" } } } })"sv);
    EXPECT_EQ(redirect_url(double_slash_path, url("https://a.test/old?q=1"sv), "abc"sv)->serialize(), "https://a.test//evil.test/x?q=1"sv);

    auto bad_port = rule(R"({ "id": 1, "action": { "type": "redirect", "redirect": { "transform": { "port": "notaport" } } } })"sv);
    EXPECT(!redirect_url(bad_port, url("https://a.test/"sv), "abc"sv).has_value());
}

TEST_CASE(extension_network_rules_lifecycle_and_precedence)
{
    Test::TemporaryDirectory directory { "TestExtensionNetworkRules"sv };
    auto storage_path = directory.path().append("ExtensionNetworkRules.json"sv).string();

    auto manifest = MUST(JsonValue::from_string(R"({
        "manifest_version": 3,
        "name": "Blocker",
        "version": "1.0",
        "permissions": ["declarativeNetRequest"],
        "host_permissions": ["<all_urls>"],
        "declarative_net_request": {
            "rule_resources": [
                { "id": "default", "enabled": true, "path": "rules/default.json" },
                { "id": "extra", "enabled": false, "path": "rules/extra.json" }
            ]
        }
    })"sv)).as_object();
    auto default_rules = R"([
        { "id": 1, "priority": 1, "action": { "type": "block" }, "condition": { "urlFilter": "||ads.test^" } },
        { "id": 2, "priority": 1, "action": { "type": "upgradeScheme" }, "condition": { "urlFilter": "||upgrade.test^" } }
    ])"sv;
    auto extra_rules = R"([
        { "id": 10, "priority": 1, "action": { "type": "redirect", "redirect": { "extensionPath": "/noop.js" } }, "condition": { "urlFilter": "||shim.test^" } }
    ])"sv;

    auto make_reader = [&]() -> WebView::ExtensionNetworkRules::ReadPackageFile {
        return [&](StringView, StringView path) -> Optional<ByteBuffer> {
            if (path == "rules/default.json"sv)
                return MUST(ByteBuffer::copy(default_rules.bytes()));
            if (path == "rules/extra.json"sv)
                return MUST(ByteBuffer::copy(extra_rules.bytes()));
            return {};
        };
    };

    auto ext_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"_string;
    WebView::ExtensionNetworkRules engine { storage_path, make_reader() };
    engine.set_extensions({ ext_id }, [&](StringView) -> JsonObject const& { return manifest; });

    EXPECT(engine.has_rules());
    EXPECT(engine.extension_has_permission(ext_id));
    EXPECT_EQ(engine.enabled_rulesets(ext_id).size(), 1u);
    EXPECT_EQ(engine.evaluate(request("https://ads.test/banner.js"sv), {}).outcome, NetworkRequestOutcome::Block);
    EXPECT_EQ(engine.evaluate(request("https://shim.test/a.js"sv), {}).outcome, NetworkRequestOutcome::Allow);

    // Disable and re-enable an individual static rule.
    auto disable_rule1 = MUST(JsonValue::from_string(R"({ "rulesetId": "default", "disableRuleIds": [1] })"sv)).as_object();
    EXPECT(!engine.update_static_rules(ext_id, disable_rule1).is_error());
    EXPECT_EQ(engine.evaluate(request("https://ads.test/banner.js"sv), {}).outcome, NetworkRequestOutcome::Allow);
    auto disabled_ids = TRY_OR_FAIL(engine.disabled_rule_ids(ext_id, MUST(JsonValue::from_string(R"({ "rulesetId": "default" })"sv)).as_object()));
    EXPECT_EQ(disabled_ids.size(), 1u);
    auto enable_rule1 = MUST(JsonValue::from_string(R"({ "rulesetId": "default", "enableRuleIds": [1] })"sv)).as_object();
    EXPECT(!engine.update_static_rules(ext_id, enable_rule1).is_error());
    EXPECT_EQ(engine.evaluate(request("https://ads.test/banner.js"sv), {}).outcome, NetworkRequestOutcome::Block);

    auto upgraded = engine.evaluate(request("http://upgrade.test:80/a.js"sv), {});
    EXPECT_EQ(upgraded.outcome, NetworkRequestOutcome::UpgradeScheme);
    EXPECT_EQ(upgraded.redirect_url->serialize(), "https://upgrade.test/a.js"sv);
    EXPECT_EQ(engine.evaluate(request("https://upgrade.test/a.js"sv), {}).outcome, NetworkRequestOutcome::Allow);

    // Enable extra static ruleset.
    auto enable_extra = MUST(JsonValue::from_string(R"({ "enableRulesetIds": ["extra"] })"sv)).as_object();
    EXPECT(!engine.update_enabled_rulesets(ext_id, enable_extra).is_error());
    auto redirected = engine.evaluate(request("https://shim.test/a.js"sv), {});
    EXPECT_EQ(redirected.outcome, NetworkRequestOutcome::Redirect);
    EXPECT_EQ(redirected.redirect_url->serialize(), "chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/noop.js"sv);

    // Dynamic allowAllRequests rule exempts requests from a trusted top-level document and deeper descendants of an allowed iframe.
    auto add_allow_all = MUST(JsonValue::from_string(R"({
        "addRules": [
            {
                "id": 100,
                "priority": 10,
                "action": { "type": "allowAllRequests" },
                "condition": { "requestDomains": ["trusted.test"], "resourceTypes": ["main_frame"] }
            },
            {
                "id": 101,
                "priority": 10,
                "action": { "type": "allowAllRequests" },
                "condition": { "requestDomains": ["middle.test"], "initiatorDomains": ["parent.test"], "resourceTypes": ["sub_frame"] }
            }
        ]
    })"sv)).as_object();
    EXPECT(!engine.update_dynamic_rules(ext_id, add_allow_all).is_error());
    WebView::ExtensionNetworkRules::FrameContext trusted_frame {
        .ancestors = { { url("https://trusted.test/page"sv), {}, true } },
    };
    EXPECT_EQ(engine.evaluate(request("https://ads.test/banner.js"sv), trusted_frame).outcome, NetworkRequestOutcome::Allow);
    EXPECT_EQ(engine.evaluate(request("https://ads.test/banner.js"sv), {}).outcome, NetworkRequestOutcome::Block);

    WebView::ExtensionNetworkRules::FrameContext nested_iframe {
        .ancestors = {
            { url("https://leaf.test/frame"sv), url("https://middle.test/"sv).origin(), false },
            { url("https://middle.test/frame"sv), url("https://parent.test/"sv).origin(), false },
            { url("https://parent.test/page"sv), {}, true },
        },
    };
    EXPECT_EQ(engine.evaluate(request("https://ads.test/banner.js"sv), nested_iframe).outcome, NetworkRequestOutcome::Allow);

    // Session rule with higher priority beats dynamic allowAllRequests.
    auto add_session_block = MUST(JsonValue::from_string(R"({
        "addRules": [{
            "id": 200,
            "priority": 20,
            "action": { "type": "block" },
            "condition": { "urlFilter": "||ads.test/must-block" }
        }]
    })"sv)).as_object();
    EXPECT(!engine.update_session_rules(ext_id, add_session_block).is_error());
    EXPECT_EQ(engine.evaluate(request("https://ads.test/must-block"sv), trusted_frame).outcome, NetworkRequestOutcome::Block);

    auto matched = engine.matching_rules(ext_id, request("https://ads.test/must-block"sv), trusted_frame);
    EXPECT_EQ(matched.size(), 1u);
    EXPECT_EQ(matched.at(0).as_object().get_integer<i64>("ruleId"sv), 200);
    EXPECT_EQ(matched.at(0).as_object().get_string("rulesetId"sv), "_session"sv);

    // Dynamic rules and enabled rulesets persist across restarts; session rules do not.
    WebView::ExtensionNetworkRules reloaded { storage_path, make_reader() };
    reloaded.set_extensions({ ext_id }, [&](StringView) -> JsonObject const& { return manifest; });
    EXPECT_EQ(reloaded.enabled_rulesets(ext_id).size(), 2u);
    EXPECT_EQ(reloaded.evaluate(request("https://shim.test/a.js"sv), {}).outcome, NetworkRequestOutcome::Redirect);
    EXPECT_EQ(reloaded.evaluate(request("https://ads.test/must-block"sv), trusted_frame).outcome, NetworkRequestOutcome::Allow);

    // Extension version upgrade resets session rules and static ruleset overrides (persisted), retaining dynamic rules.
    auto upgraded_manifest = manifest;
    upgraded_manifest.set("version"sv, "2.0"_string);
    engine.set_extensions({ ext_id }, [&](StringView) -> JsonObject const& { return upgraded_manifest; });
    EXPECT_EQ(engine.enabled_rulesets(ext_id).size(), 1u);
    EXPECT_EQ(engine.evaluate(request("https://shim.test/a.js"sv), {}).outcome, NetworkRequestOutcome::Allow);
    EXPECT_EQ(engine.evaluate(request("https://ads.test/must-block"sv), trusted_frame).outcome, NetworkRequestOutcome::Allow);

    // Reinstalling the prior version after an upgrade does not resurrect stale static overrides.
    WebView::ExtensionNetworkRules downgraded { storage_path, make_reader() };
    downgraded.set_extensions({ ext_id }, [&](StringView) -> JsonObject const& { return manifest; });
    EXPECT_EQ(downgraded.enabled_rulesets(ext_id).size(), 1u);
    EXPECT_EQ(downgraded.evaluate(request("https://shim.test/a.js"sv), {}).outcome, NetworkRequestOutcome::Allow);
}

TEST_CASE(extension_network_rules_permissions_and_cross_extension_precedence)
{
    Test::TemporaryDirectory directory { "TestExtensionNetworkRulesPermissions"sv };
    auto storage_path = directory.path().append("ExtensionNetworkRules.json"sv).string();

    auto no_perm = MUST(JsonValue::from_string(R"({
        "manifest_version": 3, "name": "NoPerm", "version": "1.0",
        "declarative_net_request": { "rule_resources": [{ "id": "r", "enabled": true, "path": "r.json" }] }
    })"sv)).as_object();
    auto dnr_no_hosts = MUST(JsonValue::from_string(R"({
        "manifest_version": 3, "name": "NoHosts", "version": "1.0",
        "permissions": ["declarativeNetRequest"],
        "declarative_net_request": { "rule_resources": [{ "id": "r", "enabled": true, "path": "r.json" }] }
    })"sv)).as_object();
    auto host_access_only = MUST(JsonValue::from_string(R"({
        "manifest_version": 3, "name": "HostOnly", "version": "1.0",
        "permissions": ["declarativeNetRequestWithHostAccess"],
        "host_permissions": ["https://allowed-host.test/*"],
        "declarative_net_request": { "rule_resources": [{ "id": "r", "enabled": true, "path": "r.json" }] }
    })"sv)).as_object();

    auto no_perm_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"_string;
    auto no_hosts_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"_string;
    auto host_only_id = "cccccccccccccccccccccccccccccccc"_string;

    auto read_file = [&](StringView extension_id, StringView) -> Optional<ByteBuffer> {
        if (extension_id == no_perm_id)
            return MUST(ByteBuffer::copy(R"([{ "id": 1, "action": { "type": "block" }, "condition": { "urlFilter": "||noperm.test^" } }])"sv.bytes()));
        if (extension_id == no_hosts_id)
            return MUST(ByteBuffer::copy(R"([
                { "id": 1, "action": { "type": "redirect", "redirect": { "extensionPath": "/noop.js" } }, "condition": { "urlFilter": "||redir.test^" } },
                { "id": 2, "action": { "type": "upgradeScheme" }, "condition": { "urlFilter": "||both.test^" } }
            ])"sv.bytes()));
        return MUST(ByteBuffer::copy(R"([
            { "id": 1, "action": { "type": "block" }, "condition": { "urlFilter": "||allowed-host.test^" } },
            { "id": 2, "action": { "type": "block" }, "condition": { "urlFilter": "||denied-host.test^" } }
        ])"sv.bytes()));
    };

    WebView::ExtensionNetworkRules engine { storage_path, move(read_file) };
    engine.set_extensions({ no_perm_id, no_hosts_id, host_only_id }, [&](StringView id) -> JsonObject const& {
        if (id == no_perm_id)
            return no_perm;
        if (id == no_hosts_id)
            return dnr_no_hosts;
        return host_access_only;
    });

    EXPECT(!engine.extension_has_permission(no_perm_id));
    EXPECT(engine.extension_has_permission(no_hosts_id));
    EXPECT(engine.extension_has_permission(host_only_id));
    // Extension without DNR permission cannot activate static rules.
    EXPECT_EQ(engine.evaluate(request("https://noperm.test/a.js"sv), {}).outcome, NetworkRequestOutcome::Allow);
    // Redirect requires host permission even with declarativeNetRequest; upgradeScheme does not.
    EXPECT_EQ(engine.evaluate(request("https://redir.test/a.js"sv), {}).outcome, NetworkRequestOutcome::Allow);
    EXPECT_EQ(engine.evaluate(request("http://both.test/a.js"sv), {}).outcome, NetworkRequestOutcome::UpgradeScheme);
    // declarativeNetRequestWithHostAccess only acts on granted hosts.
    EXPECT_EQ(engine.evaluate(request("https://allowed-host.test/a.js"sv), {}).outcome, NetworkRequestOutcome::Block);
    EXPECT_EQ(engine.evaluate(request("https://denied-host.test/a.js"sv), {}).outcome, NetworkRequestOutcome::Allow);

    // Cross-extension UpgradeScheme and Redirect share equal priority; most recently installed extension wins.
    auto with_hosts = MUST(JsonValue::from_string(R"({
        "manifest_version": 3, "name": "WithHosts", "version": "1.0",
        "permissions": ["declarativeNetRequest"],
        "host_permissions": ["<all_urls>"],
        "declarative_net_request": { "rule_resources": [{ "id": "r", "enabled": true, "path": "r.json" }] }
    })"sv)).as_object();
    auto redir_id = "dddddddddddddddddddddddddddddddd"_string;
    auto cross_storage = directory.path().append("Cross.json"sv).string();
    WebView::ExtensionNetworkRules cross_engine { cross_storage, [&](StringView id, StringView) -> Optional<ByteBuffer> {
        if (id == no_hosts_id)
            return MUST(ByteBuffer::copy(R"([{ "id": 1, "action": { "type": "upgradeScheme" }, "condition": { "urlFilter": "||tie.test^" } }])"sv.bytes()));
        return MUST(ByteBuffer::copy(R"([{ "id": 1, "action": { "type": "redirect", "redirect": { "extensionPath": "/noop.js" } }, "condition": { "urlFilter": "||tie.test^" } }])"sv.bytes()));
    } };
    cross_engine.set_extensions({ no_hosts_id, redir_id }, [&](StringView id) -> JsonObject const& {
        return id == no_hosts_id ? dnr_no_hosts : with_hosts;
    });
    // redir_id was assigned a higher installOrder than no_hosts_id, so it wins initially; marking no_hosts_id installed flips the winner.
    EXPECT_EQ(cross_engine.evaluate(request("http://tie.test/a.js"sv), {}).outcome, NetworkRequestOutcome::Redirect);
    cross_engine.mark_installed(no_hosts_id);
    EXPECT_EQ(cross_engine.evaluate(request("http://tie.test/a.js"sv), {}).outcome, NetworkRequestOutcome::UpgradeScheme);

    // Subresource requests initiated by another extension's page (including sandboxed opaque-origin pages) are excluded.
    Request from_other_extension { url("https://allowed-host.test/a.js"sv), "get"_string, ResourceType::Script, URL::Origin { "chrome-extension"_string, URL::Host { String { no_hosts_id } }, {} } };
    EXPECT_EQ(engine.evaluate(from_other_extension, {}).outcome, NetworkRequestOutcome::Allow);
    Request from_sandboxed_other_extension { url("https://allowed-host.test/a.js"sv), "get"_string, ResourceType::Script, URL::Origin::create_opaque(), no_hosts_id };
    EXPECT_EQ(engine.evaluate(from_sandboxed_other_extension, {}).outcome, NetworkRequestOutcome::Allow);
    // Enabling a ruleset whose file is missing/malformed fails instead of persisting it.
    auto missing_manifest = MUST(JsonValue::from_string(R"({
        "manifest_version": 3, "name": "Missing", "version": "1.0",
        "permissions": ["declarativeNetRequest"],
        "declarative_net_request": { "rule_resources": [{ "id": "broken", "enabled": false, "path": "missing.json" }] }
    })"sv)).as_object();
    auto missing_storage = directory.path().append("Missing.json"sv).string();
    WebView::ExtensionNetworkRules missing_engine { missing_storage, [&](StringView, StringView) -> Optional<ByteBuffer> { return {}; } };
    missing_engine.set_extensions({ no_hosts_id }, [&](StringView) -> JsonObject const& { return missing_manifest; });
    EXPECT(missing_engine.update_enabled_rulesets(no_hosts_id, MUST(JsonValue::from_string(R"({ "enableRulesetIds": ["broken"] })"sv)).as_object()).is_error());
    EXPECT_EQ(missing_engine.enabled_rulesets(no_hosts_id).size(), 0u);
    Request from_same_extension { url("https://allowed-host.test/a.js"sv), "get"_string, ResourceType::Script, URL::Origin { "chrome-extension"_string, URL::Host { String { host_only_id } }, {} } };
    EXPECT_EQ(engine.evaluate(from_same_extension, {}).outcome, NetworkRequestOutcome::Block);

    // Higher-priority upgradeScheme on an already-HTTPS URL does not shadow a lower-priority block rule.
    auto add_https_upgrade = MUST(JsonValue::from_string(R"({
        "addRules": [{ "id": 99, "priority": 100, "action": { "type": "upgradeScheme" }, "condition": { "urlFilter": "||allowed-host.test^" } }]
    })"sv)).as_object();
    EXPECT(!engine.update_session_rules(host_only_id, add_https_upgrade).is_error());
    EXPECT_EQ(engine.evaluate(request("https://allowed-host.test/a.js"sv), {}).outcome, NetworkRequestOutcome::Block);

    // Frame navigation redirect requires destination host permission only, not the linking page's initiator host.
    auto nav_redirect = MUST(JsonValue::from_string(R"({
        "addRules": [{ "id": 101, "priority": 200, "action": { "type": "redirect", "redirect": { "url": "https://other.test/" } }, "condition": { "urlFilter": "||allowed-host.test/nav", "resourceTypes": ["main_frame"] } }]
    })"sv)).as_object();
    EXPECT(!engine.update_session_rules(host_only_id, nav_redirect).is_error());
    EXPECT_EQ(engine.evaluate(request("https://allowed-host.test/nav"sv, ResourceType::MainFrame, "https://ungranted-linker.test/"sv), {}).outcome, NetworkRequestOutcome::Redirect);

    // A write failure during persistence rejects the update and keeps previous in-memory rules intact.
    auto unwritable_path = directory.path().append("as_dir"sv).string();
    MUST(Core::Directory::create(unwritable_path, Core::Directory::CreateDirectories::Yes));
    WebView::ExtensionNetworkRules failing_engine { unwritable_path, [&](StringView, StringView) -> Optional<ByteBuffer> { return {}; } };
    failing_engine.set_extensions({ no_hosts_id }, [&](StringView) -> JsonObject const& { return dnr_no_hosts; });
    auto add_rule = MUST(JsonValue::from_string(R"({ "addRules": [{ "id": 1, "action": { "type": "block" }, "condition": { "urlFilter": "||fail.test^" } }] })"sv)).as_object();
    EXPECT(failing_engine.update_dynamic_rules(no_hosts_id, add_rule).is_error());
    EXPECT_EQ(failing_engine.dynamic_rules(no_hosts_id, {}).size(), 0u);
    EXPECT_EQ(failing_engine.evaluate(request("https://fail.test/a.js"sv), {}).outcome, NetworkRequestOutcome::Allow);
}
