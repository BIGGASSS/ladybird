/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Function.h>
#include <LibGC/Root.h>
#include <LibURL/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWebCommon/Extensions/MessageDeliveryStatus.h>
#include <LibWebCommon/Extensions/NetworkRequestOutcome.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/Page/PageId.h>

namespace Web::Extensions {

// The point in a document's life at which a content script runs, as named by a manifest's "run_at".
enum class RunAt : u8 {
    DocumentStart,
    DocumentEnd,
    DocumentIdle,
};

struct ContentScriptSource {
    String path;
    Utf16String source;
};

struct ContentScript {
    Vector<String> matches;
    Vector<String> exclude_matches;
    Vector<String> include_globs;
    Vector<String> exclude_globs;
    bool all_frames { false };
    bool match_about_blank { false };
    bool match_origin_as_fallback { false };
    // Scripts of the "MAIN" world run in the page's own world, and get no extension API, as in Chrome.
    bool main_world { false };
    // Dynamically registered scripts (via chrome.scripting) only inject into pages covered by host permissions.
    bool is_dynamic { false };
    RunAt run_at { RunAt::DocumentIdle };
    Vector<ContentScriptSource> js;
    Utf16String css;
};

struct WebAccessibleResource {
    ByteBuffer data;
    String mime_type;
    // The pages that can use the resource, by the match patterns of their origins. Any page can, if this is empty.
    Vector<String> matches;
};

struct Extension {
    String id;
    HashMap<String, WebAccessibleResource> resources;
    Vector<String> sandboxed_pages;
    String sandbox_policy;
};

// The installed extensions, as the browser process told this process about them. Content scripts of these run in the
// documents they match, in the page's own JavaScript world, as this engine has no isolated worlds.
class WEB_API ExtensionRegistry {
    AK_MAKE_NONCOPYABLE(ExtensionRegistry);
    AK_MAKE_NONMOVABLE(ExtensionRegistry);

public:
    static ExtensionRegistry& the();

    bool has_extensions() const { return !m_extensions.is_empty(); }
    bool has_extensions_for_document(DOM::Document const&) const;
    Vector<Extension> const& extensions() const { return m_extensions; }
    Extension const* extension(StringView id) const;

    ErrorOr<void> set_extensions_from_bytes(ReadonlyBytes, u64 generation = 0, u64 min_retained_generation = 0);
    void set_storage(JsonObject);

    // The contents of an extension's storage area ("local" or "sync"), serialized as JSON.
    String storage_snapshot(StringView extension_id, StringView area) const;

    enum class ChangeSource : u8 {
        ThisProcess,
        OtherProcess,
    };
    // Applies changes of the form {"clear": bool, "remove": [keys], "set": {key: value}}, in that order, and tells the
    // documents of this process that use the extension. Changes made in this process are passed on to on_storage_change.
    void apply_storage_changes(StringView extension_id, StringView area, StringView changes_json, ChangeSource);
    Function<void(String const& extension_id, String const& area, String const& changes_json)> on_storage_change;

    Optional<WebAccessibleResource const&> web_accessible_resource(URL::URL const&, URL::Origin const& requester) const;

    // The other files of an extension, which its own pages load, come from the browser process when they are asked for.
    using ResourceCallback = GC::Ref<GC::Function<void(Optional<ReadonlyBytes>, StringView mime_type)>>;
    void load_resource(URL::URL const&, ResourceCallback);
    // The manifest's sandboxed pages are served with a content security policy with a sandbox, which gives them an
    // opaque origin. This is that policy, if the URL is of such a page.
    Optional<String> sandbox_policy_for(URL::URL const&) const;
    void did_load_resource(u64 request_id, Optional<ReadonlyBytes>, StringView mime_type);
    Function<void(u64 request_id, String const& extension_id, String const& path)> on_load_resource;

    // The CSS of the content scripts that match the document, which the document adds to its user style sheet.
    enum class ContentScriptKind : u8 {
        Static,
        Dynamic,
    };
    Utf16String style_sheet_for_document(DOM::Document const&, ContentScriptKind) const;
    u64 inserted_user_style_sheets_version(DOM::Document const&) const;
    void for_each_inserted_user_style_sheet(DOM::Document const&, Function<void(CSS::StyleSheetState&)> const&) const;
    void for_each_inserted_author_style_sheet(DOM::Document const&, Function<void(CSS::StyleSheetState&)> const&) const;

    // Runs the content scripts of every stage up to and including the given one that have not yet run in the document.
    // A page of an extension gets the extension's API first.
    void snapshot_extensions_for_document(DOM::Document const&, Optional<u64> generation = {});
    void did_destroy_document(DOM::Document const&);
    void cancel_pending_script_injections_for_document(DOM::Document const&);
    void run_content_scripts(DOM::Document&, RunAt);

