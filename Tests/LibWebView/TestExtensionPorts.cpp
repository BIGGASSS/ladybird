/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/TCPServer.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <signal.h>

namespace {

static void append_u16_le(ByteBuffer& buffer, u16 value)
{
    u8 bytes[2] = { static_cast<u8>(value & 0xff), static_cast<u8>((value >> 8) & 0xff) };
    buffer.append(bytes, sizeof(bytes));
}

static void append_u32_le(ByteBuffer& buffer, u32 value)
{
    u8 bytes[4] = {
        static_cast<u8>(value & 0xff),
        static_cast<u8>((value >> 8) & 0xff),
        static_cast<u8>((value >> 16) & 0xff),
        static_cast<u8>((value >> 24) & 0xff),
    };
    buffer.append(bytes, sizeof(bytes));
}

static ByteBuffer stored_zip(ReadonlySpan<Array<StringView, 2>> entries)
{
    ByteBuffer archive;
    ByteBuffer central_directory;
    for (auto const& entry : entries) {
        auto name = entry[0];
        auto content = entry[1];
        auto local_offset = static_cast<u32>(archive.size());

        archive.append("PK\x03\x04"sv.bytes());
        append_u16_le(archive, 20);
        append_u16_le(archive, 0);
        append_u16_le(archive, 0);
        append_u16_le(archive, 0);
        append_u16_le(archive, 0);
        append_u32_le(archive, 0);
        append_u32_le(archive, static_cast<u32>(content.length()));
        append_u32_le(archive, static_cast<u32>(content.length()));
        append_u16_le(archive, static_cast<u16>(name.length()));
        append_u16_le(archive, 0);
        archive.append(name.bytes());
        archive.append(content.bytes());

        central_directory.append("PK\x01\x02"sv.bytes());
        append_u16_le(central_directory, 20);
        append_u16_le(central_directory, 20);
        append_u16_le(central_directory, 0);
        append_u16_le(central_directory, 0);
        append_u16_le(central_directory, 0);
        append_u16_le(central_directory, 0);
        append_u32_le(central_directory, 0);
        append_u32_le(central_directory, static_cast<u32>(content.length()));
        append_u32_le(central_directory, static_cast<u32>(content.length()));
        append_u16_le(central_directory, static_cast<u16>(name.length()));
        append_u16_le(central_directory, 0);
        append_u16_le(central_directory, 0);
        append_u16_le(central_directory, 0);
        append_u16_le(central_directory, 0);
        append_u32_le(central_directory, 0);
        append_u32_le(central_directory, local_offset);
        central_directory.append(name.bytes());
    }
    auto cd_offset = static_cast<u32>(archive.size());
    archive.append(central_directory.bytes());
    archive.append("PK\x05\x06"sv.bytes());
    append_u16_le(archive, 0);
    append_u16_le(archive, 0);
    append_u16_le(archive, static_cast<u16>(entries.size()));
    append_u16_le(archive, static_cast<u16>(entries.size()));
    append_u32_le(archive, static_cast<u32>(central_directory.size()));
    append_u32_le(archive, cd_offset);
    append_u16_le(archive, 0);
    return archive;
}

class PortTestServer {
public:
    PortTestServer()
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));
            m_connections.append(make<Connection>(move(socket)));
            auto& connection = *m_connections.last();
            connection.socket->on_ready_to_read = [this, &connection] { read_from(connection); };
        };
    }

    u16 port() const { return *m_server->local_port(); }

    URL::URL url(StringView path) const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}{}", port(), path)).release_value();
    }

    URL::URL localhost_url(StringView path) const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://localhost:{}{}", port(), path)).release_value();
    }

    bool has_event(StringView type) const
    {
        return m_events_by_type.contains(type);
    }

    JsonObject const& event(StringView type) const
    {
        return m_events_by_type.get(type).value();
    }

    void clear_event(StringView type)
    {
        m_events_by_type.remove(type);
    }

    void send_bg_command(ByteString command)
    {
        if (m_waiting_bg_cmd_connection) {
            auto* connection = exchange(m_waiting_bg_cmd_connection, nullptr);
            write_response(*connection, "text/plain"sv, command);
            return;
        }
        m_queued_bg_commands.append(move(command));
    }

    Function<void(StringView query, Function<void()> release)> on_race_hook;

