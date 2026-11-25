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
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibURL/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

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
    // Scripts of the "MAIN" world run in the page's own world, and get no extension API, as in Chrome.
    bool main_world { false };
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
    String manifest_json;
    String messages_json;
    String ui_language;
    Vector<ContentScript> content_scripts;
    HashMap<String, WebAccessibleResource> resources;
};

// The installed extensions, as the browser process told this process about them. Content scripts of these run in the
// documents they match, in the page's own JavaScript world, as this engine has no isolated worlds.
class WEB_API ExtensionRegistry {
    AK_MAKE_NONCOPYABLE(ExtensionRegistry);
    AK_MAKE_NONMOVABLE(ExtensionRegistry);

public:
    static ExtensionRegistry& the();

    bool has_extensions() const { return !m_extensions.is_empty(); }
    Vector<Extension> const& extensions() const { return m_extensions; }
    Extension const* extension(StringView id) const;

    ErrorOr<void> set_extensions_from_bytes(ReadonlyBytes);
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

    // The CSS of the content scripts that match the document, which the document adds to its user style sheet.
    Utf16String style_sheet_for_document(DOM::Document const&) const;

    // Runs the content scripts of every stage up to and including the given one that have not yet run in the document.
    void run_content_scripts(DOM::Document&, RunAt);

private:
    ExtensionRegistry() = default;

    bool content_script_matches(ContentScript const&, DOM::Document const&) const;
    void inject(DOM::Document&, Extension const&, Vector<ContentScript const*> const&);

    Vector<Extension> m_extensions;
    // Extension ID -> storage area name -> items.
    HashMap<String, HashMap<String, JsonObject>> m_storage;
};

WEB_API bool match_pattern_matches(StringView pattern, URL::URL const&);
WEB_API bool glob_matches(StringView glob, StringView text);

}