    // On-demand injection (chrome.scripting.executeScript / insertCSS / removeCSS).
    void execute_script(u64 injection_id, u32 target_index, StringView extension_id, PageId, HTML::CrossProcessId document_id, bool main_world, bool inject_immediately, ReadonlySpan<String> file_paths, ReadonlySpan<String> file_sources, Optional<String> const& func_source, Optional<String> const& args_json);
    void insert_css(u64 injection_id, u32 target_index, StringView extension_id, PageId, HTML::CrossProcessId document_id, ReadonlySpan<String> css_keys, ReadonlySpan<String> css_sources, bool is_user_origin);
    void remove_css(u64 injection_id, u32 target_index, StringView extension_id, PageId, HTML::CrossProcessId document_id, ReadonlySpan<String> css_keys);
    Function<void(u64 injection_id, u32 target_index, bool succeeded, String const& result_json, String const& error)> on_script_injection_complete;
    Function<void(u64 injection_id, u32 target_index, bool succeeded, String const& error)> on_css_injection_complete;

    // What the API of an extension asks the browser process to do, such as querying tabs or sending a message, goes to
    // on_api_call, and its result comes back to did_complete_call().
    u64 call(u64 context_id, StringView method, StringView arguments_json);
    void did_complete_call(u64 call_id, u64 context_id, bool succeeded, StringView result_json);
    Function<void(PageId, HTML::CrossProcessId document_id, u64 context_id, u64 call_id, String const& extension_id, String const& method, String const& arguments_json)> on_api_call;

    // Tells the listeners of the extension's pages, or of the content scripts in the given page, of a message.
    struct MessageDeliveryResult {
        MessageDeliveryStatus status { MessageDeliveryStatus::NoListener };
        String response;
    };
    // https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest
    // What happens to a request of a page is up to the network rules of the extensions, which the browser process keeps.
    bool has_network_rules() const { return m_has_network_rules; }
    using NetworkRequestCallback = GC::Ref<GC::Function<void(NetworkRequestOutcome, Optional<URL::URL>)>>;
    struct AncestorFrame {
        URL::URL url;
        Optional<URL::Origin> initiator;
        bool is_top_level { false };
    };
    struct NetworkRequest {
        URL::URL url;
        String method;
        StringView resource_type;
        Optional<URL::Origin> initiator;
        Optional<String> initiator_extension_id;
        Vector<AncestorFrame> ancestor_frames;
    };
    void evaluate_network_request(PageId, NetworkRequest, NetworkRequestCallback);
    void did_evaluate_network_request(u64 request_id, NetworkRequestOutcome, Optional<URL::URL> redirect_url);
    Function<void(PageId, u64 request_id, NetworkRequest const&)> on_evaluate_network_request;

    // Tells the extension's pages of an event of its API, such as its toolbar button being clicked.
    void deliver_event(StringView extension_id, StringView name, StringView arguments_json);
    MessageDeliveryResult deliver_message(u64 delivery_id, StringView extension_id, Optional<PageId> content_scripts_of_page, bool only_top_level_frame, u64 excluded_context_id, StringView message_json, StringView sender_json);
    Function<void(u64 delivery_id, bool responded, String const& response_json)> on_message_response;

    // Long-lived message ports (chrome.runtime.connect / chrome.tabs.connect / chrome.runtime.onConnect).
    enum class NotifyLocalPorts : u8 {
        No,
        Yes,
    };
    u64 connect_port(u64 context_id, StringView name, Optional<u64> target_tab_id, Optional<u64> target_frame_id, Optional<String> target_document_id);
    bool is_port_deactivated(u64 context_id, u64 local_port_id) const;
    void post_port_message(u64 context_id, u64 local_port_id, StringView message_json);
    void disconnect_port(u64 context_id, u64 local_port_id);
    void disconnect_ports_of_context(u64 context_id, NotifyLocalPorts);
    void open_port(u64 channel_id, StringView extension_id, PageId page_id, HTML::CrossProcessId document_id, bool target_extension_page, u64 excluded_context_id, StringView name, StringView sender_json);
    void deliver_port_message(u64 context_id, u64 local_port_id, StringView message_json);
    void disconnect_port_endpoint(u64 context_id, u64 local_port_id, StringView error, bool notify = true);

