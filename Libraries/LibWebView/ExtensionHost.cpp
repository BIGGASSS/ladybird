/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebCommon/Extensions/MessageDeliveryStatus.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalSessionHistoryEntry.h>
#include <LibWebView/CanonicalTraversable.h>
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
    for (auto& view : tabs())
        m_known_tab_ids.set(view.view_id());
}

ExtensionHost::~ExtensionHost()
{
    for (auto& [_, channel] : m_port_channels) {
        if (channel.open_timeout_timer)
            channel.open_timeout_timer->stop();
    }
    m_port_channels.clear();
    m_endpoint_to_channel.clear();
    m_pending_messages.clear();
    m_pending_tab_removes.clear();
    m_closing_tab_ids.clear();
    m_awaiting_close_tab_ids.clear();
    m_known_tab_ids.clear();
    m_background_pages.clear();
}

void ExtensionHost::start_background_pages(Optional<StringView> only_extension_id)
{
    bool is_browser_startup = !only_extension_id.has_value();
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
        auto install_event = m_manager.pending_install_event(extension.id);
        auto url = m_manager.background_page_url(extension.id);
        if (!url.has_value()) {
            if (install_event.has_value())
                m_manager.mark_install_event_delivered(extension.id, extension.version);
            continue;
        }

        bool should_fire_startup = is_browser_startup && (!install_event.has_value() || install_event->reason != "install"sv);
        auto view = HeadlessWebView::create(theme.value(), { 800, 600 }, IsPrivate::No, HeadlessWebView::IsTab::No);
        auto* view_ptr = view.ptr();
        view->on_load_finish = [this, view_ptr, extension_id = extension.id, version = extension.version, expected_url = *url, install_event = move(install_event), should_fire_startup, fired = false](URL::URL const& loaded_url) mutable {
            if (fired || loaded_url != expected_url)
                return;
            if (!view_ptr->has_traversable() || view_ptr->traversable().active_document().creation_url() == URL::about_error())
                return;
            fired = true;
            if (install_event.has_value()) {
                JsonObject details;
                details.set("reason"sv, install_event->reason);
                if (install_event->previous_version.has_value())
                    details.set("previousVersion"sv, *install_event->previous_version);
                JsonArray arguments;
                arguments.must_append(move(details));
                deliver_event(extension_id, "runtime.onInstalled"_string, arguments);
                m_manager.mark_install_event_delivered(extension_id, version);
            }
            if (should_fire_startup)
                deliver_event(extension_id, "runtime.onStartup"_string, {});
        };
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
bool ExtensionHost::may_see_url_of(String const& extension_id, URL::URL const& url) const
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

bool ExtensionHost::may_see_url_of(String const& extension_id, ViewImplementation& view) const
{
    return may_see_url_of(extension_id, view.url());
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

    if (method == "tabs.remove"sv) {
        if (arguments.is_empty())
            return fail("Invalid tab ID"sv);
        Vector<u64> tab_ids;
        HashTable<u64> seen_ids;
        auto add_id = [&](JsonValue const& value) -> ErrorOr<void, String> {
            auto id = value.get_integer<u64>();
            if (!id.has_value())
                return "Invalid tab ID"_string;
            if (!tab(*id).has_value())
                return MUST(String::formatted("No tab with id: {}", *id));
            if (seen_ids.set(*id) == HashSetResult::InsertedNewEntry)
                tab_ids.append(*id);
            return {};
        };
        if (arguments.at(0).is_array()) {
            for (auto const& item : arguments.at(0).as_array().values()) {
                if (auto result = add_id(item); result.is_error())
                    return fail(result.error());
            }
        } else {
            if (auto result = add_id(arguments.at(0)); result.is_error())
                return fail(result.error());
        }
        if (tab_ids.is_empty())
            return respond(true, {});

        auto remove_id = ++m_next_tab_remove_id;
        PendingTabRemove pending { .sender = client, .call_id = call_id, .context_id = context_id, .remaining_tab_ids = move(seen_ids) };
        m_pending_tab_removes.set(remove_id, move(pending));

        for (auto tab_id : tab_ids) {
            // Coalesce overlapping remove calls for the same tab so only one unload check/close runs per tab.
            if (m_closing_tab_ids.set(tab_id) != HashSetResult::InsertedNewEntry)
                continue;

            auto view = tab(tab_id);
            if (!view.has_value()) {
                m_closing_tab_ids.remove(tab_id);
                if (auto it = m_pending_tab_removes.find(remove_id); it != m_pending_tab_removes.end())
                    it->value.remaining_tab_ids.remove(tab_id);
                continue;
            }
            if (!view->page().is_open() || !view->has_traversable()) {
                if (view->on_close) {
                    m_awaiting_close_tab_ids.set(tab_id);
                    view->on_close();
                } else if (m_known_tab_ids.contains(tab_id)) {
                    m_closing_tab_ids.remove(tab_id);
                    fail_pending_tab_removes_for(tab_id, "Unable to close tab"sv);
                }
                continue;
            }
            view->resume_debugger_if_paused();
            Vector<Web::HTML::CrossProcessId> inclusive_descendants;
            view->traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& descendant) {
                inclusive_descendants.append(descendant.id());
                return IterationDecision::Continue;
            });
            (void)view->traversable().check_if_unloading_is_canceled(
                move(inclusive_descendants), {}, {}, {}, Web::HTML::UnloadPromptShown::No,
                [weak_this = make_weak_ptr(), tab_id](Web::HTML::CheckIfUnloadingIsCanceledResult result, Web::HTML::UnloadPromptShown) {
                    if (!weak_this || !weak_this->m_closing_tab_ids.contains(tab_id))
                        return;
                    if (result != Web::HTML::CheckIfUnloadingIsCanceledResult::Continue) {
                        weak_this->m_closing_tab_ids.remove(tab_id);
                        weak_this->fail_pending_tab_removes_for(tab_id, "Tab close was canceled"sv);
                        return;
                    }
                    auto current = weak_this->tab(tab_id);
                    if (!current.has_value()) {
                        weak_this->m_closing_tab_ids.remove(tab_id);
                        return;
                    }
                    if (current->on_close) {
                        weak_this->m_awaiting_close_tab_ids.set(tab_id);
                        current->on_close();
                    } else if (current->page().is_open()) {
                        auto close = current->prepare_for_close_without_prompting();
                        close();
                    } else if (weak_this->m_known_tab_ids.contains(tab_id)) {
                        weak_this->m_closing_tab_ids.remove(tab_id);
                        weak_this->fail_pending_tab_removes_for(tab_id, "Unable to close tab"sv);
                    }
                });
        }
        finish_tab_remove(remove_id);
        return;
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
            auto min_retained = minimum_retained_extension_generation();
            WebContentClient::for_each_client([&](WebContentClient& target) {
                if (target.is_private() == IsPrivate::No)
                    target.async_set_extensions(m_manager.extensions_buffer(), m_manager.generation(), min_retained);
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

void ExtensionHost::deliver_event(String const& extension_id, String const& name, JsonArray const& arguments) const
{
    auto arguments_json = arguments.serialized();
    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.is_private() == IsPrivate::No)
            client.async_deliver_extension_event(extension_id, name, arguments_json);
        return IterationDecision::Continue;
    });
}

