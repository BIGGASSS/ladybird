/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ExtensionRegistry.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLFrameSetElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/Layout/Node.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLHtmlElement);

HTMLHtmlElement::HTMLHtmlElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLHtmlElement::~HTMLHtmlElement() = default;

// AD-HOC: Content scripts that run at document_start do so once the document element exists, before anything else of
//         the document is parsed, as in Chrome.
void HTMLHtmlElement::post_connection()
{
    Base::post_connection();
    if (document().document_element() == this)
        Extensions::ExtensionRegistry::the().run_content_scripts(document(), Extensions::RunAt::DocumentStart);
}

void HTMLHtmlElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);
    publish_body_construction_facts();
}

void HTMLHtmlElement::publish_body_construction_facts()
{
    Optional<Layout::ForcedReadScope> read;
    for (auto* child = first_child(); child; child = child->next_sibling()) {
        if (!is<HTMLBodyElement>(*child) && !is<HTMLFrameSetElement>(*child))
            continue;
        auto& element = as<DOM::Element>(*child);
        CSS::record_element_construction_facts(element);
        // The box of an element that starts or stops being the document's body is built again.
        if (!element.has_layout_box())
            continue;
        if (!read.has_value())
            read.emplace(document());
        auto const* layout_node = element.unsafe_layout_node(*read);
        if (layout_node && layout_node->is_body() != (document().body() == &element))
            element.set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::StyleChange);
    }
}

}
