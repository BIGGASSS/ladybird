/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebView/ExtensionNetworkRules.h>

namespace WebView {

using namespace DeclarativeNetRequest;
using Web::Extensions::NetworkRequestOutcome;

// https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest#property-MAX_NUMBER_OF_DYNAMIC_RULES
static constexpr size_t MAX_NUMBER_OF_DYNAMIC_RULES = 30000;
static constexpr size_t MAX_NUMBER_OF_SESSION_RULES = 5000;
static constexpr size_t MAX_NUMBER_OF_ENABLED_STATIC_RULESETS = 50;
static constexpr size_t MAX_NUMBER_OF_DISABLED_STATIC_RULES = 5000;
static constexpr i64 GUARANTEED_MINIMUM_STATIC_RULES = 30000;
static constexpr i64 GLOBAL_STATIC_RULE_LIMIT = 300000;

static constexpr auto DYNAMIC_RULESET_ID = "_dynamic"sv;
static constexpr auto SESSION_RULESET_ID = "_session"sv;

ExtensionNetworkRules::ExtensionNetworkRules(ByteString storage_path, ReadPackageFile read_package_file)
    : m_storage_path(move(storage_path))
    , m_read_package_file(move(read_package_file))
{
    load_storage();
}

ExtensionNetworkRules::~ExtensionNetworkRules() = default;

ExtensionNetworkRules::ExtensionRules* ExtensionNetworkRules::find(StringView extension_id)
{
    for (auto& extension : m_extensions) {
        if (extension.extension_id == extension_id)
            return &extension;
    }
    return nullptr;
}

ExtensionNetworkRules::ExtensionRules const* ExtensionNetworkRules::find(StringView extension_id) const
{
    return const_cast<ExtensionNetworkRules&>(*this).find(extension_id);
}

bool ExtensionNetworkRules::extension_has_permission(StringView extension_id) const
{
    auto const* rules = find(extension_id);
    return rules && (rules->has_dnr_permission || rules->has_dnr_with_host_access);
}

static Vector<Rule> parse_rules(JsonArray const& rules)
{
    Vector<Rule> parsed;
    for (auto const& value : rules.values()) {
        if (!value.is_object())
            continue;
        if (auto rule = parse_rule(value.as_object()); !rule.is_error())
            parsed.append(rule.release_value());
    }
    return parsed;
}

static bool host_permission_matches(StringView pattern, URL::URL const& url)
{
    if (pattern == "<all_urls>"sv)
        return Web::Extensions::match_pattern_matches(pattern, url);
    auto host_start = pattern.find("://"sv);
    if (!host_start.has_value())
        return false;
    auto path_start = pattern.find('/', *host_start + 3);
    if (!path_start.has_value())
        return false;
    auto any_path = ByteString::formatted("{}/*", pattern.substring_view(0, *path_start));
    return Web::Extensions::match_pattern_matches(any_path, url);
}

static bool has_host_permission(ReadonlySpan<String> patterns, URL::URL const& url)
{
    return any_of(patterns, [&](String const& pattern) { return host_permission_matches(pattern, url); });
}

static bool is_frame(ResourceType type)
{
    return type == ResourceType::MainFrame || type == ResourceType::SubFrame;
}

static bool has_host_permission_for_request(ReadonlySpan<String> patterns, Request const& request)
{
    if (!has_host_permission(patterns, request.url))
        return false;
    // Frame navigations require host permission for the destination URL only; subresources also require initiator access.
    if (!is_frame(request.type) && request.initiator.has_value() && !request.initiator->is_opaque() && request.initiator->scheme().has_value() && request.initiator->scheme()->bytes_as_string_view().is_one_of("http"sv, "https"sv)) {
        auto initiator_url = URL::Parser::basic_parse(ByteString::formatted("{}/", request.initiator->serialize()));
        if (!initiator_url.has_value() || !has_host_permission(patterns, *initiator_url))
            return false;
    }
    return true;
}

static Vector<String> manifest_string_array(JsonObject const& object, StringView key)
{
    Vector<String> result;
    if (auto array = object.get_array(key); array.has_value()) {
        array->for_each([&](auto const& value) {
            if (value.is_string())
                result.append(value.as_string());
        });
    }
    return result;
}

void ExtensionNetworkRules::set_extensions(Vector<String> const& extension_ids, Function<JsonObject const&(StringView)> manifest_for)
{
    bool needs_save = false;
    i64 global_extra_used = 0;
    Vector<ExtensionRules> extensions;
    for (auto const& extension_id : extension_ids) {
        auto const& manifest = manifest_for(extension_id);

        ExtensionRules rules;
        rules.extension_id = extension_id;
        rules.version = manifest.get_string("version"sv).value_or({});

        auto permissions = manifest_string_array(manifest, "permissions"sv);
        rules.has_dnr_permission = permissions.contains_slow("declarativeNetRequest"_string);
        rules.has_dnr_with_host_access = permissions.contains_slow("declarativeNetRequestWithHostAccess"_string);

        auto host_permissions = manifest_string_array(manifest, "host_permissions"sv);
        if (!host_permissions.is_empty()) {
            rules.host_permissions = move(host_permissions);
        } else {
            for (auto const& permission : permissions) {
                if (permission == "<all_urls>"sv || permission.contains("://"sv))
                    rules.host_permissions.append(permission);
            }
        }

        if (auto stored = m_storage.get_object(extension_id); stored.has_value()) {
            if (auto order = stored->get_u64("installOrder"sv); order.has_value()) {
                rules.install_order = *order;
                m_next_install_order = max(m_next_install_order, *order);
            }
        }
        if (rules.install_order == 0) {
            if (auto* existing = find(extension_id); existing && existing->install_order > 0)
                rules.install_order = existing->install_order;
            else
                rules.install_order = ++m_next_install_order;
        }

        if (!rules.has_dnr_permission && !rules.has_dnr_with_host_access) {
            extensions.append(move(rules));
            continue;
        }

        if (auto network_rules = manifest.get_object("declarative_net_request"sv); network_rules.has_value()) {
            if (auto resources = network_rules->get_array("rule_resources"sv); resources.has_value()) {
                for (auto const& resource : resources->values()) {
                    if (!resource.is_object())
                        continue;
                    auto id = resource.as_object().get_string("id"sv);
                    auto path = resource.as_object().get_string("path"sv);
                    if (id.has_value() && path.has_value())
                        rules.rulesets.append({ *id, *path, resource.as_object().get_bool("enabled"sv).value_or(false) });
                }
            }
        }

        auto has_ruleset = [&](StringView id) {
            return any_of(rules.rulesets, [&](auto const& ruleset) { return ruleset.id == id; });
        };

        // Session rules last as long as the browser runs, unless the extension itself was updated to a new version.
        if (auto* existing = find(extension_id); existing && existing->version == rules.version)
            rules.session_rules = move(existing->session_rules);

        if (auto stored = m_storage.get_object(extension_id); stored.has_value()) {
            auto stored_version = stored->get_string("version"sv);
            auto same_version = !stored_version.has_value() || *stored_version == rules.version;
            if (!same_version) {
                needs_save = true;
            } else {
                if (auto enabled = stored->get_array("enabledRulesets"sv); enabled.has_value()) {
                    Vector<String> ids;
                    enabled->for_each([&](auto const& id) {
                        if (id.is_string() && has_ruleset(id.as_string()))
                            ids.append(id.as_string());
                        else
                            needs_save = true;
                    });
                    rules.enabled_rulesets = move(ids);
                }
                if (auto disabled_map = stored->get_object("disabledStaticRules"sv); disabled_map.has_value()) {
                    disabled_map->for_each_member([&](String const& ruleset_id, JsonValue const& rule_ids) {
                        if (!has_ruleset(ruleset_id) || !rule_ids.is_array()) {
                            needs_save = true;
                            return;
                        }
                        HashTable<i64> ids;
                        rule_ids.as_array().for_each([&](auto const& id) {
                            if (auto number = id.template get_integer<i64>(); number.has_value())
                                ids.set(*number);
                        });
                        if (!ids.is_empty())
                            rules.disabled_static_rules.set(ruleset_id, move(ids));
                    });
                }
            }
            if (auto dynamic = stored->get_array("dynamicRules"sv); dynamic.has_value())
                rules.dynamic_rules = RuleIndex { parse_rules(*dynamic) };
        }

        // Enforce the shared static rule quota and enabled-ruleset limit during initial loading as well.
        auto wanted = enabled_ruleset_ids(rules);
        if (wanted.size() > MAX_NUMBER_OF_ENABLED_STATIC_RULESETS)
            wanted.shrink(MAX_NUMBER_OF_ENABLED_STATIC_RULESETS);
        i64 own_count = 0;
        for (auto const& id : wanted) {
            auto loaded = load_static_rules_for(rules, Array { id });
            auto entry = loaded.find(id);
            if (entry == loaded.end())
                continue;
            auto candidate_own = own_count + static_cast<i64>(entry->value.size());
            auto additional_extra = max<i64>(0, candidate_own - GUARANTEED_MINIMUM_STATIC_RULES) - max<i64>(0, own_count - GUARANTEED_MINIMUM_STATIC_RULES);
            if (global_extra_used + additional_extra > GLOBAL_STATIC_RULE_LIMIT) {
                warnln("Skipping static ruleset '{}' of extension {}: global static rule limit exceeded", id, rules.extension_id);
                continue;
            }
            own_count = candidate_own;
            global_extra_used += additional_extra;
            rules.static_rules.set(entry->key, move(entry->value));
        }
        extensions.append(move(rules));
    }
    m_extensions = move(extensions);
    rebuild_evaluation_order();
    if (needs_save)
        (void)save_storage(serialize_storage());
}

void ExtensionNetworkRules::mark_installed(StringView extension_id)
{
    auto* rules = find(extension_id);
    if (!rules)
        return;
    rules->install_order = ++m_next_install_order;
    rebuild_evaluation_order();
    (void)save_storage(serialize_storage());
}

void ExtensionNetworkRules::rebuild_evaluation_order()
{
    m_evaluation_order.clear_with_capacity();
    for (size_t i = 0; i < m_extensions.size(); ++i)
        m_evaluation_order.append(i);
    quick_sort(m_evaluation_order, [&](size_t a, size_t b) {
        if (m_extensions[a].install_order != m_extensions[b].install_order)
            return m_extensions[a].install_order > m_extensions[b].install_order;
        return a > b;
    });
}

bool ExtensionNetworkRules::has_rules() const
{
    return any_of(m_extensions, [](auto const& extension) {
        return (extension.has_dnr_permission || extension.has_dnr_with_host_access)
            && (!extension.static_rules.is_empty() || extension.dynamic_rules.size() > 0 || extension.session_rules.size() > 0);
    });
}

Vector<String> ExtensionNetworkRules::enabled_ruleset_ids(ExtensionRules const& rules) const
{
    if (rules.enabled_rulesets.has_value())
        return *rules.enabled_rulesets;
    Vector<String> ids;
    for (auto const& ruleset : rules.rulesets) {
        if (ruleset.enabled_by_default)
            ids.append(ruleset.id);
    }
    return ids;
}

HashMap<String, RuleIndex> ExtensionNetworkRules::load_static_rules_for(ExtensionRules const& rules, ReadonlySpan<String> enabled) const
{
    HashMap<String, RuleIndex> loaded;
    for (auto const& ruleset : rules.rulesets) {
        if (!enabled.contains_slow(ruleset.id))
            continue;
        auto file = m_read_package_file(rules.extension_id, ruleset.path);
        if (!file.has_value()) {
            warnln("Extension {} has no ruleset file {}", rules.extension_id, ruleset.path);
            continue;
        }
        StringView contents { file->bytes() };
        if (contents.starts_with("\xEF\xBB\xBF"sv))
            contents = contents.substring_view(3);
        auto json = JsonValue::from_string(contents);
        if (json.is_error() || !json.value().is_array()) {
            warnln("Extension {} has an invalid ruleset {}", rules.extension_id, ruleset.path);
            continue;
        }
        loaded.set(ruleset.id, RuleIndex { parse_rules(json.value().as_array()) });
    }
    return loaded;
}

static JsonArray rules_json(RuleIndex const& index, JsonObject const& filter)
{
    Optional<Vector<i64>> ids;
    if (auto rule_ids = filter.get_array("ruleIds"sv); rule_ids.has_value()) {
        ids = Vector<i64> {};
        rule_ids->for_each([&](auto const& id) {
            if (auto number = id.template get_integer<i64>(); number.has_value())
                ids->append(*number);
        });
    }

    JsonArray result;
    for (auto const& rule : index.rules()) {
        if (!ids.has_value() || ids->contains_slow(rule.id))
            result.must_append(rule.json);
    }
    return result;
}

JsonArray ExtensionNetworkRules::dynamic_rules(StringView extension_id, JsonObject const& filter) const
{
    auto const* rules = find(extension_id);
    return rules ? rules_json(rules->dynamic_rules, filter) : JsonArray {};
}

JsonArray ExtensionNetworkRules::session_rules(StringView extension_id, JsonObject const& filter) const
{
    auto const* rules = find(extension_id);
    return rules ? rules_json(rules->session_rules, filter) : JsonArray {};
}

// https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest#method-updateDynamicRules
static ErrorOr<RuleIndex, String> updated_rules(RuleIndex const& index, JsonObject const& options, size_t limit)
{
    HashTable<i64> removed_ids;
    if (auto ids = options.get_array("removeRuleIds"sv); ids.has_value()) {
        for (auto const& id : ids->values()) {
            if (auto number = id.get_integer<i64>(); number.has_value())
                removed_ids.set(*number);
        }
    }

    Vector<Rule> rules;
    HashTable<i64> ids;
    for (auto const& rule : index.rules()) {
        if (removed_ids.contains(rule.id))
            continue;
        ids.set(rule.id);
        rules.append(MUST(parse_rule(rule.json)));
    }

    if (auto added = options.get_array("addRules"sv); added.has_value()) {
        for (auto const& value : added->values()) {
            if (!value.is_object())
                return "Rules must be objects"_string;
            auto rule = TRY(parse_rule(value.as_object()));
            if (ids.set(rule.id) != HashSetResult::InsertedNewEntry)
                return MUST(String::formatted("Rule with id {} does not have a unique ID.", rule.id));
            rules.append(move(rule));
        }
    }

    if (rules.size() > limit)
        return "The number of rules exceeds the limit."_string;
    return RuleIndex { move(rules) };
}

ErrorOr<void, String> ExtensionNetworkRules::update_dynamic_rules(StringView extension_id, JsonObject const& options)
{
    auto* rules = find(extension_id);
    if (!rules)
        return "The extension is not available"_string;
    auto next_dynamic = TRY(updated_rules(rules->dynamic_rules, options, MAX_NUMBER_OF_DYNAMIC_RULES));

    ExtensionRules candidate;
    candidate.extension_id = rules->extension_id;
    candidate.version = rules->version;
    candidate.install_order = rules->install_order;
    candidate.enabled_rulesets = rules->enabled_rulesets;
    candidate.disabled_static_rules = rules->disabled_static_rules;
    candidate.dynamic_rules = move(next_dynamic);

    auto storage = serialize_storage(&candidate);
    TRY(save_storage(storage));
    rules->dynamic_rules = move(candidate.dynamic_rules);
    return {};
}

ErrorOr<void, String> ExtensionNetworkRules::update_session_rules(StringView extension_id, JsonObject const& options)
{
    auto* rules = find(extension_id);
    if (!rules)
        return "The extension is not available"_string;
    rules->session_rules = TRY(updated_rules(rules->session_rules, options, MAX_NUMBER_OF_SESSION_RULES));
    return {};
}

JsonArray ExtensionNetworkRules::enabled_rulesets(StringView extension_id) const
{
    JsonArray result;
    if (auto const* rules = find(extension_id)) {
        for (auto const& ruleset : rules->rulesets) {
            if (rules->static_rules.contains(ruleset.id))
                result.must_append(ruleset.id);
        }
    }
    return result;
}

// https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest#method-updateEnabledRulesets
ErrorOr<void, String> ExtensionNetworkRules::update_enabled_rulesets(StringView extension_id, JsonObject const& options)
{
    auto* rules = find(extension_id);
    if (!rules)
        return "The extension is not available"_string;

    auto ids_of = [&](StringView key) -> ErrorOr<Vector<String>, String> {
        Vector<String> ids;
        if (auto list = options.get_array(key); list.has_value()) {
            for (auto const& id : list->values()) {
                if (!id.is_string() || !any_of(rules->rulesets, [&](auto const& ruleset) { return ruleset.id == id.as_string(); }))
                    return MUST(String::formatted("Invalid ruleset id: {}.", id.is_string() ? id.as_string() : id.serialized()));
                ids.append(id.as_string());
            }
        }
        return ids;
    };
    auto disabled = TRY(ids_of("disableRulesetIds"sv));
    auto enabled = TRY(ids_of("enableRulesetIds"sv));

    Vector<String> result;
    for (auto const& ruleset : rules->rulesets) {
        auto is_enabled = enabled.contains_slow(ruleset.id) || (rules->static_rules.contains(ruleset.id) && !disabled.contains_slow(ruleset.id));
        if (is_enabled)
            result.append(ruleset.id);
    }
    if (result.size() > MAX_NUMBER_OF_ENABLED_STATIC_RULESETS)
        return "The number of enabled static rulesets exceeds the limit."_string;

    // Load newly enabled rulesets first so we can verify they loaded and stay within the shared static rule quota.
    Vector<String> to_load;
    i64 candidate_rule_count = 0;
    for (auto const& id : result) {
        if (auto existing = rules->static_rules.find(id); existing != rules->static_rules.end())
            candidate_rule_count += static_cast<i64>(existing->value.size());
        else
            to_load.append(id);
    }
    auto newly_loaded = load_static_rules_for(*rules, to_load);
    for (auto const& id : to_load) {
        if (!newly_loaded.contains(id))
            return MUST(String::formatted("Failed to load static ruleset: {}.", id));
    }
    for (auto const& entry : newly_loaded)
        candidate_rule_count += static_cast<i64>(entry.value.size());

    i64 global_extra = max<i64>(0, candidate_rule_count - GUARANTEED_MINIMUM_STATIC_RULES);
    for (auto const& other : m_extensions) {
        if (other.extension_id == extension_id)
            continue;
        i64 other_count = 0;
        for (auto const& ruleset : other.static_rules)
            other_count += static_cast<i64>(ruleset.value.size());
        global_extra += max<i64>(0, other_count - GUARANTEED_MINIMUM_STATIC_RULES);
    }
    if (global_extra > GLOBAL_STATIC_RULE_LIMIT)
        return "The number of enabled static rules exceeds the global limit."_string;

    auto previous = rules->enabled_rulesets;
    rules->enabled_rulesets = move(result);
    auto storage = serialize_storage();
    if (auto saved = save_storage(storage); saved.is_error()) {
        rules->enabled_rulesets = move(previous);
        return saved.release_error();
    }
    rules->static_rules.remove_all_matching([&](auto const& id, auto const&) { return !rules->enabled_rulesets->contains_slow(id); });
    for (auto& entry : newly_loaded)
        rules->static_rules.set(entry.key, move(entry.value));
    return {};
}

// https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest#method-getDisabledRuleIds
ErrorOr<JsonArray, String> ExtensionNetworkRules::disabled_rule_ids(StringView extension_id, JsonObject const& options) const
{
    auto const* rules = find(extension_id);
    if (!rules)
        return "The extension is not available"_string;
    auto ruleset_id = options.get_string("rulesetId"sv);
    if (!ruleset_id.has_value() || !any_of(rules->rulesets, [&](auto const& ruleset) { return ruleset.id == *ruleset_id; }))
        return MUST(String::formatted("Invalid ruleset id: {}.", ruleset_id.value_or({})));

    JsonArray result;
    if (auto ids = rules->disabled_static_rules.get(*ruleset_id); ids.has_value()) {
        for (auto id : *ids)
            result.must_append(id);
    }
    return result;
}

// https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest#method-updateStaticRules
ErrorOr<void, String> ExtensionNetworkRules::update_static_rules(StringView extension_id, JsonObject const& options)
{
    auto* rules = find(extension_id);
    if (!rules)
        return "The extension is not available"_string;
    auto ruleset_id = options.get_string("rulesetId"sv);
    if (!ruleset_id.has_value() || !any_of(rules->rulesets, [&](auto const& ruleset) { return ruleset.id == *ruleset_id; }))
        return MUST(String::formatted("Invalid ruleset id: {}.", ruleset_id.value_or({})));

    auto next_disabled = rules->disabled_static_rules;
    auto& set = next_disabled.ensure(*ruleset_id);
    if (auto enable_ids = options.get_array("enableRuleIds"sv); enable_ids.has_value()) {
        for (auto const& id : enable_ids->values()) {
            if (auto number = id.get_integer<i64>(); number.has_value())
                set.remove(*number);
        }
    }
    if (auto disable_ids = options.get_array("disableRuleIds"sv); disable_ids.has_value()) {
        for (auto const& id : disable_ids->values()) {
            if (auto number = id.get_integer<i64>(); number.has_value())
                set.set(*number);
        }
    }
    if (set.is_empty())
        next_disabled.remove(*ruleset_id);

    size_t total_disabled = 0;
    for (auto const& entry : next_disabled)
        total_disabled += entry.value.size();
    if (total_disabled > MAX_NUMBER_OF_DISABLED_STATIC_RULES)
        return "The number of disabled static rules exceeds the limit."_string;

    auto previous = move(rules->disabled_static_rules);
    rules->disabled_static_rules = move(next_disabled);
    auto storage = serialize_storage();
    if (auto saved = save_storage(storage); saved.is_error()) {
        rules->disabled_static_rules = move(previous);
        return saved.release_error();
    }
    return {};
}

i64 ExtensionNetworkRules::available_static_rule_count(StringView extension_id) const
{
    i64 own_count = 0;
    i64 global_extra = 0;
    for (auto const& extension : m_extensions) {
        i64 count = 0;
        for (auto const& ruleset : extension.static_rules)
            count += static_cast<i64>(ruleset.value.size());
        if (extension.extension_id == extension_id)
            own_count = count;
        global_extra += max<i64>(0, count - GUARANTEED_MINIMUM_STATIC_RULES);
    }
    auto guaranteed_remaining = max<i64>(0, GUARANTEED_MINIMUM_STATIC_RULES - own_count);
    auto global_remaining = max<i64>(0, GLOBAL_STATIC_RULE_LIMIT - global_extra);
    return guaranteed_remaining + global_remaining;
}

ExtensionNetworkRules::Match ExtensionNetworkRules::best_match(ExtensionRules const& rules, PreparedRequest& prepared, FrameContext const& frame) const
{
    if (!rules.has_dnr_permission && !rules.has_dnr_with_host_access)
        return {};
    // An extension's rules must not intercept non-main-frame requests initiated by another extension's page (including
    // manifest-sandboxed extension pages, whose origin is opaque).
    Optional<String> initiating_extension_id = prepared.request.initiator_extension_id;
    if (!initiating_extension_id.has_value()
        && prepared.request.initiator.has_value()
        && !prepared.request.initiator->is_opaque()
        && prepared.request.initiator->scheme() == "chrome-extension"sv) {
        initiating_extension_id = prepared.request.initiator->host().serialize();
    }
    if (prepared.request.type != ResourceType::MainFrame
        && initiating_extension_id.has_value()
        && *initiating_extension_id != rules.extension_id) {
        return {};
    }

    Match best;
    auto consider = [&](RuleIndex const& index, StringView ruleset_id, PreparedRequest& candidate, u32 action_mask, HashTable<i64> const* disabled_ids = nullptr) {
        if (auto const* rule = index.best_match(candidate, action_mask, disabled_ids); rule && rule_wins_over(*rule, best.rule))
            best = { rule, MUST(String::from_utf8(ruleset_id)) };
    };
    auto consider_all = [&](PreparedRequest& candidate, u32 action_mask) {
        consider(rules.session_rules, SESSION_RULESET_ID, candidate, action_mask);
        consider(rules.dynamic_rules, DYNAMIC_RULESET_ID, candidate, action_mask);
        for (auto const& ruleset : rules.static_rules) {
            auto disabled = rules.disabled_static_rules.get(ruleset.key);
            consider(ruleset.value, ruleset.key, candidate, action_mask, disabled.has_value() ? &disabled.value() : nullptr);
        }
    };

    static constexpr u32 request_actions = action_type_mask(ActionType::Allow)
        | action_type_mask(ActionType::Block)
        | action_type_mask(ActionType::UpgradeScheme)
        | action_type_mask(ActionType::Redirect);
    static constexpr u32 allow_all_actions = action_type_mask(ActionType::AllowAllRequests);
    static constexpr u32 frame_actions = request_actions | allow_all_actions;

    auto has_host_access = has_host_permission_for_request(rules.host_permissions, prepared.request);
    if (rules.has_dnr_permission || has_host_access) {
        auto actions = is_frame(prepared.request.type) ? frame_actions : request_actions;
        if (!has_host_access)
            actions &= ~action_type_mask(ActionType::Redirect);
        // upgradeScheme does not apply to non-http URLs; exclude it so it cannot shadow a lower-priority action.
        if (prepared.request.url.scheme() != "http"sv)
            actions &= ~action_type_mask(ActionType::UpgradeScheme);
        consider_all(prepared, actions);
    }

    auto has_allow_all = rules.session_rules.has_any_action(allow_all_actions)
        || rules.dynamic_rules.has_any_action(allow_all_actions)
        || any_of(rules.static_rules, [](auto const& ruleset) { return ruleset.value.has_any_action(allow_all_actions); });
    if (!has_allow_all)
        return best;

    // A rule that allows all requests of an ancestor document allows those of its descendants' requests that no rule of
    // a higher priority acts on.
    for (auto const& ancestor : frame.ancestors) {
        if (!ancestor.url.scheme().is_one_of("http"sv, "https"sv))
            continue;
        Request document_request { ancestor.url, "get"_string, ancestor.is_top_level ? ResourceType::MainFrame : ResourceType::SubFrame, ancestor.initiator };
        if (!rules.has_dnr_permission && !has_host_permission_for_request(rules.host_permissions, document_request))
            continue;
        PreparedRequest prepared_document { document_request };
        consider_all(prepared_document, allow_all_actions);
    }

    return best;
}

// https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest#implementation-matching-algorithm
ExtensionNetworkRules::Outcome ExtensionNetworkRules::evaluate(Request const& request, FrameContext const& frame) const
{
    if (!has_rules())
        return {};
    PreparedRequest prepared { request };
    Optional<Outcome> redirect_or_upgrade;
    for (auto index : m_evaluation_order) {
        auto const& rules = m_extensions[index];
        auto match = best_match(rules, prepared, frame);
        if (!match.rule)
            continue;
        switch (match.rule->action) {
        case ActionType::Block:
            // A request that any extension blocks is blocked.
            return { NetworkRequestOutcome::Block, {} };
        case ActionType::Redirect:
            if (!redirect_or_upgrade.has_value()) {
                if (auto target = redirect_url(*match.rule, request.url, rules.extension_id); target.has_value() && !target->equals(request.url, URL::ExcludeFragment::Yes))
                    redirect_or_upgrade = Outcome { NetworkRequestOutcome::Redirect, move(target) };
            }
            break;
        case ActionType::UpgradeScheme:
            if (!redirect_or_upgrade.has_value() && request.url.scheme() == "http"sv) {
                auto upgraded = request.url;
                upgraded.set_scheme("https"sv);
                if (upgraded.port() == 80)
                    upgraded.set_port({});
                redirect_or_upgrade = Outcome { NetworkRequestOutcome::UpgradeScheme, move(upgraded) };
            }
            break;
        case ActionType::Allow:
        case ActionType::AllowAllRequests:
        case ActionType::ModifyHeaders:
            break;
        }
    }

    if (redirect_or_upgrade.has_value())
        return redirect_or_upgrade.release_value();
    return {};
}

JsonArray ExtensionNetworkRules::matching_rules(StringView extension_id, Request const& request, FrameContext const& frame) const
{
    JsonArray result;
    auto const* rules = find(extension_id);
    if (!rules)
        return result;
    PreparedRequest prepared { request };
    auto match = best_match(*rules, prepared, frame);
    if (!match.rule)
        return result;
    JsonObject matched;
    matched.set("ruleId"sv, match.rule->id);
    matched.set("rulesetId"sv, match.ruleset_id);
    result.must_append(move(matched));
    return result;
}

void ExtensionNetworkRules::load_storage()
{
    auto file = Core::File::open(m_storage_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return;
    auto bytes = file.value()->read_until_eof();
    if (bytes.is_error())
        return;
    auto json = JsonValue::from_string(StringView { bytes.value() });
    if (!json.is_error() && json.value().is_object())
        m_storage = move(json.value().as_object());
}

JsonObject ExtensionNetworkRules::serialize_storage(ExtensionRules const* override_extension) const
{
    JsonObject storage;
    for (auto const& extension : m_extensions) {
        auto const& rules = (override_extension && override_extension->extension_id == extension.extension_id) ? *override_extension : extension;
        JsonObject entry;
        entry.set("version"sv, rules.version);
        entry.set("installOrder"sv, rules.install_order);
        if (rules.enabled_rulesets.has_value()) {
            JsonArray enabled;
            for (auto const& id : *rules.enabled_rulesets)
                enabled.must_append(id);
            entry.set("enabledRulesets"sv, move(enabled));
        }
        if (!rules.disabled_static_rules.is_empty()) {
            JsonObject disabled;
            for (auto const& ruleset : rules.disabled_static_rules) {
                JsonArray ids;
                for (auto id : ruleset.value)
                    ids.must_append(id);
                disabled.set(ruleset.key, move(ids));
            }
            entry.set("disabledStaticRules"sv, move(disabled));
        }
        entry.set("dynamicRules"sv, rules_json(rules.dynamic_rules, {}));
        storage.set(rules.extension_id, move(entry));
    }
    return storage;
}

ErrorOr<void, String> ExtensionNetworkRules::save_storage(JsonObject const& storage)
{
    auto result = [&]() -> ErrorOr<void> {
        TRY(Core::Directory::create(LexicalPath { m_storage_path }.parent(), Core::Directory::CreateDirectories::Yes));
        auto temporary_path = ByteString::formatted("{}.tmp", m_storage_path);
        {
            auto file = TRY(Core::File::open(temporary_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
            auto serialized = storage.serialized();
            TRY(file->write_until_depleted(serialized.bytes()));
        }
        TRY(Core::System::rename(temporary_path, m_storage_path));
        return {};
    }();
    if (result.is_error()) {
        warnln("Unable to save the network rules of extensions to {}: {}", m_storage_path, result.error());
        return MUST(String::formatted("Unable to save network rules: {}", result.error()));
    }
    m_storage = storage;
    return {};
}

}