// https://developer.chrome.com/docs/extensions/reference/api/action#event-onClicked
void ExtensionHost::click_action(String const& extension_id, u64 tab_view_id)
{
    auto view = tab(tab_view_id);
    if (!view.has_value() || !m_manager.manifest(extension_id).has_value())
        return;

    JsonArray arguments;
    arguments.must_append(describe_tab(*view, extension_id));
    deliver_event(extension_id, "action.onClicked"_string, arguments);
}

void ExtensionHost::on_tab_created(ViewImplementation& view)
{
    if (!is_tab(view))
        return;
    if (m_known_tab_ids.set(view.view_id()) != HashSetResult::InsertedNewEntry)
        return;
    for (auto const& extension : m_manager.extensions()) {
        JsonArray arguments;
        arguments.must_append(describe_tab(view, extension.id));
        deliver_event(extension.id, "tabs.onCreated"_string, arguments);
    }
}

void ExtensionHost::on_tab_updated(ViewImplementation& view, TabUpdate const& update)
{
    if (!is_tab(view) || !m_known_tab_ids.contains(view.view_id()))
        return;
    for (auto const& extension : m_manager.extensions()) {
        JsonObject change_info;
        if (update.status.has_value())
            change_info.set("status"sv, *update.status);
        if (update.url.has_value() && may_see_url_of(extension.id, *update.url))
            change_info.set("url"sv, update.url->serialize());
        if (update.title.has_value() && may_see_url_of(extension.id, view))
            change_info.set("title"sv, update.title->to_well_formed_utf8());
        if (change_info.is_empty())
            continue;

        JsonArray arguments;
        arguments.must_append(view.view_id());
        arguments.must_append(move(change_info));
        arguments.must_append(describe_tab(view, extension.id));
        deliver_event(extension.id, "tabs.onUpdated"_string, arguments);
    }
}

