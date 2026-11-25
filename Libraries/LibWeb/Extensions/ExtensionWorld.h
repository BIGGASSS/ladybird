/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Forward.h>

namespace Web::Extensions {

// The isolated world an extension's content scripts run in, in one document. It has a JavaScript realm of its own,
// whose global object wraps the document's Window, so that the scripts share the DOM with the page, and nothing else.
class ExtensionWorld final : public JS::Cell {
    GC_CELL(ExtensionWorld, JS::Cell);
    GC_DECLARE_ALLOCATOR(ExtensionWorld);

public:
    static GC::Ref<ExtensionWorld> create(DOM::Document&, String extension_id);

    String const& extension_id() const { return m_extension_id; }
    JS::Realm& realm() { return *m_realm_execution_context->realm(); }

    // Runs a classic script in this world, and reports what it throws to the page's console.
    void run_script(Utf16String const& source, StringView filename);

    void set_api(GC::Ref<JS::Object> api, GC::Ref<JS::FunctionObject> dispatch_storage_change);
    void dispatch_storage_change(StringView area, StringView changes_json);

    // The document is going away: let go of the wrappers this world keeps for it.
    void detach();

private:
    ExtensionWorld(String extension_id, NonnullOwnPtr<JS::ExecutionContext>, GC::Ref<Bindings::WrapperWorld>);

    virtual void visit_edges(Visitor&) override;

    String m_extension_id;
    NonnullOwnPtr<JS::ExecutionContext> m_realm_execution_context;
    GC::Ref<Bindings::WrapperWorld> m_wrapper_world;
    GC::Ptr<JS::Object> m_api;
    GC::Ptr<JS::FunctionObject> m_dispatch_storage_change;
};

}