    Function<void(PageId, HTML::CrossProcessId document_id, u64 context_id, u64 local_port_id, String const& extension_id, String const& name, Optional<u64> target_tab_id, Optional<u64> target_frame_id, Optional<String> target_document_id)> on_connect_port;
    Function<void(u64 channel_id, HTML::CrossProcessId document_id, u64 context_id, u64 local_port_id)> on_accept_port;
    Function<void(u64 channel_id)> on_finish_open_port;
    Function<void(u64 context_id, u64 local_port_id, String const& message_json)> on_post_port_message;
    Function<void(u64 context_id, u64 local_port_id)> on_disconnect_port;
    Function<void(u64 context_id)> on_disconnect_context_ports;

private:
    struct InjectionExtension {
        String id;
        String manifest_json;
        String messages_json;
        String ui_language;
        Vector<String> host_permissions;
        Vector<ContentScript> content_scripts;
    };
    struct InjectionGeneration : public RefCounted<InjectionGeneration> {
        u64 generation { 0 };
        Vector<InjectionExtension> extensions;
    };
    struct DocumentExtensionSnapshot {
        RefPtr<InjectionGeneration> generation;
        URL::URL creation_url;
    };
    struct InsertedCSS {
        String extension_id;
        String key;
        bool is_user_origin { false };
        NonnullRefPtr<CSS::StyleSheetState> sheet;
    };
    struct ScriptInjectionRequest {
        u64 injection_id { 0 };
        u32 target_index { 0 };
        String extension_id;
        PageId page_id { 0 };
        HTML::CrossProcessId document_id {};
        bool main_world { false };
        Vector<String> file_paths;
        Vector<String> file_sources;
        Optional<String> func_source;
        Optional<String> args_json;
    };
    struct PendingScriptInjection {
        u64 injection_id { 0 };
        u32 target_index { 0 };
        Optional<u64> file_batch_id {};
    };
    struct PendingFileBatch {
        u64 injection_id { 0 };
        u32 target_index { 0 };
        size_t remaining { 0 };
        String last_result_json {};
        String last_result_error {};
    };

    ExtensionRegistry() = default;

    GC::Ptr<DOM::Document> find_active_document(PageId, HTML::CrossProcessId document_id) const;
    bool take_pending_script_injection(DOM::Document const*, u64 injection_id, u32 target_index);
    bool is_pending_script_injection(DOM::Document const*, u64 injection_id, u32 target_index) const;
    void run_script_injection_now(DOM::Document&, ScriptInjectionRequest);
    void settle_file_batch_item(DOM::Document&, JS::Realm&, u64 batch_id, u64 injection_id, u32 target_index, bool is_last, bool fulfilled, JS::Value value, bool main_world);
    Optional<URL::URL> url_for_matching(ContentScript const&, DOM::Document const&, bool use_live_document_url = false) const;
    bool content_script_matches(InjectionExtension const&, ContentScript const&, DOM::Document const&, bool use_live_document_url = false) const;
    void for_each_context(Function<void(ExtensionContext&)>) const;
    GC::Ptr<ExtensionContext> context(u64 context_id) const;
    void inject(DOM::Document&, InjectionExtension const&, Vector<ContentScript const*> const&);

    Vector<Extension> m_extensions;
    NonnullRefPtr<InjectionGeneration> m_current_injection_generation { adopt_ref(*new InjectionGeneration) };
    OrderedHashMap<u64, NonnullRefPtr<InjectionGeneration>> m_recent_injection_generations;
    HashMap<DOM::Document const*, DocumentExtensionSnapshot> m_document_extension_snapshots;
    HashMap<DOM::Document const*, Vector<InsertedCSS>> m_document_inserted_css;
    HashMap<DOM::Document const*, u64> m_document_inserted_user_css_versions;
    HashMap<DOM::Document const*, Vector<ScriptInjectionRequest>> m_deferred_script_injections;
    HashMap<DOM::Document const*, Vector<PendingScriptInjection>> m_pending_script_injections;
    HashMap<u64, PendingFileBatch> m_pending_file_batches;
    u64 m_next_file_batch_id { 0 };
    HashMap<u64, GC::Root<GC::Function<void(Optional<ReadonlyBytes>, StringView)>>> m_pending_resource_loads;
    u64 m_next_resource_request_id { 0 };
    u64 m_next_call_id { 0 };
    u64 m_next_port_id { 0 };
    HashMap<u64, HashTable<u64>> m_context_active_ports;
    HashMap<u64, HashTable<u64>> m_context_deactivated_ports;
    bool m_has_network_rules { false };
    HashMap<u64, GC::Root<GC::Function<void(NetworkRequestOutcome, Optional<URL::URL>)>>> m_pending_network_requests;
    u64 m_next_network_request_id { 0 };
    // Extension ID -> storage area name -> items.
    HashMap<String, HashMap<String, JsonObject>> m_storage;
};

}