void ExtensionHost::on_tab_activated(ViewImplementation& view)
{
    if (!is_tab(view) || !m_known_tab_ids.contains(view.view_id()))
        return;
    JsonObject active_info;
    active_info.set("tabId"sv, view.view_id());
    active_info.set("windowId"sv, 1);
    JsonArray arguments;
    arguments.must_append(move(active_info));
    for (auto const& extension : m_manager.extensions())
        deliver_event(extension.id, "tabs.onActivated"_string, arguments);
}

void ExtensionHost::finish_tab_remove(u64 remove_id)
{
    auto it = m_pending_tab_removes.find(remove_id);
    if (it == m_pending_tab_removes.end() || !it->value.remaining_tab_ids.is_empty())
        return;
    auto pending = move(it->value);
    m_pending_tab_removes.remove(it);
    if (auto sender = pending.sender.strong_ref()) {
        if (pending.failed)
            sender->async_extension_api_call_completed(pending.call_id, pending.context_id, false, JsonValue { pending.error_message }.serialized());
        else
            sender->async_extension_api_call_completed(pending.call_id, pending.context_id, true, String {});
    }
}

void ExtensionHost::fail_pending_tab_removes_for(u64 tab_id, StringView error_message)
{
    Vector<u64> completed_removes;
    for (auto& [remove_id, pending] : m_pending_tab_removes) {
        if (!pending.remaining_tab_ids.remove(tab_id))
            continue;
        if (!pending.failed) {
            pending.failed = true;
            pending.error_message = MUST(String::from_utf8(error_message));
        }
        if (pending.remaining_tab_ids.is_empty())
            completed_removes.append(remove_id);
    }
    for (auto remove_id : completed_removes)
        finish_tab_remove(remove_id);
}

void ExtensionHost::on_tab_removed(u64 view_id)
{
    m_closing_tab_ids.remove(view_id);
    m_awaiting_close_tab_ids.remove(view_id);
    if (!m_known_tab_ids.remove(view_id))
        return;
    JsonObject remove_info;
    remove_info.set("windowId"sv, 1);
    remove_info.set("isWindowClosing"sv, false);
    JsonArray arguments;
    arguments.must_append(view_id);
    arguments.must_append(move(remove_info));
    for (auto const& extension : m_manager.extensions())
        deliver_event(extension.id, "tabs.onRemoved"_string, arguments);

    Vector<u64> completed_removes;
    for (auto& [remove_id, pending] : m_pending_tab_removes) {
        if (pending.remaining_tab_ids.remove(view_id) && pending.remaining_tab_ids.is_empty())
            completed_removes.append(remove_id);
    }
    for (auto remove_id : completed_removes)
        finish_tab_remove(remove_id);
}

void ExtensionHost::on_tab_close_declined(u64 view_id)
{
    m_awaiting_close_tab_ids.remove(view_id);
    if (!m_closing_tab_ids.remove(view_id))
        return;
    fail_pending_tab_removes_for(view_id, "Tab close was canceled"sv);
}

void ExtensionHost::on_tab_crashed(u64 view_id)
{
    if (!m_closing_tab_ids.contains(view_id))
        return;
    // Defer so crash handlers that schedule tab removal (e.g. headless child tabs) settle via on_tab_removed() first.
    Core::deferred_invoke([weak_this = make_weak_ptr(), view_id] {
        if (!weak_this || weak_this->m_awaiting_close_tab_ids.contains(view_id))
            return;
        if (!weak_this->m_closing_tab_ids.remove(view_id))
            return;
        weak_this->fail_pending_tab_removes_for(view_id, "Tab renderer exited before closing"sv);
    });
}

