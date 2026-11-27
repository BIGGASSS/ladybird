/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/DeferGC.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ExtensionContext.h>
#include <LibWeb/Extensions/ExtensionRegistry.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::Extensions {

GC_DEFINE_ALLOCATOR(ExtensionContext);

static u64 s_next_context_id = 0;

GC::Ref<ExtensionContext> ExtensionContext::create_for_content_scripts(DOM::Document& document, String extension_id)
{
    auto& window = *document.window();
    auto& settings = document.relevant_settings_object();
    auto& principal_realm = settings.realm();
    auto& vm = principal_realm.vm();
    auto& heap = vm.heap();

    // NB: Until the context holds on to it, the realm is reachable only through its execution context, which the GC
    //     does not see.
    GC::DeferGC defer_gc(heap);

    auto execution_context = Bindings::create_a_new_javascript_realm(
        vm,
        settings.agent_cluster_cross_origin_isolation_mode(),
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return Bindings::create_global_object_wrapper(realm, GC::Ref { window });
        },
        nullptr);

    auto& realm = *execution_context->realm();
    auto intrinsics = realm.create<Bindings::Intrinsics>(realm);
    auto wrapper_world = heap.allocate<Bindings::WrapperWorld>(Bindings::WrapperWorld::Type::Extension);
    realm.set_host_defined(heap.allocate<Bindings::HostDefined>(intrinsics, wrapper_world, principal_realm));
    Bindings::cache_global_object_wrapper(realm);

    vm.push_execution_context(*execution_context);
    MUST(Bindings::initialize_window_web_interfaces(window, realm));
    vm.pop_execution_context();

    return heap.allocate<ExtensionContext>(Kind::ContentScript, move(extension_id), document, realm, move(execution_context), wrapper_world);
}

GC::Ref<ExtensionContext> ExtensionContext::create_for_extension_page(DOM::Document& document, String extension_id)
{
    return document.heap().allocate<ExtensionContext>(Kind::ExtensionPage, move(extension_id), document, document.relevant_settings_object().realm(), nullptr, nullptr);
}

ExtensionContext::ExtensionContext(Kind kind, String extension_id, GC::Ref<DOM::Document> document, GC::Ref<JS::Realm> realm, OwnPtr<JS::ExecutionContext> realm_execution_context, GC::Ptr<Bindings::WrapperWorld> wrapper_world)
    : m_id(++s_next_context_id)
    , m_kind(kind)
    , m_extension_id(move(extension_id))
    , m_document(document)
    , m_realm(realm)
    , m_realm_execution_context(move(realm_execution_context))
    , m_wrapper_world(wrapper_world)
{
}

void ExtensionContext::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_realm);
    if (m_realm_execution_context)
        m_realm_execution_context->visit_edges(visitor);
    visitor.visit(m_wrapper_world);
    visitor.visit(m_api);
    visitor.visit(m_dispatch);
}

void ExtensionContext::run_script(Utf16String const& source, StringView filename)
{
    auto& realm = this->realm();
    auto& settings = HTML::principal_realm_settings_object(realm);
    auto script = HTML::ClassicScript::create_in_realm(filename.to_byte_string(), source, settings, realm, settings.api_base_url());

    // NB: What the script throws goes to the console, and not to the page, whose error handlers would get it.
    auto result = script->run(HTML::ClassicScript::RethrowErrors::Yes);
    if (result.is_error())
        HTML::report_exception_to_console(result.value(), realm, HTML::ErrorInPromise::No);
}

void ExtensionContext::set_api(GC::Ref<JS::Object> api, GC::Ref<JS::FunctionObject> dispatch)
{
    m_api = api;
    m_dispatch = dispatch;
}

Optional<JS::Value> ExtensionContext::dispatch(ReadonlySpan<JS::Value> arguments)
{
    if (!m_dispatch || m_detached)
        return {};

    auto& realm = this->realm();
    auto callback = realm.create<WebIDL::CallbackType>(*m_dispatch, realm);
    auto result = WebIDL::invoke_callback(*callback, JS::js_undefined(), WebIDL::ExceptionBehavior::Report, arguments);
    if (result.is_abrupt())
        return {};
    return result.value();
}

void ExtensionContext::detach()
{
    if (m_detached)
        return;
    m_detached = true;
    ExtensionRegistry::the().disconnect_ports_of_context(m_id, ExtensionRegistry::NotifyLocalPorts::No);
    m_api = nullptr;
    m_dispatch = nullptr;
    if (m_wrapper_world)
        m_wrapper_world->detach();
}

}