private:
    struct Connection {
        AK_ALLOC_WITH_KMALLOC;

        NonnullOwnPtr<Core::TCPSocket> socket;
        ByteBuffer data {};
    };

    void write_response(Connection& connection, StringView content_type, StringView body)
    {
        auto response = ByteString::formatted(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: {}\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Length: {}\r\n"
            "Connection: close\r\n"
            "\r\n{}",
            content_type, body.length(), body);
        MUST(connection.socket->write_until_depleted(response.bytes()));
        connection.socket->on_ready_to_read = nullptr;
        connection.data.clear();
    }

    void read_from(Connection& connection)
    {
        auto buffer = MUST(ByteBuffer::create_uninitialized(16 * KiB));
        auto received = connection.socket->read_some(buffer);
        if (received.is_error() || received.value().is_empty()) {
            connection.socket->on_ready_to_read = nullptr;
            return;
        }
        connection.data.append(received.value());

        auto data = StringView { connection.data.bytes() };
        if (!data.contains("\r\n\r\n"sv))
            return;

        auto first_line = data.split_view("\r\n"sv)[0];
        auto parts = first_line.split_view(' ');
        if (parts.size() < 2)
            return;
        auto target = parts[1];

        if (target == "/bg-cmd"sv) {
            connection.socket->on_ready_to_read = nullptr;
            connection.data.clear();
            if (!m_queued_bg_commands.is_empty()) {
                auto command = m_queued_bg_commands.take_first();
                write_response(connection, "text/plain"sv, command);
            } else {
                m_waiting_bg_cmd_connection = &connection;
            }
            return;
        }

        if (target.starts_with("/race-hook"sv)) {
            connection.socket->on_ready_to_read = nullptr;
            connection.data.clear();
            auto query = target.substring_view("/race-hook"sv.length());
            if (on_race_hook) {
                on_race_hook(query, [this, &connection] {
                    write_response(connection, "text/plain"sv, "ok"sv);
                });
            } else {
                write_response(connection, "text/plain"sv, "ok"sv);
            }
            return;
        }

        StringView content_type = "text/html"sv;
        ByteString body;
        if (target.starts_with("/event?data="sv)) {
            auto encoded = target.substring_view("/event?data="sv.length());
            auto decoded = URL::percent_decode(encoded);
            if (auto parsed = JsonValue::from_string(decoded); !parsed.is_error() && parsed.value().is_object()) {
                auto obj = parsed.release_value().as_object();
                if (auto type = obj.get_string("type"sv); type.has_value())
                    m_events_by_type.set(*type, move(obj));
            }
            content_type = "text/plain"sv;
            body = "ok";
        } else if (target == "/multiframe"sv) {
            body = "<!doctype html><html><head><title>Multiframe</title></head><body>"
                   "<iframe id='fa' src='/frame-a'></iframe>"
                   "<iframe id='fb' src='/frame-b'></iframe>"
                   "</body></html>";
        } else if (target == "/frame-a"sv) {
            body = "<!doctype html><html><body>Frame A</body></html>";
        } else if (target == "/frame-b"sv) {
            body = "<!doctype html><html><body>Frame B</body></html>";
        } else if (target == "/crash-target"sv) {
            body = "<!doctype html><html><body>Crash Target</body></html>";
        } else if (target == "/bf-page1"sv) {
            body = "<!doctype html><html><body>BF Page 1</body></html>";
        } else if (target == "/bf-page2"sv) {
            body = "<!doctype html><html><body>BF Page 2</body></html>";
        } else if (target == "/dynamic-port"sv) {
            body = "<!doctype html><html><body>Dynamic Port</body></html>";
        } else if (target.starts_with("/race-host"sv)) {
            body = "<!doctype html><html><body><script>"
                   "window.addEventListener('pagehide', () => {"
                   "  const xhr = new XMLHttpRequest();"
                   "  xhr.open('GET', '/race-hook' + location.search, false);"
                   "  xhr.send();"
                   "});"
                   "fetch('/event?data=' + encodeURIComponent(JSON.stringify({ type: 'race-host-ready' + location.search })));"
                   "</script></body></html>";
        } else if (target == "/race-reg"sv) {
            body = ByteString::formatted(
                "<!doctype html><html><body>Race Reg<script>"
                "window.addEventListener('load', () => {{"
                "  setTimeout(() => {{"
                "    fetch('http://127.0.0.1:{}/event?data=' + encodeURIComponent(JSON.stringify({{ type: 'race-reg-dom', dynInjected: document.documentElement.dataset.dynInjected || null }})));"
                "  }}, 250);"
                "}});"
                "</script></body></html>",
                port());
        } else if (target == "/race-unreg"sv) {
            body = "<!doctype html><html><body>Race Unreg</body></html>";
        } else {
            body = "<!doctype html><html><body>Not Found</body></html>";
        }

        write_response(connection, content_type, body);
    }

    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Connection>> m_connections;
    HashMap<String, JsonObject> m_events_by_type;
    Connection* m_waiting_bg_cmd_connection { nullptr };
    Vector<ByteString> m_queued_bg_commands;
};

class ExtensionTestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(ExtensionTestApplication)