JsonObject ExtensionHost::build_message_sender(Optional<ViewImplementation&> caller, String const& extension_id, String const& sender_url, bool sender_is_top_level) const
{
    JsonObject sender;
    sender.set("id"sv, extension_id);
    sender.set("url"sv, sender_url);
    if (auto url = URL::Parser::basic_parse(sender_url); url.has_value() && !url->origin().is_opaque())
        sender.set("origin"sv, url->origin().serialize());
    if (caller.has_value() && is_tab(*caller)) {
        sender.set("tab"sv, describe_tab(*caller, extension_id));
        sender.set("frameId"sv, sender_is_top_level ? 0 : 1);
    }
    return sender;
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
    auto sender_json = build_message_sender(caller, extension_id, arguments.at(2).as_string(), arguments.at(3).as_bool()).serialized();

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

static constexpr size_t MAX_PORT_MESSAGE_BYTES = 4 * MiB;
static constexpr size_t MAX_QUEUED_PORT_MESSAGE_BYTES = 16 * MiB;
static constexpr size_t MAX_QUEUED_PORT_MESSAGES = 1024;
static constexpr int PORT_OPEN_TIMEOUT_MS = 5000;
static constexpr StringView PORT_QUEUE_OVERFLOW_ERROR = "Port message queue limit exceeded while waiting for receiver."sv;

static bool is_non_sandboxed_extension_page(CanonicalDocument const& document, StringView extension_id)
{
    return !document.origin().is_opaque()
        && document.origin().scheme() == "chrome-extension"sv
        && document.origin().host().serialize() == extension_id;
}

void ExtensionHost::retain_pending_extension_generation(u64 generation)
{
    m_pending_document_generations.ensure(generation) += 1;
}

void ExtensionHost::release_pending_extension_generation(u64 generation)
{
    auto it = m_pending_document_generations.find(generation);
    if (it == m_pending_document_generations.end())
        return;
    if (it->value <= 1)
        m_pending_document_generations.remove(it);
    else
        --it->value;
}

u64 ExtensionHost::minimum_retained_extension_generation() const
{
    auto minimum = m_manager.generation();
    for (auto const& [generation, count] : m_pending_document_generations) {
        if (count > 0 && generation < minimum)
            minimum = generation;
    }
    return minimum;
}

void ExtensionHost::snapshot_content_script_authorization(CanonicalNavigable const& navigable, CanonicalDocument& document, Optional<URL::Origin> const& initiator_origin, RetainPendingGeneration retain)
{
    if (!document.has_snapshotted_content_script_extensions()) {
        document.set_snapshotted_content_script_extensions();
        document.set_extension_generation(m_manager.generation());
        auto is_top_level = navigable.is_top_level_traversable();
        for (auto const& extension : m_manager.extensions()) {
            if (m_manager.extension_can_run_content_script(extension.id, document.creation_url(), document.origin(), initiator_origin, is_top_level))
                document.authorized_content_script_extensions().set(extension.id);
        }
    }
    if (retain == RetainPendingGeneration::Yes)
        document.retain_pending_extension_generation();
}

bool ExtensionHost::document_is_authorized_for_content_script(CanonicalNavigable const& navigable, CanonicalDocument& document, StringView extension_id)
{
    Optional<URL::Origin> initiator_origin;
    if (auto const& entry = navigable.active_session_history_entry())
        initiator_origin = entry->document_state->initiator_origin;
    snapshot_content_script_authorization(navigable, document, initiator_origin);
    return document.authorized_content_script_extensions().contains(extension_id);
}

JsonObject ExtensionHost::build_port_sender(ViewImplementation& view, CanonicalNavigable const& navigable, CanonicalDocument const& document, String const& extension_id) const
{
    JsonObject sender;
    sender.set("id"sv, extension_id);
    sender.set("url"sv, navigable.active_document_url().serialize());
    if (!document.origin().is_opaque())
        sender.set("origin"sv, document.origin().serialize());
    sender.set("documentId"sv, MUST(String::formatted("{}", document.id())));
    if (is_tab(view)) {
        sender.set("tab"sv, describe_tab(view, extension_id));
        sender.set("frameId"sv, navigable.extension_frame_id());
    }
    return sender;
}

void ExtensionHost::connect_port(WebContentClient& client, Web::PageId page_id, Web::HTML::CrossProcessId document_id, u64 context_id, u64 local_port_id, String const& extension_id, String const& name, Optional<u64> target_tab_id, Optional<u64> target_frame_id, Optional<String> target_document_id)
{
    auto disconnect_opener = [&](StringView error) {
        if (client.is_open())
            client.async_disconnect_extension_port_endpoint(context_id, local_port_id, MUST(String::from_utf8(error)), true);
    };

    if (client.is_private() == IsPrivate::Yes || !m_manager.manifest(extension_id).has_value() || local_port_id == 0)
        return disconnect_opener(NO_RECEIVER_ERROR);

    PortEndpointKey opener_key { &client, local_port_id };
    if (m_endpoint_to_channel.contains(opener_key))
        return;

    auto* page = client.page(page_id);
    if (!page || !page->is_open() || !page->has_traversable())
        return disconnect_opener(NO_RECEIVER_ERROR);

    auto& caller_view = page->view();
    CanonicalNavigable* opener_navigable = nullptr;
    caller_view.traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable& navigable) {
        auto& active_doc = navigable.active_document();
        if (active_doc.id() == document_id && active_doc.host().ptr() == page) {
            opener_navigable = &navigable;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (!opener_navigable)
        return disconnect_opener(NO_RECEIVER_ERROR);

    auto& opener_document = opener_navigable->active_document();
    bool opener_is_extension_page = is_non_sandboxed_extension_page(opener_document, extension_id);
    bool opener_is_content_script = !opener_is_extension_page && is_tab(caller_view) && document_is_authorized_for_content_script(*opener_navigable, opener_document, extension_id);
    if (!opener_is_extension_page && !opener_is_content_script)
        return disconnect_opener(NO_RECEIVER_ERROR);

    if (target_tab_id.has_value() && !opener_is_extension_page)
        return disconnect_opener(NO_RECEIVER_ERROR);
    if (!target_tab_id.has_value() && (target_frame_id.has_value() || target_document_id.has_value()))
        return disconnect_opener(NO_RECEIVER_ERROR);

    auto sender_json = build_port_sender(caller_view, *opener_navigable, opener_document, extension_id).serialized();
    auto channel_id = ++m_next_port_channel_id;
    PortChannel channel {
        .id = channel_id,
        .extension_id = extension_id,
        .opener = { .client = client, .client_key = &client, .context_id = context_id, .local_port_id = local_port_id },
    };

    auto authorize_and_open = [&](CanonicalDocument const& target_doc, bool target_extension_page, u64 excluded_context_id) {
        auto const& host = target_doc.host();
        if (!host || !host->is_open())
            return;
        auto& target_client = host->client();
        if (!target_client.is_open() || target_client.is_private() == IsPrivate::Yes)
            return;
        AuthorizedPortReceiver authorized { &target_client, target_doc.id() };
        if (channel.authorized_receivers.set(authorized) != HashSetResult::InsertedNewEntry)
            return;
        target_client.async_open_extension_port(channel_id, extension_id, host->id(), target_doc.id(), target_extension_page, excluded_context_id, name, sender_json);
        channel.pending_open_replies.set(&target_client, channel.pending_open_replies.get(&target_client).value_or(0) + 1);
    };

    if (target_tab_id.has_value()) {
        auto view = tab(*target_tab_id);
        if (!view.has_value() || !view->has_traversable())
            return disconnect_opener(NO_RECEIVER_ERROR);
        view->traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable& navigable) {
            auto& target_doc = navigable.active_document();
            if (target_frame_id.has_value() && navigable.extension_frame_id() != *target_frame_id)
                return IterationDecision::Continue;
            if (target_document_id.has_value() && MUST(String::formatted("{}", target_doc.id())) != *target_document_id)
                return IterationDecision::Continue;
            if (!document_is_authorized_for_content_script(navigable, target_doc, extension_id))
                return IterationDecision::Continue;
            authorize_and_open(target_doc, false, 0);
            return IterationDecision::Continue;
        });
    } else {
        ViewImplementation::for_each_view([&](ViewImplementation& view) {
            if (view.is_private() == IsPrivate::Yes || !view.has_traversable())
                return IterationDecision::Continue;
            view.traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable& navigable) {
                auto& target_doc = navigable.active_document();
                if (target_doc.id() == opener_document.id())
                    return IterationDecision::Continue;
                if (!is_non_sandboxed_extension_page(target_doc, extension_id))
                    return IterationDecision::Continue;
                auto const& host = target_doc.host();
                u64 excluded = (host && &host->client() == &client) ? context_id : 0;
                authorize_and_open(target_doc, true, excluded);
                return IterationDecision::Continue;
            });
            return IterationDecision::Continue;
        });
    }

    if (channel.pending_open_replies.is_empty())
        return disconnect_opener(NO_RECEIVER_ERROR);

    channel.open_timeout_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(PORT_OPEN_TIMEOUT_MS), [weak_this = make_weak_ptr(), channel_id] {
        if (weak_this)
            weak_this->expire_port_open(channel_id);
    });
    channel.open_timeout_timer->start();

    m_endpoint_to_channel.set(opener_key, channel_id);
    m_port_channels.set(channel_id, move(channel));
}

