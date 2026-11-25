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

    // The page that runs the extension's background scripts or service worker, if it has any.
    Optional<URL::URL> background_page_url(StringView extension_id) const;
    String storage_json() const { return m_storage.serialized(); }

    // Applies changes a process hosting web content made to an extension's storage, and saves the storage soon.
    void apply_storage_changes(StringView extension_id, StringView area, StringView changes_json);

private:
    void load_storage();
    void schedule_storage_save();
    void save_storage();

    ByteString m_extensions_directory;
    ByteString m_storage_path;
    Vector<InstalledExtension> m_extensions;
    HashMap<String, ExtensionPackage> m_packages;
    HashMap<String, JsonObject> m_manifests;
    HashMap<String, JsonObject> m_messages;
    Core::AnonymousBuffer m_extensions_buffer;
    JsonObject m_storage;
    RefPtr<Core::Timer> m_storage_save_timer;
};

// Turns an extension package into what processes hosting web content need to know of it, or an error if the
// extension is not one we can run.
WEBVIEW_API ErrorOr<JsonObject> extension_data_for_web_content(ExtensionPackage const&, ReadonlySpan<String> preferred_languages);

}
