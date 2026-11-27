/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Forward.h>

namespace Web::Extensions {

// Where an extension's code runs in a document, with the extension's API.
class ExtensionContext final : public JS::Cell {
    GC_CELL(ExtensionContext, JS::Cell);
    GC_DECLARE_ALLOCATOR(ExtensionContext);

public:
    enum class Kind : u8 {
        // The isolated world of an extension's content scripts. It has a JavaScript realm of its own, whose global
        // object wraps the document's Window, so that the scripts share the DOM with the page, and nothing else.
        ContentScript,
        // A page of the extension, such as its background page or popup, whose own world gets the extension's API.
        ExtensionPage,
    };

    static GC::Ref<ExtensionContext> create_for_content_scripts(DOM::Document&, String extension_id);
    static GC::Ref<ExtensionContext> create_for_extension_page(DOM::Document&, String extension_id);

    u64 id() const { return m_id; }
    Kind kind() const { return m_kind; }
    String const& extension_id() const { return m_extension_id; }
    DOM::Document& document() const { return m_document; }
    JS::Realm& realm() const { return *m_realm; }
    bool is_detached() const { return m_detached; }

    // Runs a classic script in this context, and reports what it throws to the console.
    void run_script(Utf16String const& source, StringView filename);

    void set_api(GC::Ref<JS::Object> api, GC::Ref<JS::FunctionObject> dispatch);
    // Calls the API's dispatcher with the given arguments, and gives back what it returns, if it does not throw.
    Optional<JS::Value> dispatch(ReadonlySpan<JS::Value>);

    // The document is going away: let go of what this context keeps for it.
    void detach();

private:
    ExtensionContext(Kind, String extension_id, GC::Ref<DOM::Document>, GC::Ref<JS::Realm>, OwnPtr<JS::ExecutionContext>, GC::Ptr<Bindings::WrapperWorld>);

    virtual void visit_edges(Visitor&) override;

    u64 m_id { 0 };
    Kind m_kind;
    String m_extension_id;
    GC::Ref<DOM::Document> m_document;
    GC::Ref<JS::Realm> m_realm;
    OwnPtr<JS::ExecutionContext> m_realm_execution_context;
    GC::Ptr<Bindings::WrapperWorld> m_wrapper_world;
    GC::Ptr<JS::Object> m_api;
    GC::Ptr<JS::FunctionObject> m_dispatch;
    bool m_detached { false };
};

}