void ExtensionHost::accept_port(WebContentClient& client, u64 channel_id, Web::HTML::CrossProcessId document_id, u64 context_id, u64 local_port_id)
{
    if (client.is_private() == IsPrivate::Yes || local_port_id == 0)
        return;

    auto it = m_port_channels.find(channel_id);
    if (it == m_port_channels.end() || !it->value.authorized_receivers.remove({ &client, document_id })) {
        if (client.is_open())
            client.async_disconnect_extension_port_endpoint(context_id, local_port_id, String {}, true);
        return;
    }

    auto& channel = it->value;
    PortEndpointKey receiver_key { &client, local_port_id };
    if (m_endpoint_to_channel.contains(receiver_key))
        return;

    channel.ever_accepted = true;
    if (channel.closed || channel.opener_disconnected) {
        if (client.is_open())
            client.async_disconnect_extension_port_endpoint(context_id, local_port_id, String {}, true);
        return;
    }

    channel.receivers.append({ .client = client, .client_key = &client, .context_id = context_id, .local_port_id = local_port_id });
    m_endpoint_to_channel.set(receiver_key, channel_id);
    if (client.is_open()) {
        for (auto const& message : channel.queued_messages)
            client.async_deliver_extension_port_message(context_id, local_port_id, message);
    }
}

