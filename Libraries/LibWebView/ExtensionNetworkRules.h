/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/NetworkRequestOutcome.h>
#include <LibWebView/DeclarativeNetRequest.h>
#include <LibWebView/Export.h>

namespace WebView {

// The declarativeNetRequest rules of the installed extensions: the rulesets of their manifests that are enabled, and the
// dynamic rules (which are kept in the profile) and session rules that they add, which together decide what happens to
// the requests of pages.
class WEBVIEW_API ExtensionNetworkRules {
    AK_MAKE_NONCOPYABLE(ExtensionNetworkRules);

public:
    AK_ALLOC_WITH_KMALLOC;

    // Reads a file of an extension's package, such as one of its rulesets.
    using ReadPackageFile = Function<Optional<ByteBuffer>(StringView extension_id, StringView path)>;

    ExtensionNetworkRules(ByteString storage_path, ReadPackageFile);
    ~ExtensionNetworkRules();

    // Takes the extensions that are installed and their manifests.
    void set_extensions(Vector<String> const& extension_ids, Function<JsonObject const&(StringView)> manifest_for);
    // Records that an extension was just installed or updated, so it wins cross-extension redirect/upgrade ties.
    void mark_installed(StringView extension_id);

    bool has_rules() const;
    bool extension_has_permission(StringView extension_id) const;

    // https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest
    JsonArray dynamic_rules(StringView extension_id, JsonObject const& filter) const;
    ErrorOr<void, String> update_dynamic_rules(StringView extension_id, JsonObject const& options);
    JsonArray session_rules(StringView extension_id, JsonObject const& filter) const;
    ErrorOr<void, String> update_session_rules(StringView extension_id, JsonObject const& options);
    JsonArray enabled_rulesets(StringView extension_id) const;
    ErrorOr<void, String> update_enabled_rulesets(StringView extension_id, JsonObject const& options);
    ErrorOr<JsonArray, String> disabled_rule_ids(StringView extension_id, JsonObject const& options) const;
    ErrorOr<void, String> update_static_rules(StringView extension_id, JsonObject const& options);
    i64 available_static_rule_count(StringView extension_id) const;

    // The ancestor documents a request comes from (innermost first), which rules that allow all requests of a document
    // are checked against.
    struct AncestorFrame {
        URL::URL url;
        Optional<URL::Origin> initiator;
        bool is_top_level { false };
    };
    struct FrameContext {
        Vector<AncestorFrame> ancestors;
    };
    struct Outcome {
        Web::Extensions::NetworkRequestOutcome outcome { Web::Extensions::NetworkRequestOutcome::Allow };
        Optional<URL::URL> redirect_url;
    };
    Outcome evaluate(DeclarativeNetRequest::Request const&, FrameContext const&) const;

    // The rules of the extension that would act on the request, as testMatchOutcome() gives them.
    JsonArray matching_rules(StringView extension_id, DeclarativeNetRequest::Request const&, FrameContext const&) const;

private:
    struct Ruleset {
        String id;
        String path;
        bool enabled_by_default { false };
    };
    struct ExtensionRules {
        String extension_id;
        String version;
        u64 install_order { 0 };
        bool has_dnr_permission { false };
        bool has_dnr_with_host_access { false };
        Vector<String> host_permissions;
        Vector<Ruleset> rulesets;
        // The rulesets that the extension enabled or disabled, if it did, rather than those its manifest enables.
        Optional<Vector<String>> enabled_rulesets;
        HashMap<String, HashTable<i64>> disabled_static_rules;
        HashMap<String, DeclarativeNetRequest::RuleIndex> static_rules;
        DeclarativeNetRequest::RuleIndex dynamic_rules;
        DeclarativeNetRequest::RuleIndex session_rules;
    };

    struct Match {
        DeclarativeNetRequest::Rule const* rule { nullptr };
        String ruleset_id;
    };
    Match best_match(ExtensionRules const&, DeclarativeNetRequest::PreparedRequest&, FrameContext const&) const;

    ExtensionRules* find(StringView extension_id);
    ExtensionRules const* find(StringView extension_id) const;
    Vector<String> enabled_ruleset_ids(ExtensionRules const&) const;
    HashMap<String, DeclarativeNetRequest::RuleIndex> load_static_rules_for(ExtensionRules const&, ReadonlySpan<String> enabled) const;
    void rebuild_evaluation_order();
    void load_storage();
    JsonObject serialize_storage(ExtensionRules const* override_extension = nullptr) const;
    ErrorOr<void, String> save_storage(JsonObject const&);

    ByteString m_storage_path;
    ReadPackageFile m_read_package_file;
    Vector<ExtensionRules> m_extensions;
    // Indices into m_extensions ordered by most recently installed first.
    Vector<size_t> m_evaluation_order;
    u64 m_next_install_order { 0 };
    JsonObject m_storage;
};

}
