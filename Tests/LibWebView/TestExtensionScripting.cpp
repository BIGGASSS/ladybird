/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
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
#include <LibWebView/ExtensionHost.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

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

class ScriptingTestServer {
public:
    ScriptingTestServer()
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 0, 0, 0, 0 }, 0));
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

    bool has_event(StringView type) const { return m_events_by_type.contains(type); }
    JsonObject const& event(StringView type) const { return m_events_by_type.get(type).value(); }

private:
    struct Connection {
        AK_ALLOC_WITH_KMALLOC;
        NonnullOwnPtr<Core::TCPSocket> socket;
        ByteBuffer data {};
        RefPtr<Core::Timer> delay_timer {};
    };

    void send_response(Connection& connection, StringView content_type, StringView body, StringView extra_headers = ""sv)
    {
        auto response = ByteString::formatted(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: {}\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "{}"
            "Content-Length: {}\r\n"
            "Connection: close\r\n"
            "\r\n{}",
            content_type, extra_headers, body.length(), body);
        (void)connection.socket->write_until_depleted(response.bytes());
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

        StringView content_type = "text/html"sv;
        StringView extra_headers = ""sv;
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
        } else if (target == "/allowed"sv) {
            body = ByteString::formatted(
                "<!doctype html><html><head><title>Allowed Page</title>"
                "<style>#box {{ background-color: rgb(10, 20, 30); color: rgb(40, 50, 60); border-top: 2px solid rgb(11, 22, 33); }}</style>"
                "</head><body>"
                "<div id='box'>Box</div>"
                "<script>window.__pageSecret = 'top-secret';</script>"
                "<iframe id='fa' src='/frame-a'></iframe>"
                "<iframe id='remote' src='http://127.0.0.2:{}/remote-allowed'></iframe>"
                "<iframe id='data-allowed' src='data:text/html,<div id=dt>data-allowed</div>'></iframe>"
                "<iframe src='http://localhost:{}/denied-frame'></iframe>"
                "</body></html>",
                port(), port());
        } else if (target == "/frame-a"sv) {
            body = "<!doctype html><html><head><title>Frame A</title></head><body>"
                   "<div id='tag'>frame-a</div>"
                   "<script>window.__pageSecret = 'frame-a-secret';</script>"
                   "</body></html>";
        } else if (target == "/remote-allowed"sv) {
            body = "<!doctype html><html><head><title>Remote Allowed</title></head><body>"
                   "<div id='rtag'>remote-allowed</div>"
                   "</body></html>";
        } else if (target == "/late-import.css"sv) {
            connection.socket->on_ready_to_read = nullptr;
            connection.delay_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(80), [this, &connection] {
                send_response(connection, "text/css"sv, "#box { border-top-color: rgb(9, 8, 7) !important; }"sv);
            });
            connection.delay_timer->start();
            return;
        } else if (target == "/slow"sv) {
            body = "<!doctype html><html><head><title>Slow Page</title></head><body>"
                   "<div id='slow-ready'>ready</div>"
                   "<script src='/slow-script.js'></script>"
                   "</body></html>";
        } else if (target == "/slow-script.js"sv) {
            connection.socket->on_ready_to_read = nullptr;
            connection.delay_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(120), [this, &connection] {
                send_response(connection, "text/javascript"sv, "window.__slowScriptRan = true;"sv);
            });
            connection.delay_timer->start();
            return;
        } else if (target == "/activetab"sv) {
            ++m_activetab_request_count;
            // Second request at the exact same URL serves a CSP sandbox (opaque origin) to test same-URL revocation.
            if (m_activetab_request_count == 2)
                extra_headers = "Content-Security-Policy: sandbox\r\n"sv;
            body = "<!doctype html><html><head><title>ActiveTab Page</title></head><body>ActiveTab</body></html>";
        } else if (target == "/denied-frame"sv) {
            body = "<!doctype html><html><head><title>Denied</title></head><body>"
                   "Denied<iframe src='data:text/html,<div id=dt>data-denied</div>'></iframe>"
                   "</body></html>";
        } else if (target == "/denied"sv) {
            body = "<!doctype html><html><head><title>Denied</title></head><body>Denied</body></html>";
        } else {
            body = "<!doctype html><html><body>Not Found</body></html>";
        }

        send_response(connection, content_type, body, extra_headers);
    }

    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Connection>> m_connections;
    HashMap<String, JsonObject> m_events_by_type;
    size_t m_activetab_request_count { 0 };
};

class ScriptingTestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(ScriptingTestApplication)

public:
    explicit ScriptingTestApplication(Optional<ByteString> ladybird_binary_path)
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