void ExtensionHost::finish_open_port(WebContentClient& client, u64 channel_id)
{
    auto it = m_port_channels.find(channel_id);
    if (it == m_port_channels.end())
        return;
    auto reply_it = it->value.pending_open_replies.find(&client);
    if (reply_it == it->value.pending_open_replies.end())
        return;
    if (reply_it->value <= 1)
        it->value.pending_open_replies.remove(reply_it);
    else
        --reply_it->value;
    maybe_finish_port_channel(channel_id);
}

void ExtensionHost::expire_port_open(u64 channel_id)
{
    auto it = m_port_channels.find(channel_id);
    if (it == m_port_channels.end())
        return;
    auto& channel = it->value;
    if (channel.open_timeout_timer) {
        channel.open_timeout_timer->stop();
        channel.open_timeout_timer = nullptr;
    }
    channel.pending_open_replies.clear();
    channel.authorized_receivers.clear();
    channel.queued_messages.clear();
    channel.queued_message_bytes = 0;
    maybe_finish_port_channel(channel_id);
}

void ExtensionHost::maybe_finish_port_channel(u64 channel_id)
{
    auto it = m_port_channels.find(channel_id);
    if (it == m_port_channels.end() || !it->value.pending_open_replies.is_empty())
        return;

    auto& channel = it->value;
    if (channel.open_timeout_timer) {
        channel.open_timeout_timer->stop();
        channel.open_timeout_timer = nullptr;
    }
    channel.authorized_receivers.clear();
    channel.queued_messages.clear();
    channel.queued_message_bytes = 0;

    if (channel.closed || channel.opener_disconnected) {
        auto receivers = move(channel.receivers);
        for (auto const& receiver : receivers) {
            m_endpoint_to_channel.remove({ receiver.client_key, receiver.local_port_id });
            if (auto receiver_client = receiver.client.strong_ref(); receiver_client && receiver_client->is_open())
                receiver_client->async_disconnect_extension_port_endpoint(receiver.context_id, receiver.local_port_id, String {}, true);
        }
        m_port_channels.remove(it);
        return;
    }

    if (channel.receivers.is_empty()) {
        m_endpoint_to_channel.remove({ channel.opener.client_key, channel.opener.local_port_id });
        if (auto opener_client = channel.opener.client.strong_ref(); opener_client && opener_client->is_open()) {
            auto error = channel.ever_accepted ? String {} : MUST(String::from_utf8(NO_RECEIVER_ERROR));
            opener_client->async_disconnect_extension_port_endpoint(channel.opener.context_id, channel.opener.local_port_id, error, true);
        }
        m_port_channels.remove(it);
    }
}

