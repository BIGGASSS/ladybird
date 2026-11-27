/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Forward.h>
#include <LibURL/URL.h>
#include <LibWebView/Export.h>
#include <LibWebView/ExtensionNetworkRules.h>
#include <LibWebView/ExtensionPackage.h>

namespace WebView {

struct InstalledExtension {
    String id;
    String name;
    String version;
    ByteString path;
};

// Chrome extensions are .crx files (or .zip archives) in the profile's Extensions directory. The content scripts of
// these run in the pages they match; their storage is kept here, and given to every process hosting web content.
class WEBVIEW_API ExtensionManager {
    AK_MAKE_NONCOPYABLE(ExtensionManager);

public:
    AK_ALLOC_WITH_KMALLOC;

    ExtensionManager(ByteString extensions_directory, ByteString storage_path);
    ~ExtensionManager();

    ByteString const& extensions_directory() const { return m_extensions_directory; }
    Vector<InstalledExtension> const& extensions() const { return m_extensions; }

    // Reads every extension in the extensions directory. Those that cannot be read are skipped.
    ErrorOr<void> load(ReadonlySpan<String> preferred_languages);

    // Copies the extension at the given path into the extensions directory, and reads every extension anew.
    ErrorOr<InstalledExtension> install(StringView path, ReadonlySpan<String> preferred_languages);

    Core::AnonymousBuffer const& extensions_buffer() const { return m_extensions_buffer; }
    u64 generation() const { return m_generation; }

    struct Resource {
        Core::AnonymousBuffer data;
        String mime_type;
    };
    Optional<Resource> resource(StringView extension_id, StringView path) const;

    // The manifest of the extension, with its user-visible strings translated.
    Optional<JsonObject const&> manifest(StringView extension_id) const;
    struct Action {
        String extension_id;
        String title;
        Optional<ByteBuffer> icon;
        Optional<URL::URL> popup_url;
    };
    // The toolbar buttons of the extensions that have one.
    Vector<Action> actions() const;

    ExtensionNetworkRules& network_rules() { return m_network_rules; }

    struct InstallEvent {
        String reason;
        Optional<String> previous_version;
    };
    Optional<InstallEvent> pending_install_event(StringView extension_id) const;
    void mark_install_event_delivered(StringView extension_id, StringView delivered_version);

    // https://developer.chrome.com/docs/extensions/reference/api/scripting
    struct ScriptingFile {
        String path;
        String content;
    };
    bool extension_has_scripting_permission(StringView extension_id) const;
    bool extension_has_active_tab_permission(StringView extension_id) const;
    bool extension_has_host_permission(StringView extension_id, URL::URL const& document_url, URL::Origin const& document_origin, Optional<URL::Origin> const& initiator_origin = {}) const;
    bool extension_can_run_content_script(StringView extension_id, URL::URL const& document_url, URL::Origin const& document_origin, Optional<URL::Origin> const& initiator_origin, bool is_top_level) const;
    ErrorOr<Vector<String>, String> normalize_scripting_file_paths(ReadonlySpan<String> raw_paths) const;
    ErrorOr<Vector<ScriptingFile>, String> resolve_scripting_js_files(StringView extension_id, ReadonlySpan<String> raw_paths) const;
    ErrorOr<Vector<ScriptingFile>, String> resolve_scripting_css_files(StringView extension_id, ReadonlySpan<String> raw_paths) const;
    ErrorOr<void, String> register_content_scripts(StringView extension_id, JsonArray const& scripts);
    ErrorOr<void, String> update_content_scripts(StringView extension_id, JsonArray const& scripts);
    ErrorOr<JsonArray, String> registered_content_scripts(StringView extension_id, Optional<JsonObject const&> filter) const;
    ErrorOr<void, String> unregister_content_scripts(StringView extension_id, Optional<JsonObject const&> filter);

    // The page that runs the extension's background scripts or service worker, if it has any.
    Optional<URL::URL> background_page_url(StringView extension_id) const;
    String storage_json() const { return m_storage.serialized(); }

    // Applies changes a process hosting web content made to an extension's storage, and saves the storage soon.
    void apply_storage_changes(StringView extension_id, StringView area, StringView changes_json);

private:
    struct RegisteredContentScript {
        String id;
        Vector<String> matches;
        Vector<String> exclude_matches;
        Vector<String> js;
        Vector<String> css;
        bool all_frames { false };
        bool match_origin_as_fallback { false };
        bool persist_across_sessions { true };
        String run_at { "document_idle"_string };
        String world { "ISOLATED"_string };
    };

    struct PersistedPendingInstallEvent {
        String target_version;
        InstallEvent event;
    };

    void load_storage();
    void schedule_storage_save();
    void save_storage();
    void load_content_scripts();
    ErrorOr<void, String> save_content_scripts(JsonObject const&) const;
    void load_known_versions();
    ErrorOr<void> save_known_versions(HashMap<String, String> const&, HashMap<String, PersistedPendingInstallEvent> const&) const;
    ErrorOr<Core::AnonymousBuffer, String> build_extensions_buffer(HashMap<String, Vector<RegisteredContentScript>> const&) const;
    ErrorOr<void, String> commit_content_scripts(StringView extension_id, Vector<RegisteredContentScript> next_scripts);
    Optional<String> extension_version(StringView extension_id) const;

    ByteString m_extensions_directory;
    ByteString m_storage_path;
    ByteString m_content_scripts_path;
    ByteString m_versions_path;
    Vector<InstalledExtension> m_extensions;
    HashMap<String, ExtensionPackage> m_packages;
    HashMap<String, JsonObject> m_manifests;
    HashMap<String, JsonObject> m_messages;
    HashMap<String, JsonObject> m_base_extension_data;
    HashMap<String, Vector<RegisteredContentScript>> m_registered_content_scripts;
    JsonObject m_persisted_content_scripts;
    HashMap<String, String> m_known_versions;
    HashMap<String, PersistedPendingInstallEvent> m_persisted_pending_install_events;
    HashMap<String, InstallEvent> m_pending_install_events;
    ExtensionNetworkRules m_network_rules;
    Core::AnonymousBuffer m_extensions_buffer;
    u64 m_generation { 0 };
    JsonObject m_storage;
    RefPtr<Core::Timer> m_storage_save_timer;
};

// Turns an extension package into what processes hosting web content need to know of it, or an error if the
// extension is not one we can run.
WEBVIEW_API ErrorOr<JsonObject> extension_data_for_web_content(ExtensionPackage const&, ReadonlySpan<String> preferred_languages);

}
