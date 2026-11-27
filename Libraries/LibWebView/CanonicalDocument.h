/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Weakable.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/HTML/CrossOrigin/OpenerPolicy.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/EmbedderPolicy.h>
#include <LibWebCommon/HTML/SandboxingFlagSet.h>
#include <LibWebCommon/PermissionsPolicy/PermissionsPolicy.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// https://dom.spec.whatwg.org/#concept-document
class WEBVIEW_API CanonicalDocument final
    : public RefCounted<CanonicalDocument>
    , public Weakable<CanonicalDocument> {
public:
    enum class IsInitialAboutBlank : bool {
        No,
        Yes,
    };

    static NonnullRefPtr<CanonicalDocument> create(Web::HTML::CrossProcessId id, URL::URL creation_url, URL::Origin, NonnullRefPtr<CanonicalBrowsingContext>, NonnullRefPtr<CanonicalWindow>, Web::HTML::SandboxingFlagSet active_sandboxing_flag_set, IsInitialAboutBlank);

    ~CanonicalDocument();

    Web::HTML::CrossProcessId id() const { return m_id; }

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-creation-url
    URL::URL const& creation_url() const { return m_creation_url; }

    // https://dom.spec.whatwg.org/#concept-document-origin
    URL::Origin const& origin() const { return m_origin; }
    void set_origin_for_testing(URL::Origin origin) { m_origin = move(origin); }

    // https://html.spec.whatwg.org/multipage/document-sequences.html#concept-document-bc
    CanonicalBrowsingContext& browsing_context() const { return m_browsing_context; }

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-relevant-global
    CanonicalWindow& relevant_global_object() const { return m_relevant_global_object; }

    // https://html.spec.whatwg.org/multipage/browsers.html#active-sandboxing-flag-set
    Web::HTML::SandboxingFlagSet active_sandboxing_flag_set() const { return m_active_sandboxing_flag_set; }

    // https://html.spec.whatwg.org/multipage/dom.html#is-initial-about:blank
    bool is_initial_about_blank() const { return m_is_initial_about_blank == IsInitialAboutBlank::Yes; }

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-coop
    Web::HTML::OpenerPolicy const& opener_policy() const { return m_opener_policy; }
    void set_opener_policy(Web::HTML::OpenerPolicy opener_policy) { m_opener_policy = move(opener_policy); }

    // https://html.spec.whatwg.org/multipage/browsers.html#policy-container-embedder-policy
    // The embedder policy of the document's policy container.
    Web::HTML::EmbedderPolicy const& embedder_policy() const { return m_embedder_policy; }
    void set_embedder_policy(Web::HTML::EmbedderPolicy embedder_policy) { m_embedder_policy = move(embedder_policy); }

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-permissions-policy
    Web::PermissionsPolicy::PermissionsPolicy const& permissions_policy() const { return m_permissions_policy; }
    void set_permissions_policy(Web::PermissionsPolicy::PermissionsPolicy permissions_policy) { m_permissions_policy = move(permissions_policy); }
    bool is_allowed_to_use(Web::PermissionsPolicy::PolicyControlledFeature) const;

    // https://html.spec.whatwg.org/multipage/dom.html#completely-loaded
    bool is_completely_loaded() const { return m_completely_loaded; }
    void set_completely_loaded() { m_completely_loaded = true; }

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#unload-counter
    // NB: Nonzero from when unload a document and its descendants starts for the document until the task that unloads
    //     it is queued, or until another page hosting it has unloaded it.
    u32 unload_counter() const { return m_unload_counter; }
    void increment_unload_counter() { ++m_unload_counter; }
    void decrement_unload_counter()
    {
        VERIFY(m_unload_counter > 0);
        --m_unload_counter;
    }

    // The origin of the top-level document of this document's browsing context, or nothing if that browsing context has
    // been discarded.
    Optional<URL::Origin> top_level_origin() const;

    RefPtr<WebContentPage> const& host() const { return m_host; }
    void set_host(RefPtr<WebContentPage>);

    bool is_top_level() const;

    // Whether this top-level document may be hosted by one process with the tabs of its origin. A cross-origin isolated
    // document may not. Other opener policies only keep cross-origin windows away.
    bool may_share_a_process_with_tabs_of_its_origin() const;

    // Whether this is local file content, whose process may read local files: a document created from a file: URL, or
    // from a blob: URL whose entry local file content added.
    bool is_local_file_content() const { return m_is_local_file_content; }
    void determine_whether_it_is_local_file_content(BlobURLStore const*);

    // The host of the WebUI the document displays, if it was created from the about: URL of a page with a channel to
    // the UI process.
    Optional<String> const& web_ui_host() const { return m_web_ui_host; }

    void make_active();

    HashTable<String>& authorized_content_script_extensions() { return m_authorized_content_script_extensions; }
    HashTable<String> const& authorized_content_script_extensions() const { return m_authorized_content_script_extensions; }
    bool has_snapshotted_content_script_extensions() const { return m_has_snapshotted_content_script_extensions; }
    void set_snapshotted_content_script_extensions() { m_has_snapshotted_content_script_extensions = true; }
    Optional<u64> extension_generation() const { return m_extension_generation; }
    void set_extension_generation(u64 generation) { m_extension_generation = generation; }
    void retain_pending_extension_generation();
    void release_pending_extension_generation();

private:
    CanonicalDocument(Web::HTML::CrossProcessId id, URL::URL creation_url, URL::Origin, NonnullRefPtr<CanonicalBrowsingContext>, NonnullRefPtr<CanonicalWindow>, Web::HTML::SandboxingFlagSet active_sandboxing_flag_set, IsInitialAboutBlank);

    Web::HTML::CrossProcessId m_id;
    URL::URL m_creation_url;
    URL::Origin m_origin;
    NonnullRefPtr<CanonicalBrowsingContext> m_browsing_context;
    NonnullRefPtr<CanonicalWindow> m_relevant_global_object;
    Web::HTML::SandboxingFlagSet m_active_sandboxing_flag_set {};
    IsInitialAboutBlank m_is_initial_about_blank { IsInitialAboutBlank::No };
    Web::HTML::OpenerPolicy m_opener_policy;
    Web::HTML::EmbedderPolicy m_embedder_policy;
    Web::PermissionsPolicy::PermissionsPolicy m_permissions_policy { Web::PermissionsPolicy::PermissionsPolicy::empty_permissions_policy() };
    bool m_completely_loaded { false };
    u32 m_unload_counter { 0 };
    bool m_is_local_file_content { false };
    Optional<String> m_web_ui_host;
    RefPtr<WebContentPage> m_host;
    HashTable<String> m_authorized_content_script_extensions;
    bool m_has_snapshotted_content_script_extensions { false };
    Optional<u64> m_extension_generation;
    bool m_retains_pending_extension_generation { false };
};

}