void ExtensionHost::post_port_message(WebContentClient& client, u64 context_id, u64 local_port_id, String const& message_json)
{
    if (client.is_private() == IsPrivate::Yes)
        return;

    auto message_size = message_json.bytes().size();
    if (message_size > MAX_PORT_MESSAGE_BYTES)
        return;

    auto channel_id = m_endpoint_to_channel.get({ &client, local_port_id });
    if (!channel_id.has_value())
        return;
    auto it = m_port_channels.find(*channel_id);
    if (it == m_port_channels.end())
        return;

    auto& channel = it->value;
    if (channel.opener.client_key == &client && channel.opener.local_port_id == local_port_id) {
        if (channel.opener.context_id != context_id || channel.closed || channel.opener_disconnected)
            return;
        if (!channel.pending_open_replies.is_empty()) {
            if (channel.queued_messages.size() >= MAX_QUEUED_PORT_MESSAGES || channel.queued_message_bytes > MAX_QUEUED_PORT_MESSAGE_BYTES - message_size) {
                channel.closed = true;
                if (channel.open_timeout_timer) {
                    channel.open_timeout_timer->stop();
                    channel.open_timeout_timer = nullptr;
                }
                channel.queued_messages.clear();
                channel.queued_message_bytes = 0;
                auto error = MUST(String::from_utf8(PORT_QUEUE_OVERFLOW_ERROR));
                if (!channel.opener_disconnected) {
                    channel.opener_disconnected = true;
                    m_endpoint_to_channel.remove({ channel.opener.client_key, channel.opener.local_port_id });
                    if (auto opener_client = channel.opener.client.strong_ref(); opener_client && opener_client->is_open())
                        opener_client->async_disconnect_extension_port_endpoint(channel.opener.context_id, channel.opener.local_port_id, error, true);
                }
                auto receivers = move(channel.receivers);
                for (auto const& receiver : receivers) {
                    m_endpoint_to_channel.remove({ receiver.client_key, receiver.local_port_id });
                    if (auto receiver_client = receiver.client.strong_ref(); receiver_client && receiver_client->is_open())
                        receiver_client->async_disconnect_extension_port_endpoint(receiver.context_id, receiver.local_port_id, error, true);
                }
                m_port_channels.remove(it);
                return;
            }
            channel.queued_messages.append(message_json);
            channel.queued_message_bytes += message_size;
        }
        for (auto const& receiver : channel.receivers) {
            if (auto receiver_client = receiver.client.strong_ref(); receiver_client && receiver_client->is_open())
                receiver_client->async_deliver_extension_port_message(receiver.context_id, receiver.local_port_id, message_json);
        }
        return;
    }

    for (auto const& receiver : channel.receivers) {
        if (receiver.client_key == &client && receiver.local_port_id == local_port_id) {
            if (receiver.context_id != context_id || channel.closed || channel.opener_disconnected)
                return;
            if (auto opener_client = channel.opener.client.strong_ref(); opener_client && opener_client->is_open())
                opener_client->async_deliver_extension_port_message(channel.opener.context_id, channel.opener.local_port_id, message_json);
            return;
        }
    }
}

