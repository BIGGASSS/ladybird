/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>
#include <LibURL/URL.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/ExtensionNetworkRules.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class ExtensionManager;
class HeadlessWebView;

// What the browser does for the extensions that are installed: runs their background pages, answers what their API
// asks of the browser, such as which tabs there are, and passes their messages on.
class WEBVIEW_API ExtensionHost : public Weakable<ExtensionHost> {
    AK_MAKE_NONCOPYABLE(ExtensionHost);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ExtensionHost(ExtensionManager&);
    ~ExtensionHost();

    // (Re)starts the background page of every extension that has one, or of the given one.
    void start_background_pages(Optional<StringView> only_extension_id = {});

    void handle_api_call(WebContentClient&, Web::PageId, Web::HTML::CrossProcessId document_id, u64 context_id, u64 call_id, String const& extension_id, String const& method, String const& arguments_json);
    void did_handle_message(WebContentClient&, u64 delivery_id, u8 status, String const& response_json);

    void connect_port(WebContentClient&, Web::PageId, Web::HTML::CrossProcessId document_id, u64 context_id, u64 local_port_id, String const& extension_id, String const& name, Optional<u64> target_tab_id, Optional<u64> target_frame_id, Optional<String> target_document_id);
    void accept_port(WebContentClient&, u64 channel_id, Web::HTML::CrossProcessId document_id, u64 context_id, u64 local_port_id);
    void finish_open_port(WebContentClient&, u64 channel_id);
    void post_port_message(WebContentClient&, u64 context_id, u64 local_port_id, String const& message_json);
    void disconnect_port(WebContentClient&, u64 context_id, u64 local_port_id);
    void disconnect_context_ports(WebContentClient&, u64 context_id);
    void did_complete_script_injection(WebContentClient&, u64 injection_id, u32 target_index, bool succeeded, String const& result_json, String const& error);
    void did_complete_css_injection(WebContentClient&, u64 injection_id, u32 target_index, bool succeeded, String const& error);
    void on_client_disconnected(WebContentClient&);
    enum class RetainPendingGeneration : u8 {
        No,
        Yes,
    };
    void snapshot_content_script_authorization(CanonicalNavigable const&, CanonicalDocument&, Optional<URL::Origin> const& initiator_origin, RetainPendingGeneration = RetainPendingGeneration::No);
    void retain_pending_extension_generation(u64 generation);
    void release_pending_extension_generation(u64 generation);
    u64 minimum_retained_extension_generation() const;

    // An extension's popup acts for the tab of the window it was opened from.
    void set_tab_of_popup(u64 popup_view_id, u64 tab_view_id, String const& extension_id);

    // What the network rules of the extensions make of a request of a page.
    void evaluate_network_request(WebContentClient&, u64 request_id, DeclarativeNetRequest::Request const&, ExtensionNetworkRules::FrameContext const&);

    // The toolbar button of an extension that has no popup tells the extension's pages that it was clicked.
    void click_action(String const& extension_id, u64 tab_view_id);

    struct TabUpdate {
        Optional<StringView> status {};
        Optional<URL::URL> url {};
        Optional<Utf16String> title {};
    };
    void on_tab_created(ViewImplementation&);
    void on_tab_document_committed(ViewImplementation&);
    void on_frame_committed(ViewImplementation&, CanonicalNavigable const&);
    void on_frame_completed(ViewImplementation&, CanonicalNavigable const&);
    void on_frame_error_occurred(ViewImplementation&, CanonicalNavigable const&, URL::URL const& url, StringView error);
    void on_populated_document_abandoned(Web::HTML::CrossProcessId document_id);
    void on_tab_updated(ViewImplementation&, TabUpdate const&);
    void on_tab_activated(ViewImplementation&);
    void on_tab_removed(u64 view_id);
    void on_tab_close_declined(u64 view_id);
    void on_tab_crashed(u64 view_id);

private:
    Vector<ViewImplementation&> tabs() const;
    Optional<ViewImplementation&> tab(u64 view_id) const;
    Optional<ViewImplementation&> current_tab(Optional<ViewImplementation&> caller) const;
    bool is_tab(ViewImplementation const&) const;
    bool may_see_url_of(String const& extension_id, URL::URL const&) const;
    bool may_see_url_of(String const& extension_id, ViewImplementation&) const;
    JsonObject describe_tab(ViewImplementation&, String const& extension_id) const;
    void deliver_event(String const& extension_id, String const& name, JsonArray const& arguments) const;

    enum class PortDisconnectReason : u8 {
        Explicit,
        Passive,
    };
    void send_message(WebContentClient&, Web::PageId, Web::HTML::CrossProcessId document_id, u64 context_id, u64 call_id, String const& extension_id, JsonArray const& arguments);
    void deliver_send_message(WebContentClient&, ViewImplementation&, CanonicalNavigable const&, CanonicalDocument&, u64 context_id, u64 call_id, String const& extension_id, Optional<u64> target_tab_id, bool only_top_level_frame, String const& message);
    void flush_deferred_messages_for_document(Web::HTML::CrossProcessId document_id);
    void finish_message(u64 delivery_id);
    bool document_is_authorized_for_content_script(CanonicalNavigable const&, CanonicalDocument&, StringView extension_id);
    bool extension_has_permission(String const& extension_id, StringView permission) const;
    JsonObject build_port_sender(ViewImplementation&, CanonicalNavigable const&, CanonicalDocument const&, String const& extension_id) const;
    void maybe_finish_port_channel(u64 channel_id);
    void expire_port_open(u64 channel_id);
    void disconnect_port_endpoint_internal(WebContentClient const&, u64 local_port_id, Optional<u64> required_context_id, PortDisconnectReason);
    void finish_tab_remove(u64 remove_id);
    void fail_pending_tab_removes_for(u64 tab_id, StringView error_message);
    void grant_active_tab(String const& extension_id, u64 tab_view_id);
    void revoke_active_tab_grants_for_tab(u64 tab_view_id, Optional<URL::Origin> const& current_origin = {});
    bool has_active_tab_grant(StringView extension_id, u64 tab_view_id, URL::URL const& document_url, URL::Origin const& document_origin, Optional<URL::Origin> const& initiator_origin = {}) const;
    void execute_scripting_injection(WebContentClient&, u64 context_id, u64 call_id, String const& extension_id, StringView method, JsonObject const& injection);
    void finish_scripting_injection(u64 injection_id);