public:
    explicit ExtensionTestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

private:
    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::No;
    }
};

}

TEST_CASE(extension_message_ports)
{
    Test::HeadlessTestEnvironment environment { "TestExtensionPorts"sv, 60 };
    PortTestServer server;

    auto app = TRY_OR_FAIL(environment.create_application<ExtensionTestApplication>());

    auto port_str = ByteString::number(server.port());
    auto bg_js = ByteString::formatted(R"JS(
const BASE = "http://127.0.0.1:{}";
function report(payload) {{
    fetch(BASE + "/event?data=" + encodeURIComponent(JSON.stringify(payload))).catch(() => {{}});
}}

const registeredFrames = {{}};
let multiTabId = null;

function maybeRunMultiframeTests() {{
    if (!registeredFrames.top || !registeredFrames.a || !registeredFrames.b) return;
    if (maybeRunMultiframeTests.ran) return;
    maybeRunMultiframeTests.ran = true;

    const topInfo = registeredFrames.top;
    const aInfo = registeredFrames.a;
    const bInfo = registeredFrames.b;
    report({{
        type: "frame-ids",
        topFrameId: topInfo.frameId,
        aFrameId: aInfo.frameId,
        bFrameId: bInfo.frameId,
        distinctDocs: new Set([topInfo.documentId, aInfo.documentId, bInfo.documentId]).size,
    }});

    // 1. Fan-out across all 3 frames
    const fanoutPort = chrome.tabs.connect(multiTabId, {{ name: "fanout" }});
    const fanoutReplies = [];
    fanoutPort.onMessage.addListener((msg) => {{
        fanoutReplies.push(msg.from);
        if (fanoutReplies.length === 3) {{
            fanoutReplies.sort();
            report({{ type: "fanout-done", replies: fanoutReplies }});
            fanoutPort.disconnect();

            // 2. Targeted by child frameId
            const frameAPort = chrome.tabs.connect(multiTabId, {{ name: "only-a", frameId: aInfo.frameId }});
            const frameAReplies = [];
            frameAPort.onMessage.addListener((m) => {{
                frameAReplies.push(m.from);
                setTimeout(() => {{
                    report({{ type: "frame-a-target", replies: frameAReplies }});
                    frameAPort.disconnect();

                    // 3. Targeted by documentId
                    const docBPort = chrome.tabs.connect(multiTabId, {{ name: "only-b-doc", documentId: bInfo.documentId }});
                    const docBReplies = [];
                    docBPort.onMessage.addListener((m2) => {{
                        docBReplies.push(m2.from);
                        setTimeout(() => {{
                            report({{ type: "doc-b-target", replies: docBReplies }});
                            docBPort.disconnect();

                            // 4. Unknown documentId & frameId fail closed
                            const badDoc = chrome.tabs.connect(multiTabId, {{ name: "bad-doc", documentId: "no-such-doc" }});
                            badDoc.onDisconnect.addListener(() => {{
                                report({{ type: "bad-doc-disconnect", error: chrome.runtime.lastError ? chrome.runtime.lastError.message : null }});
                            }});
                            const badFrame = chrome.tabs.connect(multiTabId, {{ name: "bad-frame", frameId: 999999 }});
                            badFrame.onDisconnect.addListener(() => {{
                                report({{ type: "bad-frame-disconnect", error: chrome.runtime.lastError ? chrome.runtime.lastError.message : null }});
                            }});

                            // 5. Passive frame teardown preserves channel while other receivers remain
                            const passivePort = chrome.tabs.connect(multiTabId, {{ name: "passive-test" }});
                            let passiveDisconnected = false;
                            const passiveReplies = [];
                            passivePort.onDisconnect.addListener(() => {{
                                passiveDisconnected = true;
                            }});
                            passivePort.onMessage.addListener((pm) => {{
                                if (pm.phase === "removed-b") {{
                                    passivePort.postMessage({{ phase: "ping-after-remove" }});
                                }} else if (pm.phase === "pong-after-remove") {{
                                    passiveReplies.push(pm.from);
                                    if (passiveReplies.length === 2) {{
                                        passiveReplies.sort();
                                        report({{ type: "passive-preserved", disconnected: passiveDisconnected, replies: passiveReplies }});
                                        passivePort.disconnect();

                                        // 6. Explicit receiver disconnect closes the entire channel for opener AND sibling receiver
                                        const explicitPort = chrome.tabs.connect(multiTabId, {{ name: "explicit-test" }});
                                        explicitPort.onDisconnect.addListener(() => {{
                                            report({{ type: "explicit-opener-disconnected", error: chrome.runtime.lastError ? chrome.runtime.lastError.message : null }});
                                        }});
                                        explicitPort.postMessage({{ triggerExplicitDisconnect: true }});
                                    }}
                                }}
                            }});
                            passivePort.postMessage({{ phase: "remove-b" }});
                        }}, 50);
                    }});
                    docBPort.postMessage({{ hello: "b" }});
                }}, 50);
            }});
            frameAPort.postMessage({{ hello: "a" }});
        }}
    }});
    fanoutPort.postMessage({{ hello: "all" }});
}}

chrome.runtime.onConnect.addListener((port) => {{
    if (port.name === "early") {{
        const seqs = [];
        report({{
            type: "early-sender",
            url: port.sender && port.sender.url,
            origin: port.sender && port.sender.origin,
            frameId: port.sender && port.sender.frameId,
            hasDocId: Boolean(port.sender && typeof port.sender.documentId === "string" && port.sender.documentId.length > 0),
        }});
        port.onMessage.addListener((msg) => {{
            seqs.push(msg.seq);
            port.postMessage({{ reply: msg.seq * 10 }});
            if (seqs.length === 2) {{
                report({{ type: "early-received", seqs }});
                port.disconnect();
            }}
        }});
    }} else if (port.name === "register-frame") {{
        port.onMessage.addListener((msg) => {{
            multiTabId = port.sender.tab.id;
            registeredFrames[msg.role] = {{
                frameId: port.sender.frameId,
                documentId: port.sender.documentId,
            }};
            port.disconnect();
            maybeRunMultiframeTests();
        }});
    }} else if (port.name === "crash-port") {{
        port.onDisconnect.addListener(() => {{
            report({{ type: "crash-disconnected", error: chrome.runtime.lastError ? chrome.runtime.lastError.message : null }});
        }});
        port.onMessage.addListener((msg) => {{
            if (msg.alive) report({{ type: "crash-port-ready" }});
        }});
    }} else if (port.name === "bf-port-1") {{
        port.onDisconnect.addListener(() => {{
            report({{ type: "bf-port-1-disconnected", error: chrome.runtime.lastError ? chrome.runtime.lastError.message : null }});
        }});
        port.postMessage({{ ack: 1 }});
    }} else if (port.name === "bf-port-2") {{
        port.onMessage.addListener((msg) => {{
            if (msg.restored) {{
                port.postMessage({{ ok: true }});
                report({{ type: "bf-port-2-alive" }});
            }}
        }});
    }} else if (port.name === "after-unreg-cs") {{
        port.onMessage.addListener((msg) => {{
            report({{ type: "dyn-cs-connect-after-unreg", msg }});
            port.disconnect();
        }});
    }} else if (port.name === "race-unreg-cs") {{
        port.onMessage.addListener((msg) => {{
            report({{ type: "race-unreg-cs-ok", msg }});
            port.disconnect();
        }});
    }}
}});

let handledDynTab = false;
let handledRaceRegTab = false;
let handledRaceUnregTab = false;
chrome.tabs.onUpdated.addListener(async (tabId, changeInfo, tab) => {{
    if (changeInfo.status !== "complete" || !tab.url) return;
    if (!handledDynTab && tab.url.endsWith("/dynamic-port")) {{
        handledDynTab = true;
        // Unregister BEFORE any port has ever been opened on /dynamic-port.
        await chrome.scripting.unregisterContentScripts({{ ids: ["dyn-port"] }});
        const backPort = chrome.tabs.connect(tabId, {{ name: "after-unreg-tab" }});
        backPort.onMessage.addListener((reply) => {{
            report({{ type: "dyn-tab-connect-after-unreg", reply }});
            backPort.disconnect();
        }});
        backPort.postMessage({{ ping: "after-unreg" }});
    }} else if (!handledRaceRegTab && tab.url.endsWith("/race-reg")) {{
        handledRaceRegTab = true;
        const p = chrome.tabs.connect(tabId, {{ name: "race-reg-tab" }});
        p.onMessage.addListener((reply) => {{
            report({{ type: "race-reg-unexpected-reply", reply }});
        }});
        p.onDisconnect.addListener(() => {{
            report({{ type: "race-reg-disconnected", error: chrome.runtime.lastError ? chrome.runtime.lastError.message : null }});
        }});
        p.postMessage({{ ping: "should-not-inject" }});
    }} else if (!handledRaceUnregTab && tab.url.endsWith("/race-unreg")) {{
        handledRaceUnregTab = true;
        const p = chrome.tabs.connect(tabId, {{ name: "race-unreg-tab" }});
        p.onMessage.addListener((reply) => {{
            report({{ type: "race-unreg-tab-ok", reply }});
            p.disconnect();
        }});
        p.postMessage({{ ping: "race-unreg-ping" }});
    }}
}});

async function pollCommands() {{
    while (true) {{
        let cmd = "";
        try {{
            cmd = await (await fetch(BASE + "/bg-cmd")).text();
        }} catch (e) {{
            return;
        }}
        if (cmd === "register-race-reg") {{
            await chrome.scripting.registerContentScripts([{{
                id: "race-reg-script",
                matches: ["http://localhost/race-reg"],
                js: ["dyn.js"],
                runAt: "document_end",
            }}]);
            report({{ type: "race-reg-registered" }});
        }} else if (cmd === "prepare-race-unreg") {{
            await chrome.scripting.registerContentScripts([{{
                id: "race-unreg-script",
                matches: ["http://localhost/race-unreg"],
                js: ["dyn.js"],
                runAt: "document_end",
            }}]);
            report({{ type: "race-unreg-prepared" }});
        }} else if (cmd === "unregister-race-unreg") {{
            await chrome.scripting.unregisterContentScripts({{ ids: ["race-unreg-script"] }});
            report({{ type: "race-unreg-unregistered" }});
        }} else if (cmd === "stop") {{
            return;
        }}
    }}
}}

chrome.scripting.registerContentScripts([{{
    id: "dyn-port",
    matches: ["http://127.0.0.1/dynamic-port"],
    js: ["dyn.js"],
    runAt: "document_end",
}}]).then(() => {{
    pollCommands();
    report({{ type: "bg-ready" }});
}});
)JS",
        port_str);

    auto cs_js = ByteString::formatted(R"JS(
const BASE = "http://127.0.0.1:{}";
function report(payload) {{
    fetch(BASE + "/event?data=" + encodeURIComponent(JSON.stringify(payload))).catch(() => {{}});
}}

const path = location.pathname;
if (path === "/multiframe" || path === "/frame-a" || path === "/frame-b") {{
    const role = path === "/multiframe" ? "top" : (path === "/frame-a" ? "a" : "b");

    chrome.runtime.onConnect.addListener((port) => {{
        if (port.name === "fanout" || port.name === "only-a" || port.name === "only-b-doc") {{
            port.onMessage.addListener(() => {{
                port.postMessage({{ from: role }});
            }});
        }} else if (port.name === "passive-test") {{
            port.onMessage.addListener((msg) => {{
                if (msg.phase === "remove-b" && role === "top") {{
                    const fb = document.getElementById("fb");
                    if (fb) fb.remove();
                    setTimeout(() => port.postMessage({{ phase: "removed-b" }}), 50);
                }} else if (msg.phase === "ping-after-remove") {{
                    port.postMessage({{ phase: "pong-after-remove", from: role }});
                }}
            }});
        }} else if (port.name === "explicit-test") {{
            let siblingOnDisconnectFired = false;
            if (role === "top") {{
                port.onDisconnect.addListener(() => {{
                    siblingOnDisconnectFired = true;
                }});
            }}
            port.onMessage.addListener((msg) => {{
                if (msg.triggerExplicitDisconnect) {{
                    if (role === "a") {{
                        port.disconnect();
                    }} else if (role === "top") {{
                        setTimeout(() => {{
                            let postAfterSiblingDisconnectThrew = false;
                            try {{ port.postMessage({{ stillOpen: true }}); }} catch (e) {{ postAfterSiblingDisconnectThrew = true; }}
                            report({{
                                type: "explicit-sibling-checked",
                                siblingOnDisconnectFired,
                                postAfterSiblingDisconnectThrew,
                            }});
                        }}, 100);
                    }}
                }}
            }});
        }}
    }});

    if (role === "top") {{
        // Early-send + serialization error checks
        let fnThrew = false;
        const early = chrome.runtime.connect({{ name: "early" }});
        try {{
            early.postMessage(() => {{}});
        }} catch (e) {{
            fnThrew = e instanceof TypeError;
        }}
        const replies = [];
        early.onMessage.addListener((m) => replies.push(m.reply));
        early.onDisconnect.addListener(() => {{
            let afterDisconnectThrew = false;
            try {{ early.postMessage({{ x: 1 }}); }} catch (e) {{ afterDisconnectThrew = true; }}
            report({{ type: "early-cs-done", replies, fnThrew, afterDisconnectThrew }});
        }});
        early.postMessage({{ seq: 1 }});
        early.postMessage({{ seq: 2 }});
    }}

    const reg = chrome.runtime.connect({{ name: "register-frame" }});
    reg.postMessage({{ role }});
}} else if (path === "/crash-target") {{
    const p = chrome.runtime.connect({{ name: "crash-port" }});
    p.postMessage({{ alive: true }});
}} else if (path === "/bf-page1") {{
    let p1Disconnected = false;
    const p1 = chrome.runtime.connect({{ name: "bf-port-1" }});
    p1.onDisconnect.addListener(() => {{ p1Disconnected = true; }});
    p1.onMessage.addListener((m) => {{
        if (m.ack === 1) report({{ type: "bf-page1-ready" }});
    }});
    const openReplacement = (persisted) => {{
        const p2 = chrome.runtime.connect({{ name: "bf-port-2" }});
        let p2Disconnected = false;
        p2.onDisconnect.addListener(() => {{ p2Disconnected = true; }});
        p2.onMessage.addListener((m) => {{
            if (m.ok) {{
                setTimeout(() => {{
                    report({{ type: "bf-restored-ok", persisted, p1Disconnected, p2Disconnected }});
                }}, 50);
            }}
        }});
        p2.postMessage({{ restored: true }});
    }};
    window.addEventListener("pageshow", (event) => {{
        if (event.persisted) openReplacement(true);
    }});
    if (sessionStorage.getItem("visited-bf-page1") === "1") {{
        openReplacement(false);
    }} else {{
        sessionStorage.setItem("visited-bf-page1", "1");
    }}
}}
)JS",
        port_str);

    auto dyn_js = R"JS(
