/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/WeakPtr.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/ExtensionNetworkRules.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class ExtensionManager;
class HeadlessWebView;

// What the browser does for the extensions that are installed: runs their background pages, answers what their API
// asks of the browser, such as which tabs there are, and passes their messages on.
class WEBVIEW_API ExtensionHost {
    AK_MAKE_NONCOPYABLE(ExtensionHost);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ExtensionHost(ExtensionManager&);
    ~ExtensionHost();

    // (Re)starts the background page of every extension that has one, or of the given one.
    void start_background_pages(Optional<StringView> only_extension_id = {});

    void handle_api_call(WebContentClient&, Web::PageId, u64 context_id, u64 call_id, String const& extension_id, String const& method, String const& arguments_json);
    void did_handle_message(WebContentClient&, u64 delivery_id, u8 status, String const& response_json);

    // An extension's popup acts for the tab of the window it was opened from.
    void set_tab_of_popup(u64 popup_view_id, u64 tab_view_id);

    // What the network rules of the extensions make of a request of a page.
    void evaluate_network_request(WebContentClient&, u64 request_id, DeclarativeNetRequest::Request const&, ExtensionNetworkRules::FrameContext const&);

    // The toolbar button of an extension that has no popup tells the extension's pages that it was clicked.
    void click_action(String const& extension_id, u64 tab_view_id);

private:
    Vector<ViewImplementation&> tabs() const;
    Optional<ViewImplementation&> tab(u64 view_id) const;
    Optional<ViewImplementation&> current_tab(Optional<ViewImplementation&> caller) const;
    bool is_tab(ViewImplementation const&) const;
    bool may_see_url_of(String const& extension_id, ViewImplementation&) const;
    JsonObject describe_tab(ViewImplementation&, String const& extension_id) const;

    void send_message(WebContentClient&, Optional<ViewImplementation&> caller, u64 context_id, u64 call_id, String const& extension_id, JsonArray const& arguments);
    void finish_message(u64 delivery_id);

    ExtensionManager& m_manager;
    HashMap<String, NonnullOwnPtr<HeadlessWebView>> m_background_pages;
    HashMap<u64, u64> m_tabs_of_popups;

    struct PendingMessage {
        WeakPtr<WebContentClient> sender;
        u64 call_id { 0 };
        u64 context_id { 0 };
        size_t outstanding_replies { 0 };
        bool has_listener { false };
        bool will_respond { false };
        bool has_responded { false };
    };
    HashMap<u64, PendingMessage> m_pending_messages;
    u64 m_next_delivery_id { 0 };
};

}
