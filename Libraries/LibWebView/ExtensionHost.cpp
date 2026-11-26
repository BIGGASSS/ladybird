/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibGfx/SystemTheme.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebCommon/Extensions/MessageDeliveryStatus.h>
#include <LibWebView/Application.h>
#include <LibWebView/ExtensionHost.h>
#include <LibWebView/ExtensionManager.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

using Web::Extensions::MessageDeliveryStatus;

static constexpr auto NO_RECEIVER_ERROR = "Could not establish connection. Receiving end does not exist."sv;

ExtensionHost::ExtensionHost(ExtensionManager& manager)
    : m_manager(manager)
{
}

ExtensionHost::~ExtensionHost() = default;

void ExtensionHost::start_background_pages(Optional<StringView> only_extension_id)
{
    if (only_extension_id.has_value())
        m_background_pages.remove(*only_extension_id);
    else
        m_background_pages.clear();

    auto theme_path = LexicalPath::join(s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = Gfx::load_system_theme(theme_path.string());
    if (theme.is_error()) {
        warnln("Unable to start the background pages of extensions: {}", theme.error());
        return;
    }

    for (auto const& extension : m_manager.extensions()) {
        if (only_extension_id.has_value() && extension.id != *only_extension_id)
            continue;
        auto url = m_manager.background_page_url(extension.id);
        if (!url.has_value())
            continue;

        auto view = HeadlessWebView::create(theme.value(), { 800, 600 });
        view->load_from_browser_ui(*url);
        m_background_pages.set(extension.id, move(view));
    }
}

void ExtensionHost::set_tab_of_popup(u64 popup_view_id, u64 tab_view_id)
{
    m_tabs_of_popups.remove_all_matching([](u64 popup_view_id, u64) { return !ViewImplementation::find_view_by_id(popup_view_id).has_value(); });
    m_tabs_of_popups.set(popup_view_id, tab_view_id);
}

bool ExtensionHost::is_tab(ViewImplementation const& view) const
{
    if (!view.is_tab() || view.is_private() == IsPrivate::Yes || view.owner_view_id().has_value() || m_tabs_of_popups.contains(view.view_id()))
        return false;
    for (auto const& background_page : m_background_pages) {
        if (background_page.value->view_id() == view.view_id())
            return false;
    }
    return true;
}

Vector<ViewImplementation&> ExtensionHost::tabs() const
{
    Vector<ViewImplementation&> tabs;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (is_tab(view))
            tabs.append(view);
        return IterationDecision::Continue;
    });
    return tabs;
}

Optional<ViewImplementation&> ExtensionHost::tab(u64 view_id) const
{
    auto view = ViewImplementation::find_view_by_id(view_id);
    if (!view.has_value() || !is_tab(*view))
        return {};
    return view;
}

// The current tab: that of the window a popup was opened from, or else the one the user looked at most recently.
Optional<ViewImplementation&> ExtensionHost::current_tab(Optional<ViewImplementation&> caller) const
{
    if (caller.has_value()) {
        if (auto tab_view_id = m_tabs_of_popups.get(caller->view_id()); tab_view_id.has_value())
            return tab(*tab_view_id);
    }

    if (auto active = Application::the().active_web_view(); active.has_value() && is_tab(*active))
        return active;

    // NB: Without a browser window, as in headless mode, no tab is ever current, so the only tab there is stands in.
    auto all_tabs = tabs();
    Optional<ViewImplementation&> current;
    for (auto& view : all_tabs) {
        if (view.is_current_tab_of_window() && (!current.has_value() || view.became_current_tab_of_window_order() > current->became_current_tab_of_window_order()))
            current = view;
    }
    if (!current.has_value() && all_tabs.size() == 1)
        current = all_tabs.first();
    return current;
}