// Leave a DOM marker so tests can directly observe whether WebContent injected this script.
document.documentElement.dataset.dynInjected = "1";
// Do NOT open any port on injection; wait until background connects first.
chrome.runtime.onConnect.addListener((port) => {
    if (port.name === "after-unreg-tab") {
        port.onMessage.addListener((msg) => {
            port.postMessage({ echoed: msg.ping });
            const afterPort = chrome.runtime.connect({ name: "after-unreg-cs" });
            afterPort.postMessage({ stillAuthorized: true });
        });
    } else if (port.name === "race-reg-tab") {
        port.onMessage.addListener((msg) => {
            port.postMessage({ echoed: msg.ping });
        });
    } else if (port.name === "race-unreg-tab") {
        port.onMessage.addListener((msg) => {
            port.postMessage({ echoed: msg.ping });
            const csPort = chrome.runtime.connect({ name: "race-unreg-cs" });
            csPort.postMessage({ stillAuthorized: true });
        });
    }
});
)JS"sv;

    auto manifest_json = R"({
        "manifest_version": 3,
        "name": "Port Integration Test",
        "version": "1.0",
        "permissions": ["tabs", "scripting"],
        "host_permissions": ["http://127.0.0.1/*", "http://localhost/*"],
        "background": { "service_worker": "bg.js" },
        "content_scripts": [{
            "matches": ["http://127.0.0.1/*"],
            "exclude_matches": ["http://127.0.0.1/dynamic-port", "http://127.0.0.1/race-host*"],
            "js": ["cs.js"],
            "all_frames": true,
            "run_at": "document_idle"
        }]
    })"sv;

    Array zip_entries {
        Array<StringView, 2> { "manifest.json"sv, manifest_json },
        Array<StringView, 2> { "bg.js"sv, bg_js },
        Array<StringView, 2> { "cs.js"sv, cs_js },
        Array<StringView, 2> { "dyn.js"sv, dyn_js },
    };
    auto zip_bytes = stored_zip(zip_entries);
    auto zip_path = LexicalPath::join(environment.directory(), "ports-test.zip"sv).string();
    {
        auto file = TRY_OR_FAIL(Core::File::open(zip_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        TRY_OR_FAIL(file->write_until_depleted(zip_bytes));
    }

    TRY_OR_FAIL(app->install_extension(zip_path));
    Core::EventLoop::current().spin_until([&] { return server.has_event("bg-ready"sv); });

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    // 1–3: Early sends, multi-frame fan-out, frameId/documentId targeting, passive teardown vs explicit disconnect.
    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        view->load_from_browser_ui(server.url("/multiframe"sv));

        Core::EventLoop::current().spin_until([&] {
            return server.has_event("early-sender"sv)
                && server.has_event("early-received"sv)
                && server.has_event("early-cs-done"sv)
                && server.has_event("frame-ids"sv)
                && server.has_event("fanout-done"sv)
                && server.has_event("frame-a-target"sv)
                && server.has_event("doc-b-target"sv)
                && server.has_event("bad-doc-disconnect"sv)
                && server.has_event("bad-frame-disconnect"sv)
                && server.has_event("passive-preserved"sv)
                && server.has_event("explicit-opener-disconnected"sv)
                && server.has_event("explicit-sibling-checked"sv);
        });

        auto const& early_sender = server.event("early-sender"sv);
        EXPECT_EQ(early_sender.get_string("url"sv), MUST(String::formatted("http://127.0.0.1:{}/multiframe", server.port())));
        EXPECT_EQ(early_sender.get_string("origin"sv), MUST(String::formatted("http://127.0.0.1:{}", server.port())));
        EXPECT_EQ(early_sender.get_u64("frameId"sv), 0u);
        EXPECT_EQ(early_sender.get_bool("hasDocId"sv), true);

        auto const& early_received = server.event("early-received"sv).get_array("seqs"sv).value();
        EXPECT_EQ(early_received.size(), 2u);
        EXPECT_EQ(early_received.at(0).get_i64(), 1);
        EXPECT_EQ(early_received.at(1).get_i64(), 2);

        auto const& early_cs = server.event("early-cs-done"sv);
        EXPECT_EQ(early_cs.get_bool("fnThrew"sv), true);
        EXPECT_EQ(early_cs.get_bool("afterDisconnectThrew"sv), true);
        auto const& cs_replies = early_cs.get_array("replies"sv).value();
        EXPECT_EQ(cs_replies.size(), 2u);
        EXPECT_EQ(cs_replies.at(0).get_i64(), 10);
        EXPECT_EQ(cs_replies.at(1).get_i64(), 20);

        auto const& frame_ids = server.event("frame-ids"sv);
        EXPECT_EQ(frame_ids.get_u64("topFrameId"sv), 0u);
        EXPECT(frame_ids.get_u64("aFrameId"sv).value_or(0) > 0u);
        EXPECT(frame_ids.get_u64("bFrameId"sv).value_or(0) > 0u);
        EXPECT_NE(frame_ids.get_u64("aFrameId"sv), frame_ids.get_u64("bFrameId"sv));
        EXPECT_EQ(frame_ids.get_u64("distinctDocs"sv), 3u);

        auto const& fanout = server.event("fanout-done"sv).get_array("replies"sv).value();
        EXPECT_EQ(fanout.size(), 3u);
        EXPECT_EQ(fanout.at(0).as_string(), "a"sv);
        EXPECT_EQ(fanout.at(1).as_string(), "b"sv);
        EXPECT_EQ(fanout.at(2).as_string(), "top"sv);

        auto const& frame_a = server.event("frame-a-target"sv).get_array("replies"sv).value();
        EXPECT_EQ(frame_a.size(), 1u);
        EXPECT_EQ(frame_a.at(0).as_string(), "a"sv);

        auto const& doc_b = server.event("doc-b-target"sv).get_array("replies"sv).value();
        EXPECT_EQ(doc_b.size(), 1u);
        EXPECT_EQ(doc_b.at(0).as_string(), "b"sv);

        EXPECT_EQ(server.event("bad-doc-disconnect"sv).get_string("error"sv), "Could not establish connection. Receiving end does not exist."sv);
        EXPECT_EQ(server.event("bad-frame-disconnect"sv).get_string("error"sv), "Could not establish connection. Receiving end does not exist."sv);

        auto const& passive = server.event("passive-preserved"sv);
        EXPECT_EQ(passive.get_bool("disconnected"sv), false);
        auto const& passive_replies = passive.get_array("replies"sv).value();
        EXPECT_EQ(passive_replies.size(), 2u);
        EXPECT_EQ(passive_replies.at(0).as_string(), "a"sv);
        EXPECT_EQ(passive_replies.at(1).as_string(), "top"sv);

        EXPECT(!server.event("explicit-opener-disconnected"sv).get_string("error"sv).has_value());
        auto const& explicit_sibling = server.event("explicit-sibling-checked"sv);
        EXPECT_EQ(explicit_sibling.get_bool("siblingOnDisconnectFired"sv), false);
        EXPECT_EQ(explicit_sibling.get_bool("postAfterSiblingDisconnectThrew"sv), true);
    }

    // 4. Renderer process exit disconnects port on the peer side.
    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        view->load_from_browser_ui(server.url("/crash-target"sv));
        Core::EventLoop::current().spin_until([&] { return server.has_event("crash-port-ready"sv); });
        auto pid = view->web_content_pid();
        VERIFY(pid > 0);
        kill(pid, SIGKILL);
        Core::EventLoop::current().spin_until([&] { return server.has_event("crash-disconnected"sv); });
        EXPECT(!server.event("crash-disconnected"sv).get_string("error"sv).has_value());
    }

    // 5. Navigation / BFCache deactivation disconnects old port and allows replacement port after history back.
    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        size_t loads_finished = 0;
        view->on_load_finish = [&](auto const&) { ++loads_finished; };
        view->load_from_browser_ui(server.url("/bf-page1"sv));
        Core::EventLoop::current().spin_until([&] { return server.has_event("bf-page1-ready"sv); });

        auto target_loads = loads_finished + 1;
        view->load_from_browser_ui(server.url("/bf-page2"sv));
        Core::EventLoop::current().spin_until([&] { return loads_finished >= target_loads && server.has_event("bf-port-1-disconnected"sv); });
        EXPECT(!server.event("bf-port-1-disconnected"sv).get_string("error"sv).has_value());

        // Traverse back and verify replacement port works cleanly after restoration/reload without being closed by stale teardown.
        bool traversal_done = false;
        view->on_browser_history_traversal_complete = [&] { traversal_done = true; };
        view->traverse_the_history_by_delta(-1);
        Core::EventLoop::current().spin_until([&] {
            return traversal_done && server.has_event("bf-restored-ok"sv) && server.has_event("bf-port-2-alive"sv);
        });
        auto const& restored = server.event("bf-restored-ok"sv);
        EXPECT_EQ(restored.get_bool("p2Disconnected"sv), false);
        if (restored.get_bool("persisted"sv).value_or(false))
            EXPECT_EQ(restored.get_bool("p1Disconnected"sv), true);
    }

    // 6. Unregistering a dynamic content script preserves port authorization for already-injected live documents.
    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        view->load_from_browser_ui(server.url("/dynamic-port"sv));
        Core::EventLoop::current().spin_until([&] {
            return server.has_event("dyn-tab-connect-after-unreg"sv) && server.has_event("dyn-cs-connect-after-unreg"sv);
        });
        EXPECT_EQ(server.event("dyn-tab-connect-after-unreg"sv).get_object("reply"sv)->get_string("echoed"sv), "after-unreg"sv);
        EXPECT_EQ(server.event("dyn-cs-connect-after-unreg"sv).get_object("msg"sv)->get_bool("stillAuthorized"sv), true);
    }

    // 7. Registering or unregistering between document population and activation stays consistent via the selected generation.
    {
        RefPtr<Core::Timer> release_timer;
        Function<void()> pending_release;

        // 7a. Registration between population and activation must not inject into the already-populated document.
        server.on_race_hook = [&](StringView query, Function<void()> release) {
            EXPECT_EQ(query, "?action=register"sv);
            pending_release = move(release);
            server.send_bg_command("register-race-reg");
        };
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        view->load_from_browser_ui(server.url("/race-host?action=register"sv));
        Core::EventLoop::current().spin_until([&] { return server.has_event("race-host-ready?action=register"sv); });

        view->load_from_browser_ui(server.localhost_url("/race-reg"sv));
        Core::EventLoop::current().spin_until([&] { return server.has_event("race-reg-registered"sv); });
        // Give the destination process a beat to consume the broadcast set_extensions(G+1) before activation.
        bool waited = false;
        release_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(50), [&] {
            waited = true;
            if (pending_release) {
                auto release = exchange(pending_release, nullptr);
                release();
            }
        });
        release_timer->start();
        Core::EventLoop::current().spin_until([&] { return waited && server.has_event("race-reg-disconnected"sv) && server.has_event("race-reg-dom"sv); });
        EXPECT_EQ(server.event("race-reg-disconnected"sv).get_string("error"sv), "Could not establish connection. Receiving end does not exist."sv);
        EXPECT(!server.has_event("race-reg-unexpected-reply"sv));
        EXPECT(!server.event("race-reg-dom"sv).get_string("dynInjected"sv).has_value());

        // 7b. Unregistration between population and activation must still inject and authorize the already-populated document.
        server.send_bg_command("prepare-race-unreg");
        Core::EventLoop::current().spin_until([&] { return server.has_event("race-unreg-prepared"sv); });

        server.on_race_hook = [&](StringView query, Function<void()> release) {
            EXPECT_EQ(query, "?action=unregister"sv);
            pending_release = move(release);
            server.send_bg_command("unregister-race-unreg");
        };
        view->load_from_browser_ui(server.url("/race-host?action=unregister"sv));
        Core::EventLoop::current().spin_until([&] { return server.has_event("race-host-ready?action=unregister"sv); });

        view->load_from_browser_ui(server.localhost_url("/race-unreg"sv));
        Core::EventLoop::current().spin_until([&] { return server.has_event("race-unreg-unregistered"sv); });
        waited = false;
        release_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(50), [&] {
            waited = true;
            if (pending_release) {
                auto release = exchange(pending_release, nullptr);
                release();
            }
        });
        release_timer->start();
        Core::EventLoop::current().spin_until([&] {
            return waited && server.has_event("race-unreg-tab-ok"sv) && server.has_event("race-unreg-cs-ok"sv);
        });
        EXPECT_EQ(server.event("race-unreg-tab-ok"sv).get_object("reply"sv)->get_string("echoed"sv), "race-unreg-ping"sv);
        EXPECT_EQ(server.event("race-unreg-cs-ok"sv).get_object("msg"sv)->get_bool("stillAuthorized"sv), true);

        server.on_race_hook = nullptr;
        server.send_bg_command("stop");
    }
}