TEST_CASE(extension_scripting_on_demand)
{
    Test::HeadlessTestEnvironment environment { "TestExtensionScripting"sv, 60 };
    ScriptingTestServer server;

    auto app = TRY_OR_FAIL(environment.create_application<ScriptingTestApplication>());

    auto port_str = ByteString::number(server.port());
    auto bg_js = ByteString::formatted(R"JS(
const BASE = "http://127.0.0.1:{}";
function report(payload) {{
    fetch(BASE + "/event?data=" + encodeURIComponent(JSON.stringify(payload))).catch(() => {{}});
}}

chrome.runtime.onConnect.addListener((port) => {{
    if (port.name === "ondemand-port") {{
        port.onMessage.addListener((msg) => {{
            port.postMessage({{ pong: msg.ping }});
            report({{ type: "ondemand-port-ok", ping: msg.ping }});
            port.disconnect();
        }});
    }}
}});

async function expectReject(promise) {{
    try {{
        await promise;
        return null;
    }} catch (e) {{
        return e && e.message ? e.message : String(e);
    }}
}}

let ranAllowed = false;
let ranDenied = false;
let ranExtPage = false;
let ranSlow = false;

chrome.tabs.onUpdated.addListener(async (tabId, changeInfo, tab) => {{
    if (!ranSlow && changeInfo.url && changeInfo.url.endsWith("/slow")) {{
        ranSlow = true;
        const order = [];
        const pImmediate = chrome.scripting.executeScript({{
            target: {{ tabId }},
            world: "MAIN",
            injectImmediately: true,
            func: () => ({{ readyState: document.readyState, slowRan: !!window.__slowScriptRan }}),
        }}).then((res) => {{
            order.push("immediate");
            return res[0].result;
        }});
        const pDeferred = chrome.scripting.executeScript({{
            target: {{ tabId }},
            world: "MAIN",
            func: () => ({{ readyState: document.readyState, slowRan: !!window.__slowScriptRan }}),
        }}).then((res) => {{
            order.push("deferred");
            return res[0].result;
        }});
        const [immediateInfo, deferredInfo] = await Promise.all([pImmediate, pDeferred]);
        report({{ type: "slow-done", order, immediateInfo, deferredInfo }});
        return;
    }}

    if (changeInfo.status !== "complete" || !tab.url) return;

    if (!ranAllowed && tab.url.endsWith("/allowed")) {{
        ranAllowed = true;

        // 1. ISOLATED world func + args
        const isoRes = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: (a, b) => ({{
                sum: a + b,
                title: document.title,
                sawChrome: typeof chrome === "object" && typeof chrome.runtime === "object",
                sawPageSecret: window.__pageSecret !== undefined,
            }}),
            args: [19, 23],
        }});

        // 2. MAIN world func
        const mainRes = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            world: "MAIN",
            func: () => ({{
                secret: window.__pageSecret,
                sawChrome: typeof chrome !== "undefined",
            }}),
        }});

        // 3. files execution order, early throw continuation, and cross-file Promise settlement without deadlocking
        const filesRes = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            files: ["helper.js", "run.js"],
        }});
        const multiFilesRes = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            files: ["throw.js", "deferred_p1.js", "deferred_p2.js"],
        }});

        // 4. Promise awaiting, explicit null vs omitted undefined/rejected result, and single-read thenable
        const asyncRes = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: async (tag) => {{
                await new Promise(r => setTimeout(r, 20));
                return {{ awaited: true, tag }};
            }},
            args: ["promised"],
        }});
        const nullRes = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => null,
        }});
        const undefRes = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => undefined,
        }});
        const rejectedRes = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => Promise.reject(new Error("boom")),
        }});
        const thenableRes = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => {{
                let reads = 0;
                return {{
                    get then() {{
                        reads++;
                        return (resolve) => resolve({{ reads }});
                    }},
                }};
            }},
        }});
        const deepRes = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => {{
                let v = 42;
                for (let i = 0; i < 600; i++)
                    v = [v];
                return v;
            }},
        }});
        let deepDepth = 0;
        let deepCur = deepRes[0].result;
        while (Array.isArray(deepCur)) {{
            deepDepth++;
            deepCur = deepCur[0];
        }}

        // 5. allFrames injects top, same-origin frame-a, authorized remote-allowed, and permitted-initiator data: frame,
        //    while skipping unauthorized localhost iframe and its denied-initiator data: iframe.
        const allRes = await chrome.scripting.executeScript({{
            target: {{ tabId, allFrames: true }},
            func: () => location.protocol === "data:" ? document.getElementById("dt").textContent : location.pathname,
        }});
        const frameAEntry = allRes.find(r => r.result === "/frame-a");
        const remoteEntry = allRes.find(r => r.result === "/remote-allowed");
        const frameIdRes = await chrome.scripting.executeScript({{
            target: {{ tabId, frameIds: [frameAEntry.frameId] }},
            func: () => document.title,
        }});
        const remoteFrameRes = await chrome.scripting.executeScript({{
            target: {{ tabId, frameIds: [remoteEntry.frameId] }},
            func: () => document.getElementById("rtag").textContent,
        }});
        const docIdRes = await chrome.scripting.executeScript({{
            target: {{ tabId, documentIds: [frameAEntry.documentId] }},
            func: () => document.getElementById("tag").textContent,
        }});
        const badFrameErr = await expectReject(chrome.scripting.executeScript({{
            target: {{ tabId, frameIds: [999999] }},
            func: () => 1,
        }}));
        const badDocErr = await expectReject(chrome.scripting.executeScript({{
            target: {{ tabId, documentIds: ["missing-doc-id"] }},
            func: () => 1,
        }}));

        // 6. insertCSS + removeCSS:
        //    - Equal-specificity AUTHOR sheet cascades BEFORE page <style> (page wins), while higher-specificity AUTHOR wins.
        //    - Duplicate insertCSS + single removeCSS removes only the most recent matching sheet.
        //    - Multi-file USER insertCSS parses each file independently and supports removing a single file.
        //    - Unmatched removeCSS({{ files }}) with a non-existent path is a no-op.
        //    - Late @import inside a USER stylesheet applies after parent compilation.
        const readBoxStyle = async () => {{
            const r = await chrome.scripting.executeScript({{
                target: {{ tabId }},
                func: () => {{
                    const s = getComputedStyle(document.getElementById("box"));
                    return {{ bg: s.backgroundColor, color: s.color, borderTopColor: s.borderTopColor }};
                }},
            }});
            return r[0].result;
        }};
        const styleBefore = await readBoxStyle();
        await chrome.scripting.insertCSS({{
            target: {{ tabId }},
            origin: "AUTHOR",
            css: "#box {{ background-color: rgb(1, 2, 3); }}",
        }});
        await chrome.scripting.insertCSS({{
            target: {{ tabId }},
            origin: "USER",
            css: "#box {{ color: rgb(7, 8, 9); }}",
        }});
        const styleEqualAuthorAndNormalUser = await readBoxStyle();
        await chrome.scripting.insertCSS({{
            target: {{ tabId }},
            origin: "AUTHOR",
            css: "body #box {{ background-color: rgb(1, 2, 3); }}",
        }});
        await chrome.scripting.insertCSS({{
            target: {{ tabId }},
            origin: "AUTHOR",
            css: "body #box {{ background-color: rgb(1, 2, 3); }}",
        }});
        await chrome.scripting.insertCSS({{
            target: {{ tabId }},
            origin: "USER",
            files: ["bad.css", "style.css"],
        }});
        const styleBoth = await readBoxStyle();
        await chrome.scripting.removeCSS({{
            target: {{ tabId }},
            origin: "AUTHOR",
            css: "body #box {{ background-color: rgb(1, 2, 3); }}",
        }});
        const styleAfterOneDupRemoved = await readBoxStyle();
        await chrome.scripting.removeCSS({{
            target: {{ tabId }},
            origin: "AUTHOR",
            css: "body #box {{ background-color: rgb(1, 2, 3); }}",
        }});
        await chrome.scripting.removeCSS({{
            target: {{ tabId }},
            origin: "AUTHOR",
            css: "#box {{ background-color: rgb(1, 2, 3); }}",
        }});
        const styleAfterInlineRemoved = await readBoxStyle();
        await chrome.scripting.removeCSS({{
            target: {{ tabId }},
            origin: "USER",
            css: "#box {{ color: rgb(7, 8, 9); }}",
        }});
        await chrome.scripting.removeCSS({{
            target: {{ tabId }},
            origin: "USER",
            files: ["./style.css"],
        }});
        await chrome.scripting.removeCSS({{
            target: {{ tabId }},
            origin: "USER",
            files: ["never-existed.css"],
        }});
        const styleAfterAllRemoved = await readBoxStyle();

        // Late USER @import: force parent sheet compilation before delayed import completes, then verify style updates.
        const importCss = "@import url('" + BASE + "/late-import.css');";
        await chrome.scripting.insertCSS({{
            target: {{ tabId }},
            origin: "USER",
            css: importCss,
        }});
        const styleBeforeLateImport = await readBoxStyle();
        let styleAfterLateImport = styleBeforeLateImport;
        for (let attempt = 0; attempt < 20; attempt++) {{
            await new Promise(r => setTimeout(r, 25));
            styleAfterLateImport = await readBoxStyle();
            if (styleAfterLateImport.borderTopColor === "rgb(9, 8, 7)")
                break;
        }}
        await chrome.scripting.removeCSS({{
            target: {{ tabId }},
            origin: "USER",
            css: importCss,
        }});

        // USER @font-face inserted after AUTHOR @font-face still orders ahead of AUTHOR in document.fonts.
        const authorFontCss = "@font-face {{ font-family: 'ExtAuthorFace'; src: local('Arial'); }}";
        const userFontCss = "@font-face {{ font-family: 'ExtUserFace'; src: local('Arial'); }}";
        await chrome.scripting.insertCSS({{
            target: {{ tabId }},
            origin: "AUTHOR",
            css: authorFontCss,
        }});
        await chrome.scripting.insertCSS({{
            target: {{ tabId }},
            origin: "USER",
            css: userFontCss,
        }});
        const fontOrderRes = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => Array.from(document.fonts).map(f => f.family.replace(/['"]/g, "")),
        }});
        await chrome.scripting.removeCSS({{ target: {{ tabId }}, origin: "USER", css: userFontCss }});
        await chrome.scripting.removeCSS({{ target: {{ tabId }}, origin: "AUTHOR", css: authorFontCss }});

        // USER cascade layers:
        // - In CSS Cascade Level 5, a named-layer USER !important rule beats an unlayered USER !important rule.
        // - Named @keyframes rules follow layer precedence (@layer early, late -> `late` beats `early` even when defined first).
        const userLayerCss =
            "@layer early, late; " +
            "@layer early {{ #box {{ color: rgb(2, 4, 6) !important; }} }} " +
            "#box {{ color: rgb(8, 9, 10) !important; animation: extLayerFade 1s linear both !important; }} " +
            "@layer late {{ @keyframes extLayerFade {{ from {{ opacity: 0.2; }} to {{ opacity: 0.2; }} }} }} " +
            "@layer early {{ @keyframes extLayerFade {{ from {{ opacity: 0.6; }} to {{ opacity: 0.6; }} }} }}";
        await chrome.scripting.insertCSS({{
            target: {{ tabId }},
            origin: "USER",
            css: userLayerCss,
        }});
        const styleUserLayer = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => {{
                const s = getComputedStyle(document.getElementById("box"));
                return {{ color: s.color, opacity: s.opacity }};
            }},
        }});
        await chrome.scripting.removeCSS({{
            target: {{ tabId }},
            origin: "USER",
            css: userLayerCss,
        }});

        // 7. On-demand ISOLATED script can immediately open a runtime.connect port to background
        const portRes = await chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => new Promise((resolve) => {{
                const p = chrome.runtime.connect({{ name: "ondemand-port" }});
                p.onMessage.addListener((m) => resolve(m.pong));
                p.postMessage({{ ping: "hello-ondemand" }});
            }}),
        }});

        // 8. Self-detaching function returning a primitive rejects instead of succeeding,
        //    and a pending executeScript promise settles cleanly when its iframe is removed.
        const pendingFramePromise = expectReject(chrome.scripting.executeScript({{
            target: {{ tabId, frameIds: [frameAEntry.frameId] }},
            func: () => new Promise(() => {{}}),
        }}));
        const selfDetachErr = await expectReject(chrome.scripting.executeScript({{
            target: {{ tabId, frameIds: [frameAEntry.frameId] }},
            func: () => {{
                window.frameElement.remove();
                return "should-not-win";
            }},
        }}));
        const removedFrameErr = await pendingFramePromise;

        report({{
            type: "allowed-done",
            iso: isoRes,
            main: mainRes,
            files: filesRes,
            multiFiles: multiFilesRes,
            asyncRes,
            nullHasResult: "result" in nullRes[0],
            nullValue: nullRes[0].result,
            undefHasResult: "result" in undefRes[0],
            rejectedHasResult: "result" in rejectedRes[0],
            thenableRes,
            deepDepth,
            deepLeaf: deepCur,
            fontOrder: fontOrderRes[0].result,
            allPaths: allRes.map(r => r.result),
            allFrameCount: allRes.length,
            frameIdRes,
            remoteFrameRes,
            docIdRes,
            badFrameErr,
            badDocErr,
            styleBefore,
            styleEqualAuthorAndNormalUser,
            styleBoth,
            styleAfterOneDupRemoved,
            styleAfterInlineRemoved,
            styleAfterAllRemoved,
            styleBeforeLateImport,
            styleAfterLateImport,
            styleUserLayer: styleUserLayer[0].result,
            portReply: portRes[0].result,
            selfDetachErr,
            removedFrameErr,
        }});
    }} else if (!ranDenied && tab.url.endsWith("/denied")) {{
        ranDenied = true;
        const execErr = await expectReject(chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => 1,
        }}));
        const insertErr = await expectReject(chrome.scripting.insertCSS({{
            target: {{ tabId }},
            css: "body {{ color: red; }}",
        }}));
        const removeErr = await expectReject(chrome.scripting.removeCSS({{
            target: {{ tabId }},
            css: "body {{ color: red; }}",
        }}));
        report({{ type: "denied-done", execErr, insertErr, removeErr }});
    }} else if (!ranExtPage && tab.url.startsWith("chrome-extension://") && tab.url.endsWith("/options.html")) {{
        ranExtPage = true;
        const extErr = await expectReject(chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => 1,
        }}));
        report({{ type: "ext-page-done", extErr }});
    }}
}});