// https://developer.chrome.com/docs/extensions/reference/api/tabs#type-Tab
// An extension may see the URL and title of a tab if it has the "tabs" permission, or host permissions for the tab's
// URL, as in Chrome.
bool ExtensionHost::may_see_url_of(String const& extension_id, ViewImplementation& view) const
{
    auto manifest = m_manager.manifest(extension_id);
    if (!manifest.has_value())
        return false;

    Vector<String> patterns;
    auto collect = [&](StringView key) {
        if (auto list = manifest->get_array(key); list.has_value()) {
            list->for_each([&](auto const& value) {
                if (value.is_string())
                    patterns.append(value.as_string());
            });
        }
    };
    collect("permissions"sv);
    collect("host_permissions"sv);

    auto url = view.url();
    // The extension's own pages are its own to see.
    if (url.scheme() == "chrome-extension"sv && url.serialized_host() == extension_id)
        return true;

    // NB: A host permission is for any path of its hosts, whatever path its pattern has.
    return any_of(patterns, [&](String const& pattern) {
        if (pattern == "tabs"sv)
            return true;
        auto host_start = pattern.find_byte_offset("://"sv);
        if (!host_start.has_value())
            return Web::Extensions::match_pattern_matches(pattern, url);
        auto path_start = pattern.bytes_as_string_view().find('/', *host_start + 3);
        if (!path_start.has_value())
            return false;
        auto any_path = ByteString::formatted("{}/*", pattern.bytes_as_string_view().substring_view(0, *path_start));
        return Web::Extensions::match_pattern_matches(any_path, url);
    });
}

JsonObject ExtensionHost::describe_tab(ViewImplementation& view, String const& extension_id) const
{
    auto all_tabs = tabs();
    auto index = all_tabs.find_first_index_if([&](auto const& tab) { return &tab == &view; }).value_or(0);

    JsonObject muted_info;
    muted_info.set("muted"sv, false);

    JsonObject tab;
    tab.set("id"sv, view.view_id());
    tab.set("index"sv, index);
    tab.set("windowId"sv, 1);
    if (may_see_url_of(extension_id, view)) {
        tab.set("url"sv, view.url().serialize());
        tab.set("title"sv, view.title().to_well_formed_utf8());
    }
    tab.set("active"sv, view.is_current_tab_of_window());
    tab.set("highlighted"sv, view.is_current_tab_of_window());
    tab.set("selected"sv, view.is_current_tab_of_window());
    tab.set("pinned"sv, false);
    tab.set("incognito"sv, false);
    tab.set("status"sv, view.is_loading() ? "loading"sv : "complete"sv);
    tab.set("audible"sv, view.is_playing_audio());
    tab.set("discarded"sv, false);
    tab.set("autoDiscardable"sv, true);
    tab.set("groupId"sv, -1);
    tab.set("mutedInfo"sv, move(muted_info));
    return tab;
}

static Optional<URL::URL> url_for_extension(String const& extension_id, Optional<String const&> url)
{
    // Relative URLs are relative to the extension's root, as in Chrome.
    auto base = URL::Parser::basic_parse(ByteString::formatted("chrome-extension://{}/", extension_id));
    if (!url.has_value())
        return {};
    auto result = URL::Parser::basic_parse(*url, *base);

    // An extension cannot run script in a tab with a javascript: URL, as in Chrome.
    if (!result.has_value() || result->scheme() == "javascript"sv)
        return {};
    return result;
}