    ExtensionManager& m_manager;
    HashMap<String, NonnullOwnPtr<HeadlessWebView>> m_background_pages;
    HashMap<u64, u64> m_tabs_of_popups;
    HashTable<u64> m_known_tab_ids;
    HashTable<u64> m_closing_tab_ids;
    HashTable<u64> m_awaiting_close_tab_ids;

    struct PendingMessage {
        WeakPtr<WebContentClient> sender;
        u64 call_id { 0 };
        u64 context_id { 0 };
        size_t outstanding_replies { 0 };
        bool has_listener { false };
        bool will_respond { false };
        bool has_responded { false };
    };
    struct DeferredMessage {
        WeakPtr<WebContentClient> sender;
        WebContentClient const* sender_key { nullptr };
        Web::PageId page_id {};
        Web::HTML::CrossProcessId document_id {};
        u64 context_id { 0 };
        u64 call_id { 0 };
        String extension_id;
        Optional<u64> target_tab_id {};
        bool only_top_level_frame { false };
        String message;
    };
    Vector<DeferredMessage> m_deferred_messages;
    HashMap<u64, PendingMessage> m_pending_messages;
    u64 m_next_delivery_id { 0 };

    struct PendingTabRemove {
        WeakPtr<WebContentClient> sender;
        u64 call_id { 0 };
        u64 context_id { 0 };
        HashTable<u64> remaining_tab_ids;
        bool failed { false };
        String error_message {};
    };
    HashMap<u64, PendingTabRemove> m_pending_tab_removes;
    u64 m_next_tab_remove_id { 0 };

    struct PortEndpointKey {
        WebContentClient const* client { nullptr };
        u64 local_port_id { 0 };
        bool operator==(PortEndpointKey const&) const = default;
    };
    struct PortEndpointKeyTraits : public AK::Traits<PortEndpointKey> {
        static unsigned hash(PortEndpointKey const& key)
        {
            return pair_int_hash(ptr_hash(key.client), u64_hash(key.local_port_id));
        }
    };
    struct AuthorizedPortReceiver {
        WebContentClient const* client { nullptr };
        Web::HTML::CrossProcessId document_id {};
        bool operator==(AuthorizedPortReceiver const&) const = default;
    };
    struct AuthorizedPortReceiverTraits : public AK::Traits<AuthorizedPortReceiver> {
        static unsigned hash(AuthorizedPortReceiver const& key)
        {
            return pair_int_hash(ptr_hash(key.client), Traits<Web::HTML::CrossProcessId>::hash(key.document_id));
        }
    };
    struct PortEndpoint {
        WeakPtr<WebContentClient> client;
        WebContentClient const* client_key { nullptr };
        u64 context_id { 0 };
        u64 local_port_id { 0 };
    };
    struct PortChannel {
        u64 id { 0 };
        String extension_id;
        PortEndpoint opener;
        bool opener_disconnected { false };
        bool closed { false };
        bool ever_accepted { false };
        Vector<PortEndpoint> receivers {};
        HashMap<WebContentClient const*, size_t> pending_open_replies {};
        HashTable<AuthorizedPortReceiver, AuthorizedPortReceiverTraits> authorized_receivers {};
        Vector<String> queued_messages {};
        size_t queued_message_bytes { 0 };
        RefPtr<Core::Timer> open_timeout_timer {};
    };
    struct PendingScriptingInjection {
        enum class Kind : u8 {
            ExecuteScript,
            InsertCSS,
            RemoveCSS,
        };
        struct TargetState {
            WeakPtr<WebContentClient> client;
            WebContentClient const* client_key { nullptr };
            u64 frame_id { 0 };
            String document_id;
            bool completed { false };
            bool succeeded { false };
            Optional<String> result_json {};
            String error {};
        };
        Kind kind { Kind::ExecuteScript };
        WeakPtr<WebContentClient> caller;
        WebContentClient const* caller_key { nullptr };
        u64 call_id { 0 };
        u64 context_id { 0 };
        Vector<TargetState> targets {};
        size_t remaining { 0 };
    };
    HashMap<u64, PendingScriptingInjection> m_pending_scripting_injections;
    u64 m_next_scripting_injection_id { 0 };

    struct ActiveTabGrantKey {
        String extension_id;
        u64 tab_view_id { 0 };
        bool operator==(ActiveTabGrantKey const&) const = default;
    };
    struct ActiveTabGrantKeyTraits : public AK::Traits<ActiveTabGrantKey> {
        static unsigned hash(ActiveTabGrantKey const& key)
        {
            return pair_int_hash(key.extension_id.hash(), u64_hash(key.tab_view_id));
        }
    };
    HashMap<ActiveTabGrantKey, URL::Origin, ActiveTabGrantKeyTraits> m_active_tab_grants;

    HashMap<u64, PortChannel> m_port_channels;
    HashMap<PortEndpointKey, u64, PortEndpointKeyTraits> m_endpoint_to_channel;
    u64 m_next_port_channel_id { 0 };
    HashMap<u64, size_t> m_pending_document_generations;
};

}