report({{ type: "bg-ready", extUrl: chrome.runtime.getURL("options.html") }});
)JS",
        port_str);

    auto helper_js = "globalThis.__helperBase = 100;"sv;
    auto run_js = "globalThis.__helperBase + 5;"sv;
    auto throw_js = "globalThis.__seq = ['throw']; throw new Error('early-fail');"sv;
    auto deferred_p1_js = "globalThis.__seq.push('p1-start'); new Promise(r => { globalThis.__resolveP1 = () => { globalThis.__seq.push('p1-resolved'); r('from-p1'); }; });"sv;
    auto deferred_p2_js = "globalThis.__seq.push('p2-run'); globalThis.__resolveP1(); globalThis.__seq.join(',');"sv;
    auto bad_css = ".unclosed-rule { color: red;"sv;
    auto style_css = "#box { color: __MSG_boxColor__ !important; }"sv;
    auto messages_json = R"JSON({ "boxColor": { "message": "rgb(4, 5, 6)" } })JSON"sv;
    auto options_html = "<!doctype html><html><body>Options</body></html>"sv;
    auto manifest_json = R"({
        "manifest_version": 3,
        "name": "Scripting On-Demand Test",
        "version": "1.0",
        "default_locale": "en",
        "permissions": ["tabs", "scripting"],
        "host_permissions": ["http://127.0.0.1/*", "http://127.0.0.2/*"],
        "background": { "service_worker": "bg.js" }
    })"sv;

    Array zip_entries {
        Array<StringView, 2> { "manifest.json"sv, manifest_json },
        Array<StringView, 2> { "bg.js"sv, bg_js },
        Array<StringView, 2> { "helper.js"sv, helper_js },
        Array<StringView, 2> { "run.js"sv, run_js },
        Array<StringView, 2> { "throw.js"sv, throw_js },
        Array<StringView, 2> { "deferred_p1.js"sv, deferred_p1_js },
        Array<StringView, 2> { "deferred_p2.js"sv, deferred_p2_js },
        Array<StringView, 2> { "bad.css"sv, bad_css },
        Array<StringView, 2> { "style.css"sv, style_css },
        Array<StringView, 2> { "_locales/en/messages.json"sv, messages_json },
        Array<StringView, 2> { "options.html"sv, options_html },
    };
    auto zip_bytes = stored_zip(zip_entries);
    auto zip_path = LexicalPath::join(environment.directory(), "scripting-test.zip"sv).string();
    {
        auto file = TRY_OR_FAIL(Core::File::open(zip_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        TRY_OR_FAIL(file->write_until_depleted(zip_bytes));
    }

    TRY_OR_FAIL(app->install_extension(zip_path));
    Core::EventLoop::current().spin_until([&] { return server.has_event("bg-ready"sv); });
    auto ext_options_url = URL::Parser::basic_parse(*server.event("bg-ready"sv).get_string("extUrl"sv)).release_value();

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    // 1–8: Allowed page + local, remote & data: subframes + CSS cascade/removal/late @import + port interop
    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        view->load_from_browser_ui(server.url("/allowed"sv));
        Core::EventLoop::current().spin_until([&] {
            return server.has_event("allowed-done"sv) && server.has_event("ondemand-port-ok"sv);
        });

        auto const& done = server.event("allowed-done"sv);

        auto const& iso = done.get_array("iso"sv)->at(0).as_object();
        EXPECT_EQ(iso.get_u64("frameId"sv), 0u);
        auto const& iso_val = iso.get_object("result"sv).value();
        EXPECT_EQ(iso_val.get_i64("sum"sv), 42);
        EXPECT_EQ(iso_val.get_string("title"sv), "Allowed Page"sv);
        EXPECT_EQ(iso_val.get_bool("sawChrome"sv), true);
        EXPECT_EQ(iso_val.get_bool("sawPageSecret"sv), false);

        auto const& main_val = done.get_array("main"sv)->at(0).as_object().get_object("result"sv).value();
        EXPECT_EQ(main_val.get_string("secret"sv), "top-secret"sv);
        EXPECT_EQ(main_val.get_bool("sawChrome"sv), false);

        EXPECT_EQ(done.get_array("files"sv)->at(0).as_object().get_i64("result"sv), 105);
        EXPECT_EQ(done.get_array("multiFiles"sv)->at(0).as_object().get_string("result"sv), "throw,p1-start,p2-run,p1-resolved"sv);

        auto const& async_val = done.get_array("asyncRes"sv)->at(0).as_object().get_object("result"sv).value();
        EXPECT_EQ(async_val.get_bool("awaited"sv), true);
        EXPECT_EQ(async_val.get_string("tag"sv), "promised"sv);

        EXPECT_EQ(done.get_bool("nullHasResult"sv), true);
        EXPECT(done.get("nullValue"sv)->is_null());
        EXPECT_EQ(done.get_bool("undefHasResult"sv), false);
        EXPECT_EQ(done.get_bool("rejectedHasResult"sv), false);
        EXPECT_EQ(done.get_array("thenableRes"sv)->at(0).as_object().get_object("result"sv)->get_i64("reads"sv), 1);
        EXPECT_EQ(done.get_i64("deepDepth"sv), 600);
        EXPECT_EQ(done.get_i64("deepLeaf"sv), 42);
        auto const& font_order = done.get_array("fontOrder"sv).value();
        Optional<size_t> user_font_idx;
        Optional<size_t> author_font_idx;
        for (size_t i = 0; i < font_order.size(); ++i) {
            if (font_order.at(i).as_string() == "ExtUserFace"sv)
                user_font_idx = i;
            else if (font_order.at(i).as_string() == "ExtAuthorFace"sv)
                author_font_idx = i;
        }
        EXPECT(user_font_idx.has_value());
        EXPECT(author_font_idx.has_value());
        EXPECT(*user_font_idx < *author_font_idx);

        EXPECT_EQ(done.get_u64("allFrameCount"sv), 4u);
        auto const& all_paths = done.get_array("allPaths"sv).value();
        bool saw_allowed = false;
        bool saw_frame_a = false;
        bool saw_remote = false;
        bool saw_data_allowed = false;
        bool saw_data_denied = false;
        all_paths.for_each([&](auto const& value) {
            auto path = value.as_string();
            if (path == "/allowed"sv)
                saw_allowed = true;
            else if (path == "/frame-a"sv)
                saw_frame_a = true;
            else if (path == "/remote-allowed"sv)
                saw_remote = true;
            else if (path == "data-allowed"sv)
                saw_data_allowed = true;
            else if (path == "data-denied"sv)
                saw_data_denied = true;
        });
        EXPECT(saw_allowed);
        EXPECT(saw_frame_a);
        EXPECT(saw_remote);
        EXPECT(saw_data_allowed);
        EXPECT(!saw_data_denied);

        EXPECT_EQ(done.get_array("frameIdRes"sv)->size(), 1u);
        EXPECT_EQ(done.get_array("frameIdRes"sv)->at(0).as_object().get_string("result"sv), "Frame A"sv);

        EXPECT_EQ(done.get_array("remoteFrameRes"sv)->size(), 1u);
        EXPECT_EQ(done.get_array("remoteFrameRes"sv)->at(0).as_object().get_string("result"sv), "remote-allowed"sv);

        EXPECT_EQ(done.get_array("docIdRes"sv)->size(), 1u);
        EXPECT_EQ(done.get_array("docIdRes"sv)->at(0).as_object().get_string("result"sv), "frame-a"sv);

        EXPECT(done.get_string("badFrameErr"sv).has_value());
        EXPECT(done.get_string("badDocErr"sv).has_value());

        auto const& style_before = done.get_object("styleBefore"sv).value();
        EXPECT_EQ(style_before.get_string("bg"sv), "rgb(10, 20, 30)"sv);
        EXPECT_EQ(style_before.get_string("color"sv), "rgb(40, 50, 60)"sv);
        EXPECT_EQ(style_before.get_string("borderTopColor"sv), "rgb(11, 22, 33)"sv);

        auto const& style_equal_author = done.get_object("styleEqualAuthorAndNormalUser"sv).value();
        EXPECT_EQ(style_equal_author.get_string("bg"sv), "rgb(10, 20, 30)"sv);
        EXPECT_EQ(style_equal_author.get_string("color"sv), "rgb(40, 50, 60)"sv);

        auto const& style_both = done.get_object("styleBoth"sv).value();
        EXPECT_EQ(style_both.get_string("bg"sv), "rgb(1, 2, 3)"sv);
        EXPECT_EQ(style_both.get_string("color"sv), "rgb(4, 5, 6)"sv);

        auto const& style_after_one_dup = done.get_object("styleAfterOneDupRemoved"sv).value();
        EXPECT_EQ(style_after_one_dup.get_string("bg"sv), "rgb(1, 2, 3)"sv);
        EXPECT_EQ(style_after_one_dup.get_string("color"sv), "rgb(4, 5, 6)"sv);

        auto const& style_after_inline = done.get_object("styleAfterInlineRemoved"sv).value();
        EXPECT_EQ(style_after_inline.get_string("bg"sv), "rgb(10, 20, 30)"sv);
        EXPECT_EQ(style_after_inline.get_string("color"sv), "rgb(4, 5, 6)"sv);

        auto const& style_after_all = done.get_object("styleAfterAllRemoved"sv).value();
        EXPECT_EQ(style_after_all.get_string("bg"sv), "rgb(10, 20, 30)"sv);
        EXPECT_EQ(style_after_all.get_string("color"sv), "rgb(40, 50, 60)"sv);

        EXPECT_EQ(done.get_object("styleBeforeLateImport"sv)->get_string("borderTopColor"sv), "rgb(11, 22, 33)"sv);
        EXPECT_EQ(done.get_object("styleAfterLateImport"sv)->get_string("borderTopColor"sv), "rgb(9, 8, 7)"sv);
        EXPECT_EQ(done.get_object("styleUserLayer"sv)->get_string("color"sv), "rgb(2, 4, 6)"sv);
        EXPECT_EQ(done.get_object("styleUserLayer"sv)->get_string("opacity"sv), "0.2"sv);

        EXPECT_EQ(done.get_string("portReply"sv), "hello-ondemand"sv);
        EXPECT_EQ(server.event("ondemand-port-ok"sv).get_string("ping"sv), "hello-ondemand"sv);
        EXPECT_EQ(done.get_string("selfDetachErr"sv), "The frame was removed."sv);
        EXPECT_EQ(done.get_string("removedFrameErr"sv), "The frame was removed."sv);
    }

    // 9. injectImmediately: true runs before document_idle while default waits for document_idle
    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        view->load_from_browser_ui(server.url("/slow"sv));
        Core::EventLoop::current().spin_until([&] { return server.has_event("slow-done"sv); });
        auto const& slow = server.event("slow-done"sv);
        auto const& order = slow.get_array("order"sv).value();
        EXPECT_EQ(order.size(), 2u);
        EXPECT_EQ(order.at(0).as_string(), "immediate"sv);
        EXPECT_EQ(order.at(1).as_string(), "deferred"sv);
        auto const& immediate_info = slow.get_object("immediateInfo"sv).value();
        auto const& deferred_info = slow.get_object("deferredInfo"sv).value();
        EXPECT_NE(immediate_info.get_string("readyState"sv).value(), "complete"sv);
        EXPECT_EQ(immediate_info.get_bool("slowRan"sv), false);
        EXPECT_EQ(deferred_info.get_string("readyState"sv).value(), "complete"sv);
        EXPECT_EQ(deferred_info.get_bool("slowRan"sv), true);
    }

    // 10. Unauthorized host rejection
    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        view->load_from_browser_ui(server.localhost_url("/denied"sv));
        Core::EventLoop::current().spin_until([&] { return server.has_event("denied-done"sv); });
        auto const& denied = server.event("denied-done"sv);
        EXPECT(denied.get_string("execErr"sv).has_value());
        EXPECT(denied.get_string("insertErr"sv).has_value());
        EXPECT(denied.get_string("removeErr"sv).has_value());
    }

    // 11. chrome-extension:// target rejection
    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        view->load_from_browser_ui(ext_options_url);
        Core::EventLoop::current().spin_until([&] { return server.has_event("ext-page-done"sv); });
        EXPECT(server.event("ext-page-done"sv).get_string("extErr"sv).has_value());
    }

    // 12. activeTab grants scripting without host_permissions, revokes on same-URL CSP sandbox reload
    //     (without reviving on a subsequent normal reload), and revokes on cross-origin navigation.
    {
        auto bg2_js = ByteString::formatted(R"JS(
const BASE = "http://127.0.0.1:{}";
function report(payload) {{
    fetch(BASE + "/event?data=" + encodeURIComponent(JSON.stringify(payload))).catch(() => {{}});
}}
async function expectReject(promise) {{
    try {{
        await promise;
        return null;
    }} catch (e) {{
        return e && e.message ? e.message : String(e);
    }}
}}
let targetTabId = null;
let phase = 0;
chrome.tabs.onUpdated.addListener(async (tabId, changeInfo) => {{
    if (changeInfo.status !== "complete") return;
    if (targetTabId === null) {{
        targetTabId = tabId;
        phase = 1;
        const err = await expectReject(chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => document.title,
        }}));
        report({{ type: "activetab-before", err }});
    }} else if (tabId === targetTabId && phase === 2) {{
        phase = 3;
        const err = await expectReject(chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => document.title,
        }}));
        report({{ type: "activetab-sandbox-revoked", err }});
    }} else if (tabId === targetTabId && phase === 3) {{
        phase = 4;
        const err = await expectReject(chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => document.title,
        }}));
        report({{ type: "activetab-still-revoked", err }});
    }} else if (tabId === targetTabId && phase === 5) {{
        phase = 6;
        const err = await expectReject(chrome.scripting.executeScript({{
            target: {{ tabId }},
            func: () => document.title,
        }}));
        report({{ type: "activetab-cross-origin-revoked", err }});
    }}
}});
chrome.action.onClicked.addListener(async (tab) => {{
    const res = await chrome.scripting.executeScript({{
        target: {{ tabId: tab.id }},
        func: () => document.title,
    }});
    if (phase === 1) {{
        phase = 2;
        report({{ type: "activetab-granted-1", url: tab.url || null, title: res[0].result }});
    }} else if (phase === 4) {{
        phase = 5;
        report({{ type: "activetab-granted-2", url: tab.url || null, title: res[0].result }});
    }}
}});
report({{ type: "bg2-ready", extId: chrome.runtime.id }});
)JS",
            port_str);

        auto manifest2_json = R"({
            "manifest_version": 3,
            "name": "ActiveTab Scripting Test",
            "version": "1.0",
            "permissions": ["scripting", "activeTab"],
            "action": {},
            "background": { "service_worker": "bg.js" }
        })"sv;

        Array zip2_entries {
            Array<StringView, 2> { "manifest.json"sv, manifest2_json },
            Array<StringView, 2> { "bg.js"sv, bg2_js },
        };
        auto zip2_bytes = stored_zip(zip2_entries);
        auto zip2_path = LexicalPath::join(environment.directory(), "activetab-test.zip"sv).string();
        {
            auto file = TRY_OR_FAIL(Core::File::open(zip2_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
            TRY_OR_FAIL(file->write_until_depleted(zip2_bytes));
        }
        TRY_OR_FAIL(app->install_extension(zip2_path));
        Core::EventLoop::current().spin_until([&] { return server.has_event("bg2-ready"sv); });
        auto ext2_id = *server.event("bg2-ready"sv).get_string("extId"sv);

        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        view->load_from_browser_ui(server.url("/activetab"sv));
        Core::EventLoop::current().spin_until([&] { return server.has_event("activetab-before"sv); });
        EXPECT(server.event("activetab-before"sv).get_string("err"sv).has_value());

        // Click action -> activeTab granted
        app->extension_host()->click_action(ext2_id, view->view_id());
        Core::EventLoop::current().spin_until([&] { return server.has_event("activetab-granted-1"sv); });
        auto const& granted_ev = server.event("activetab-granted-1"sv);
        EXPECT(granted_ev.get_string("url"sv).has_value());
        EXPECT(granted_ev.get_string("url"sv)->ends_with_bytes("/activetab"sv));
        EXPECT_EQ(granted_ev.get_string("title"sv), "ActiveTab Page"sv);

        // Same-URL reload returning CSP sandbox (opaque origin) -> revoked
        view->load_from_browser_ui(server.url("/activetab"sv));
        Core::EventLoop::current().spin_until([&] { return server.has_event("activetab-sandbox-revoked"sv); });
        EXPECT(server.event("activetab-sandbox-revoked"sv).get_string("err"sv).has_value());

        // Subsequent same-URL reload back to normal tuple origin -> must stay revoked (not revived)
        view->load_from_browser_ui(server.url("/activetab"sv));
        Core::EventLoop::current().spin_until([&] { return server.has_event("activetab-still-revoked"sv); });
        EXPECT(server.event("activetab-still-revoked"sv).get_string("err"sv).has_value());

        // Re-grant on normal origin, then navigate cross-origin -> revoked
        app->extension_host()->click_action(ext2_id, view->view_id());
        Core::EventLoop::current().spin_until([&] { return server.has_event("activetab-granted-2"sv); });
        EXPECT_EQ(server.event("activetab-granted-2"sv).get_string("title"sv), "ActiveTab Page"sv);

        view->load_from_browser_ui(server.localhost_url("/denied"sv));
        Core::EventLoop::current().spin_until([&] { return server.has_event("activetab-cross-origin-revoked"sv); });
        EXPECT(server.event("activetab-cross-origin-revoked"sv).get_string("err"sv).has_value());
    }
}