void ExtensionHost::handle_api_call(WebContentClient& client, Web::PageId page_id, u64 context_id, u64 call_id, String const& extension_id, String const& method, String const& arguments_json)
{
    auto respond = [&](bool succeeded, Optional<JsonValue> result) {
        client.async_extension_api_call_completed(call_id, context_id, succeeded, result.has_value() ? result->serialized() : String {});
    };
    auto fail = [&](StringView message) {
        respond(false, JsonValue { MUST(String::from_utf8(message)) });
    };

    if (client.is_private() == IsPrivate::Yes || !m_manager.manifest(extension_id).has_value())
        return fail("The extension is not available"sv);

    auto arguments_value = JsonValue::from_string(arguments_json);
    if (arguments_value.is_error() || !arguments_value.value().is_array())
        return fail("Invalid arguments"sv);
    auto const& arguments = arguments_value.value().as_array();
    auto argument_object = [&](size_t index) -> JsonObject {
        if (index < arguments.size() && arguments.at(index).is_object())
            return arguments.at(index).as_object();
        return {};
    };
    auto argument_tab = [&](size_t index) -> Optional<ViewImplementation&> {
        if (index < arguments.size() && arguments.at(index).is_number())
            return tab(arguments.at(index).get_u64().value_or(0));
        return {};
    };

    Optional<ViewImplementation&> caller;
    if (auto* page = client.page(page_id); page && page->has_traversable())
        caller = page->view();

    // https://developer.chrome.com/docs/extensions/reference/api/tabs#method-query
    if (method == "tabs.query"sv) {
        auto query = argument_object(0);
        auto active = query.get_bool("active"sv);
        auto in_current_window = query.get_bool("currentWindow"sv).value_or(false) || query.get_bool("lastFocusedWindow"sv).value_or(false);

        Vector<String> url_patterns;
        if (auto url = query.get("url"sv); url.has_value()) {
            if (url->is_string())
                url_patterns.append(url->as_string());
            else if (url->is_array())
                url->as_array().for_each([&](auto const& pattern) { if (pattern.is_string()) url_patterns.append(pattern.as_string()); });
        }
        // NB: Tabs are found by their URL only where the extension may see it, as in Chrome.
        auto matches_url = [&](ViewImplementation& view) {
            if (url_patterns.is_empty())
                return true;
            return may_see_url_of(extension_id, view) && any_of(url_patterns, [&](auto const& pattern) { return Web::Extensions::match_pattern_matches(pattern, view.url()); });
        };

        Vector<ViewImplementation&> matching;
        if (active == true && in_current_window) {
            // NB: There are no windows to tell apart here, so the current window is the one of the current tab.
            if (auto current = current_tab(caller); current.has_value() && matches_url(*current))
                matching.append(*current);
        } else {
            for (auto& view : tabs()) {
                if (active.has_value() && view.is_current_tab_of_window() != *active)
                    continue;
                if (!matches_url(view))
                    continue;
                matching.append(view);
            }
        }

        JsonArray result;
        for (auto& view : matching)
            result.must_append(describe_tab(view, extension_id));
        return respond(true, move(result));
    }

    if (method == "tabs.get"sv) {
        auto view = argument_tab(0);
        if (!view.has_value())
            return fail("No tab with that ID"sv);
        return respond(true, describe_tab(*view, extension_id));
    }

    if (method == "tabs.create"sv) {
        auto properties = argument_object(0);
        auto url = url_for_extension(extension_id, properties.get_string("url"sv));
        if (properties.has("url"sv) && !url.has_value())
            return fail("Invalid URL"sv);
        auto activate = properties.get_bool("active"sv).value_or(true) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No;
        auto view = Application::the().open_blank_new_tab(activate);
        if (!view.has_value())
            return fail("Unable to open a tab"sv);
        if (url.has_value())
            view->load_from_browser_ui(*url);
        return respond(true, describe_tab(*view, extension_id));
    }

    // Without a tab ID, these act on the current tab.
    auto argument_tab_or_current = [&](size_t index) -> Optional<ViewImplementation&> {
        if (index < arguments.size() && !arguments.at(index).is_null())
            return argument_tab(index);
        return current_tab(caller);
    };

    if (method == "tabs.update"sv) {
        auto view = argument_tab_or_current(0);
        if (!view.has_value())
            return fail("No tab with that ID"sv);
        auto properties = argument_object(1);
        if (auto url = properties.get_string("url"sv); url.has_value()) {
            auto resolved_url = url_for_extension(extension_id, *url);
            if (!resolved_url.has_value())
                return fail("Invalid URL"sv);
            view->load_from_browser_ui(*resolved_url);
        }
        auto tab = describe_tab(*view, extension_id);
        // NB: The UI activates the tab after this answers, which the tab described here already shows.
        if (properties.get_bool("active"sv) == true && view->on_activate_tab) {
            view->on_activate_tab();
            tab.set("active"sv, true);
        }
        return respond(true, move(tab));
    }

    if (method == "tabs.reload"sv) {
        auto view = argument_tab_or_current(0);
        if (!view.has_value())
            return fail("No tab with that ID"sv);
        view->reload();
        return respond(true, {});
    }

    // https://developer.chrome.com/docs/extensions/reference/api/runtime#method-openOptionsPage
    if (method == "runtime.openOptionsPage"sv) {
        auto const& manifest = *m_manager.manifest(extension_id);
        Optional<String> page;
        if (auto options_ui = manifest.get_object("options_ui"sv); options_ui.has_value())
            page = options_ui->get_string("page"sv).copy();
        if (!page.has_value())
            page = manifest.get_string("options_page"sv).copy();
        if (!page.has_value())
            return fail("The extension has no options page"sv);

        auto url = url_for_extension(extension_id, *page);
        if (!url.has_value())
            return fail("The extension's options page has an invalid URL"sv);
        Application::the().open_url_in_new_tab(*url, Web::HTML::ActivateTab::Yes);
        return respond(true, {});
    }

    if (method == "popup.setPreferredSize"sv) {
        if (caller.has_value() && m_tabs_of_popups.contains(caller->view_id()) && arguments.size() == 2 && arguments.at(0).is_number() && arguments.at(1).is_number())
            Application::the().extension_popup_did_request_size(caller->view_id(), { arguments.at(0).get_i32().value_or(0), arguments.at(1).get_i32().value_or(0) });
        return respond(true, {});
    }

    if (method == "runtime.sendMessage"sv)
        return send_message(client, caller, context_id, call_id, extension_id, arguments);

    // https://developer.chrome.com/docs/extensions/reference/api/declarativeNetRequest
    if (method.starts_with_bytes("declarativeNetRequest."sv)) {
        auto& rules = m_manager.network_rules();
        if (!rules.extension_has_permission(extension_id))
            return fail("Extension does not have declarativeNetRequest permission"sv);
        auto settle = [&](ErrorOr<void, String> result) {
            if (result.is_error())
                return fail(result.error());
            respond(true, {});
        };
        if (method == "declarativeNetRequest.getDynamicRules"sv)
            return respond(true, rules.dynamic_rules(extension_id, argument_object(0)));
        if (method == "declarativeNetRequest.updateDynamicRules"sv)
            return settle(rules.update_dynamic_rules(extension_id, argument_object(0)));
        if (method == "declarativeNetRequest.getSessionRules"sv)
            return respond(true, rules.session_rules(extension_id, argument_object(0)));
        if (method == "declarativeNetRequest.updateSessionRules"sv)
            return settle(rules.update_session_rules(extension_id, argument_object(0)));
        if (method == "declarativeNetRequest.getEnabledRulesets"sv)
            return respond(true, rules.enabled_rulesets(extension_id));
        if (method == "declarativeNetRequest.updateEnabledRulesets"sv)
            return settle(rules.update_enabled_rulesets(extension_id, argument_object(0)));
        if (method == "declarativeNetRequest.getDisabledRuleIds"sv) {
            auto result = rules.disabled_rule_ids(extension_id, argument_object(0));
            if (result.is_error())
                return fail(result.error());
            return respond(true, result.release_value());
        }
        if (method == "declarativeNetRequest.updateStaticRules"sv)
            return settle(rules.update_static_rules(extension_id, argument_object(0)));
        if (method == "declarativeNetRequest.getAvailableStaticRuleCount"sv)
            return respond(true, JsonValue { rules.available_static_rule_count(extension_id) });
        if (method == "declarativeNetRequest.isRegexSupported"sv) {
            auto options = argument_object(0);
            JsonObject result;
            auto supported = DeclarativeNetRequest::is_regex_supported(options.get_string("regex"sv).value_or({}), options.get_bool("isCaseSensitive"sv).value_or(false));
            result.set("isSupported"sv, supported);
            if (!supported)
                result.set("reason"sv, "syntaxError"sv);
            return respond(true, move(result));
        }
        if (method == "declarativeNetRequest.testMatchOutcome"sv) {
            auto details = argument_object(0);
            auto url = URL::Parser::basic_parse(details.get_string("url"sv).value_or({}));
            auto type = DeclarativeNetRequest::resource_type_from_name(details.get_string("type"sv).value_or({}));
            if (!url.has_value() || !type.has_value())
                return fail("Invalid request details"sv);
            DeclarativeNetRequest::Request request { *url, details.get_string("method"sv).value_or("get"_string).to_ascii_lowercase(), *type, {} };
            if (auto initiator = URL::Parser::basic_parse(details.get_string("initiator"sv).value_or({})); initiator.has_value())
                request.initiator = initiator->origin();
            JsonObject result;
            result.set("matchedRules"sv, rules.matching_rules(extension_id, request, {}));
            return respond(true, move(result));
        }
    }

    // https://developer.chrome.com/docs/extensions/reference/api/scripting
    if (method.starts_with_bytes("scripting."sv)) {
        if (!m_manager.extension_has_scripting_permission(extension_id))
            return fail("Extension does not have scripting permission"sv);
        auto broadcast_and_settle = [&](ErrorOr<void, String> result) {
            if (result.is_error())
                return fail(result.error());
            WebContentClient::for_each_client([&](WebContentClient& target) {
                if (target.is_private() == IsPrivate::No)
                    target.async_set_extensions(m_manager.extensions_buffer());
                return IterationDecision::Continue;
            });
            respond(true, {});
        };
        auto argument_array = [&](size_t index) -> Optional<JsonArray const&> {
            if (index < arguments.size() && arguments.at(index).is_array())
                return arguments.at(index).as_array();
            return {};
        };
        auto argument_optional_object = [&](size_t index) -> ErrorOr<Optional<JsonObject const&>, String> {
            if (index >= arguments.size() || arguments.at(index).is_null())
                return Optional<JsonObject const&> {};
            if (!arguments.at(index).is_object())
                return "filter must be an object"_string;
            return arguments.at(index).as_object();
        };
        if (method == "scripting.registerContentScripts"sv) {
            auto scripts = argument_array(0);
            if (!scripts.has_value())
                return fail("scripts must be a list"sv);
            return broadcast_and_settle(m_manager.register_content_scripts(extension_id, *scripts));
        }
        if (method == "scripting.updateContentScripts"sv) {
            auto scripts = argument_array(0);
            if (!scripts.has_value())
                return fail("scripts must be a list"sv);
            return broadcast_and_settle(m_manager.update_content_scripts(extension_id, *scripts));
        }
        if (method == "scripting.getRegisteredContentScripts"sv) {
            auto filter = argument_optional_object(0);
            if (filter.is_error())
                return fail(filter.error());
            auto result = m_manager.registered_content_scripts(extension_id, filter.value());
            if (result.is_error())
                return fail(result.error());
            return respond(true, result.release_value());
        }
        if (method == "scripting.unregisterContentScripts"sv) {
            auto filter = argument_optional_object(0);
            if (filter.is_error())
                return fail(filter.error());
            return broadcast_and_settle(m_manager.unregister_content_scripts(extension_id, filter.value()));
        }
    }

    fail(MUST(String::formatted("{} is not supported", method)));
}