void ExtensionHost::disconnect_port_endpoint_internal(WebContentClient const& client, u64 local_port_id, Optional<u64> required_context_id, PortDisconnectReason reason)
{
    PortEndpointKey key { &client, local_port_id };
    auto channel_id = m_endpoint_to_channel.get(key);
    if (!channel_id.has_value())
        return;
    auto it = m_port_channels.find(*channel_id);
    if (it == m_port_channels.end()) {
        m_endpoint_to_channel.remove(key);
        return;
    }

    auto& channel = it->value;
    auto close_channel = [&](Optional<PortEndpointKey> skip_receiver_key) {
        channel.closed = true;
        if (channel.open_timeout_timer) {
            channel.open_timeout_timer->stop();
            channel.open_timeout_timer = nullptr;
        }
        channel.queued_messages.clear();
        channel.queued_message_bytes = 0;

        if (!channel.opener_disconnected) {
            channel.opener_disconnected = true;
            m_endpoint_to_channel.remove({ channel.opener.client_key, channel.opener.local_port_id });
            if (skip_receiver_key.has_value()) {
                if (auto opener_client = channel.opener.client.strong_ref(); opener_client && opener_client->is_open())
                    opener_client->async_disconnect_extension_port_endpoint(channel.opener.context_id, channel.opener.local_port_id, String {}, true);
            }
        }

        // When a receiver explicitly disconnects, Chrome only fires onDisconnect on the opener; sibling receivers are
        // closed silently.
        bool notify_receivers = !skip_receiver_key.has_value();
        auto receivers = move(channel.receivers);
        for (auto const& receiver : receivers) {
            PortEndpointKey receiver_key { receiver.client_key, receiver.local_port_id };
            m_endpoint_to_channel.remove(receiver_key);
            if (skip_receiver_key.has_value() && receiver_key == *skip_receiver_key)
                continue;
            if (auto receiver_client = receiver.client.strong_ref(); receiver_client && receiver_client->is_open())
                receiver_client->async_disconnect_extension_port_endpoint(receiver.context_id, receiver.local_port_id, String {}, notify_receivers);
        }

        // Discard channel and any remaining pending receiver state immediately on close; late accept_port replies
        // for unknown channels are automatically disconnected.
        m_port_channels.remove(it);
    };

    if (channel.opener.client_key == &client && channel.opener.local_port_id == local_port_id) {
        if (required_context_id.has_value() && channel.opener.context_id != *required_context_id)
            return;
        close_channel({});
        return;
    }

    for (size_t index = 0; index < channel.receivers.size(); ++index) {
        auto const& receiver = channel.receivers[index];
        if (receiver.client_key != &client || receiver.local_port_id != local_port_id)
            continue;
        if (required_context_id.has_value() && receiver.context_id != *required_context_id)
            return;
        if (reason == PortDisconnectReason::Explicit) {
            close_channel(key);
            return;
        }
        m_endpoint_to_channel.remove(key);
        channel.receivers.remove(index);
        maybe_finish_port_channel(*channel_id);
        return;
    }
}

void ExtensionHost::disconnect_port(WebContentClient& client, u64 context_id, u64 local_port_id)
{
    if (client.is_private() == IsPrivate::Yes)
        return;
    disconnect_port_endpoint_internal(client, local_port_id, context_id, PortDisconnectReason::Explicit);
}

void ExtensionHost::disconnect_context_ports(WebContentClient& client, u64 context_id)
{
    if (client.is_private() == IsPrivate::Yes)
        return;
    Vector<u64> port_ids;
    for (auto const& [_, channel] : m_port_channels) {
        if (!channel.opener_disconnected && channel.opener.client_key == &client && channel.opener.context_id == context_id)
            port_ids.append(channel.opener.local_port_id);
        for (auto const& receiver : channel.receivers) {
            if (receiver.client_key == &client && receiver.context_id == context_id)
                port_ids.append(receiver.local_port_id);
        }
    }
    for (auto local_port_id : port_ids)
        disconnect_port_endpoint_internal(client, local_port_id, context_id, PortDisconnectReason::Passive);
}

void ExtensionHost::on_client_disconnected(WebContentClient& client)
{
    Vector<u64> port_ids;
    Vector<u64> pending_channel_ids;
    for (auto const& [channel_id, channel] : m_port_channels) {
        if (!channel.opener_disconnected && channel.opener.client_key == &client)
            port_ids.append(channel.opener.local_port_id);
        for (auto const& receiver : channel.receivers) {
            if (receiver.client_key == &client)
                port_ids.append(receiver.local_port_id);
        }
        if (channel.pending_open_replies.contains(&client))
            pending_channel_ids.append(channel_id);
    }
    for (auto local_port_id : port_ids)
        disconnect_port_endpoint_internal(client, local_port_id, {}, PortDisconnectReason::Passive);
    for (auto channel_id : pending_channel_ids) {
        if (auto it = m_port_channels.find(channel_id); it != m_port_channels.end()) {
            it->value.pending_open_replies.remove(&client);
            it->value.authorized_receivers.remove_all_matching([&](AuthorizedPortReceiver const& entry) {
                return entry.client == &client;
            });
            maybe_finish_port_channel(channel_id);
        }
    }
}

}