void ExtensionHost::evaluate_network_request(WebContentClient& client, u64 request_id, DeclarativeNetRequest::Request const& request, ExtensionNetworkRules::FrameContext const& frame)
{
    // NB: Extensions do not act on private browsing.
    if (client.is_private() == IsPrivate::Yes) {
        client.async_did_evaluate_extension_network_request(request_id, to_underlying(Web::Extensions::NetworkRequestOutcome::Allow), {});
        return;
    }
    auto outcome = m_manager.network_rules().evaluate(request, frame);
    client.async_did_evaluate_extension_network_request(request_id, to_underlying(outcome.outcome), move(outcome.redirect_url));
}

// https://developer.chrome.com/docs/extensions/reference/api/action#event-onClicked
void ExtensionHost::click_action(String const& extension_id, u64 tab_view_id)
{
    auto view = tab(tab_view_id);
    if (!view.has_value() || !m_manager.manifest(extension_id).has_value())
        return;

    JsonArray arguments;
    arguments.must_append(describe_tab(*view, extension_id));
    auto arguments_json = arguments.serialized();
    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.is_private() == IsPrivate::No)
            client.async_deliver_extension_event(extension_id, "action.onClicked"_string, arguments_json);
        return IterationDecision::Continue;
    });
}

// https://developer.chrome.com/docs/extensions/develop/concepts/messaging
void ExtensionHost::send_message(WebContentClient& client, Optional<ViewImplementation&> caller, u64 context_id, u64 call_id, String const& extension_id, JsonArray const& arguments)
{
    // The arguments are where the message goes ({tabId}), the message as JSON, the sender's URL, and whether the sender
    // is a top-level document.
    if (arguments.size() < 4 || !arguments.at(0).is_object() || !arguments.at(1).is_string() || !arguments.at(2).is_string()) {
        client.async_extension_api_call_completed(call_id, context_id, false, "\"Invalid arguments\""_string);
        return;
    }

    auto target_tab_id = arguments.at(0).as_object().get_u64("tabId"sv);
    auto only_top_level_frame = arguments.at(0).as_object().get_u64("frameId"sv) == 0u;
    auto const& message = arguments.at(1).as_string();

    JsonObject sender;
    sender.set("id"sv, extension_id);
    sender.set("url"sv, arguments.at(2).as_string());
    if (auto url = URL::Parser::basic_parse(arguments.at(2).as_string()); url.has_value() && !url->origin().is_opaque())
        sender.set("origin"sv, url->origin().serialize());
    if (caller.has_value() && is_tab(*caller)) {
        sender.set("tab"sv, describe_tab(*caller, extension_id));
        sender.set("frameId"sv, arguments.at(3).as_bool() ? 0 : 1);
    }
    auto sender_json = sender.serialized();

    auto delivery_id = ++m_next_delivery_id;
    PendingMessage pending { .sender = client, .call_id = call_id, .context_id = context_id };

    if (target_tab_id.has_value()) {
        // A message to a tab goes to the extension's content scripts in each of the pages that make up the tab.
        auto view = tab(*target_tab_id);
        if (!view.has_value()) {
            client.async_extension_api_call_completed(call_id, context_id, false, "\"No tab with that ID\""_string);
            return;
        }
        WebContentClient::for_each_client([&](WebContentClient& target) {
            target.for_each_page([&](WebContentPage& page) {
                if (page.has_traversable() && &page.view() == &view.value()) {
                    target.async_deliver_extension_message(delivery_id, extension_id, page.id(), only_top_level_frame, 0, message, sender_json);
                    ++pending.outstanding_replies;
                }
                return IterationDecision::Continue;
            });
            return IterationDecision::Continue;
        });
    } else {
        // Any other message goes to the extension's pages, except the one that sent it.
        WebContentClient::for_each_client([&](WebContentClient& target) {
            if (target.is_private() == IsPrivate::Yes)
                return IterationDecision::Continue;
            target.async_deliver_extension_message(delivery_id, extension_id, {}, false, &target == &client ? context_id : 0, message, sender_json);
            ++pending.outstanding_replies;
            return IterationDecision::Continue;
        });
    }

    m_pending_messages.set(delivery_id, move(pending));
    finish_message(delivery_id);
}

void ExtensionHost::did_handle_message(WebContentClient&, u64 delivery_id, u8 status_value, String const& response_json)
{
    auto pending = m_pending_messages.find(delivery_id);
    if (pending == m_pending_messages.end() || status_value > to_underlying(MessageDeliveryStatus::RespondedLater))
        return;

    auto status = static_cast<MessageDeliveryStatus>(status_value);
    if (status != MessageDeliveryStatus::RespondedLater && pending->value.outstanding_replies > 0)
        --pending->value.outstanding_replies;

    switch (status) {
    case MessageDeliveryStatus::NoListener:
        break;
    case MessageDeliveryStatus::NoResponse:
        pending->value.has_listener = true;
        break;
    case MessageDeliveryStatus::WillRespond:
        pending->value.has_listener = true;
        pending->value.will_respond = true;
        break;
    case MessageDeliveryStatus::Responded:
    case MessageDeliveryStatus::RespondedLater:
        pending->value.has_listener = true;
        if (!pending->value.has_responded) {
            pending->value.has_responded = true;
            // NB: The response is JSON, which the sender gets as a string, or nothing if it was undefined.
            auto result = response_json.is_empty() ? String {} : JsonValue { response_json }.serialized();
            if (auto sender = pending->value.sender.strong_ref())
                sender->async_extension_api_call_completed(pending->value.call_id, pending->value.context_id, true, result);
        }
        break;
    }

    finish_message(delivery_id);
}

void ExtensionHost::finish_message(u64 delivery_id)
{
    auto pending = m_pending_messages.find(delivery_id);
    if (pending == m_pending_messages.end() || pending->value.outstanding_replies > 0)
        return;

    // Every process has answered. Without a response or the promise of one, the sender learns that nobody responds.
    if (!pending->value.has_responded && !pending->value.will_respond) {
        if (auto sender = pending->value.sender.strong_ref()) {
            if (pending->value.has_listener)
                sender->async_extension_api_call_completed(pending->value.call_id, pending->value.context_id, true, String {});
            else
                sender->async_extension_api_call_completed(pending->value.call_id, pending->value.context_id, false, JsonValue { MUST(String::from_utf8(NO_RECEIVER_ERROR)) }.serialized());
        }
    }

    if (pending->value.has_responded || !pending->value.will_respond)
        m_pending_messages.remove(pending);
}

}
