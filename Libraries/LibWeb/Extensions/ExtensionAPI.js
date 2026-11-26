// The chrome.* API of an extension. This evaluates to a function, which the engine calls in each context that the
// extension's code runs in: the isolated world of its content scripts in a page ("content"), or one of its own pages
// ("page"), such as its background page or popup. It returns the API object, and a function the engine calls to tell
// the context of something: a change of storage, the result of a call it made, or a message.
(function (native, extensionId, manifestJSON, messagesJSON, uiLanguage, contextKind, contextId) {
    "use strict";

    const manifest = JSON.parse(manifestJSON);
    const messages = JSON.parse(messagesJSON);
    const baseURL = "chrome-extension://" + extensionId + "/";
    const isPage = contextKind === "page";
    const noReceiverError = "Could not establish connection. Receiving end does not exist.";
    const portClosedError = "The message port closed before a response was received.";

    let lastError;

    // The events of the API that the browser tells the extension's pages of, by name.
    const browserEvents = {};

    function reportError(error) {
        queueMicrotask(() => {
            throw error;
        });
    }

    function createEvent() {
        const listeners = [];
        const event = {
            addListener(listener) {
                if (typeof listener === "function" && !listeners.includes(listener)) listeners.push(listener);
            },
            removeListener(listener) {
                const index = listeners.indexOf(listener);
                if (index !== -1) listeners.splice(index, 1);
            },
            hasListener(listener) {
                return listeners.includes(listener);
            },
            hasListeners() {
                return listeners.length > 0;
            },
        };
        const dispatch = (...args) => {
            for (const listener of listeners.slice()) {
                try {
                    listener(...args);
                } catch (error) {
                    reportError(error);
                }
            }
        };
        return [event, dispatch, listeners];
    }

    function toError(thrown) {
        return thrown instanceof Error ? thrown : new Error(String(thrown));
    }

    // Calls the callback, if any, with runtime.lastError set to any error. Otherwise, gives back a promise.
    function finish(callback, promise) {
        if (typeof callback !== "function") return promise;
        promise.then(
            result => {
                try {
                    callback(result);
                } catch (thrown) {
                    reportError(thrown);
                }
            },
            error => {
                lastError = { message: toError(error).message };
                try {
                    callback();
                } catch (thrown) {
                    reportError(thrown);
                } finally {
                    lastError = undefined;
                }
            }
        );
        return undefined;
    }

    // Settles an API call whose result is known here.
    function settle(callback, compute) {
        let promise;
        try {
            promise = Promise.resolve(compute());
        } catch (thrown) {
            promise = Promise.reject(toError(thrown));
        }
        return finish(callback, promise);
    }

    function popCallback(args) {
        return typeof args[args.length - 1] === "function" ? args.pop() : undefined;
    }

    // Calls into the browser process, for what only it knows of or can do.
    const pendingCalls = new Map();
    function callBrowser(method, ...args) {
        return new Promise((resolve, reject) => {
            const callId = native.call(contextId, method, JSON.stringify(args));
            pendingCalls.set(callId, { resolve, reject });
        });
    }

    function browserFunction(method, argumentCount) {
        return (...args) => {
            const callback = popCallback(args);
            args.length = Math.min(args.length, argumentCount);
            return finish(callback, callBrowser(method, ...args));
        };
    }

    // Functions that are not supported, but that an extension calls as part of its setup, succeed doing nothing.
    function noop(result) {
        return (...args) => settle(popCallback(args), () => result);
    }

    // runtime

    const [onMessage, , messageListeners] = createEvent();
    const [onConnect] = createEvent();
    const [onInstalled] = createEvent();
    const [onStartup] = createEvent();

    function createDisconnectedPort(name) {
        const [onPortMessage] = createEvent();
        const [onDisconnect, dispatchDisconnect] = createEvent();
        const port = {
            name,
            onMessage: onPortMessage,
            onDisconnect,
            postMessage() {},
            disconnect() {},
        };
        Promise.resolve().then(() => {
            lastError = { message: noReceiverError };
            try {
                dispatchDisconnect(port);
            } finally {
                lastError = undefined;
            }
        });
        return port;
    }

    function sendMessage(target, message, callback) {
        let serialized;
        try {
            serialized = JSON.stringify(message === undefined ? null : message);
        } catch (thrown) {
            return finish(callback, Promise.reject(toError(thrown)));
        }
        const promise = callBrowser(
            "runtime.sendMessage",
            target,
            serialized,
            location.href,
            window === window.top
        ).then(response => (response === undefined ? undefined : JSON.parse(response)));
        return finish(callback, promise);
    }

    const runtime = {
        id: extensionId,
        getManifest() {
            return JSON.parse(manifestJSON);
        },
        getURL(path) {
            return baseURL + String(path).replace(/^\/+/, "");
        },
        sendMessage(...args) {
            const callback = popCallback(args);
            // sendMessage([extensionId,] message [, options]): a first string is an extension's ID only if it looks
            // like one, and messages to other extensions cannot be sent.
            const isExtensionId = value => typeof value === "string" && /^[a-p]{32}$/.test(value);
            if (args.length >= 2 && (args[0] === null || args[0] === undefined)) {
                args.shift();
            } else if (args.length >= 2 && isExtensionId(args[0])) {
                if (args[0] !== extensionId) return finish(callback, Promise.reject(new Error(noReceiverError)));
                args.shift();
            }
            return sendMessage({ tabId: null }, args[0], callback);
        },
        connect(...args) {
            const info = args.find(arg => arg && typeof arg === "object");
            return createDisconnectedPort(info && info.name !== undefined ? String(info.name) : "");
        },
        getPlatformInfo: noop({ os: "linux", arch: "x86-64", nacl_arch: "x86-64" }),
        onMessage,
        onConnect,
        onInstalled,
        onStartup,
    };
    Object.defineProperty(runtime, "lastError", {
        get() {
            return lastError;
        },
        enumerable: true,
    });

    // i18n

    const predefinedMessages = {
        "@@extension_id": extensionId,
        "@@ui_locale": uiLanguage.replace("-", "_"),
        "@@bidi_dir": "ltr",
        "@@bidi_reversed_dir": "rtl",
        "@@bidi_start_edge": "left",
        "@@bidi_end_edge": "right",
    };

    const i18n = {
        getMessage(name, substitutions) {
            name = String(name);
            if (Object.hasOwn(predefinedMessages, name)) return predefinedMessages[name];

            const entry = messages[name.toLowerCase()];
            if (!entry || typeof entry.message !== "string") return "";

            let values = [];
            if (Array.isArray(substitutions)) values = substitutions.slice(0, 9).map(String);
            else if (substitutions !== undefined && substitutions !== null) values = [String(substitutions)];

            const placeholders = {};
            for (const [key, value] of Object.entries(entry.placeholders || {}))
                placeholders[key.toLowerCase()] = value && value.content !== undefined ? String(value.content) : "";

            const substitute = text => text.replace(/\$(\d)/g, (_, digit) => values[digit - 1] ?? "");

            return entry.message.replace(/\$\$|\$([A-Za-z0-9_@]+)\$|\$(\d)/g, (match, placeholder, digit) => {
                if (match === "$$") return "$";
                if (placeholder !== undefined) {
                    const key = placeholder.toLowerCase();
                    return Object.hasOwn(placeholders, key) ? substitute(placeholders[key]) : match;
                }
                return values[digit - 1] ?? "";
            });
        },
        getUILanguage() {
            return uiLanguage;
        },
        getAcceptLanguages(callback) {
            return settle(callback, () => Array.from(navigator.languages));
        },
        detectLanguage(text, callback) {
            return settle(callback, () => ({ isReliable: false, languages: [] }));
        },
    };

    // storage

    const [onStorageChanged, dispatchStorageChanged] = createEvent();
    const storageAreaDispatchers = {};

    function createStorageArea(areaName, quotaBytes, readOnly) {
        const read = () => JSON.parse(native.storageGet(extensionId, areaName));
        const write = changes => {
            if (readOnly) throw new Error("This storage area is read-only.");
            native.storageApply(extensionId, areaName, JSON.stringify(changes));
        };
        const keyList = keys => (Array.isArray(keys) ? keys : [keys]).map(String);
        const [onChanged, dispatch] = createEvent();
        storageAreaDispatchers[areaName] = dispatch;

        return {
            QUOTA_BYTES: quotaBytes,
            onChanged,
            get(keys, callback) {
                if (typeof keys === "function") {
                    callback = keys;
                    keys = null;
                }
                return settle(callback, () => {
                    const items = read();
                    if (keys === null || keys === undefined) return items;
                    const result = {};
                    if (typeof keys === "string" || Array.isArray(keys)) {
                        for (const key of keyList(keys)) {
                            if (Object.hasOwn(items, key)) result[key] = items[key];
                        }
                        return result;
                    }
                    if (typeof keys === "object") {
                        for (const key of Object.keys(keys))
                            result[key] = Object.hasOwn(items, key) ? items[key] : keys[key];
                        return result;
                    }
                    throw new TypeError("Invalid storage keys");
                });
            },
            getKeys(callback) {
                return settle(callback, () => Object.keys(read()));
            },
            getBytesInUse(keys, callback) {
                if (typeof keys === "function") {
                    callback = keys;
                    keys = null;
                }
                return settle(callback, () => {
                    const items = read();
                    const wanted = keys === null || keys === undefined ? Object.keys(items) : keyList(keys);
                    let total = 0;
                    for (const key of wanted) {
                        if (Object.hasOwn(items, key)) total += key.length + JSON.stringify(items[key]).length;
                    }
                    return total;
                });
            },
            set(items, callback) {
                return settle(callback, () => {
                    if (typeof items !== "object" || items === null)
                        throw new TypeError("Storage items must be an object");
                    const set = {};
                    for (const key of Object.keys(items)) {
                        if (items[key] !== undefined) set[key] = items[key];
                    }
                    write({ set });
                });
            },
            remove(keys, callback) {
                return settle(callback, () => write({ remove: keyList(keys) }));
            },
            clear(callback) {
                return settle(callback, () => write({ clear: true }));
            },
        };
    }

    const storage = {
        local: createStorageArea("local", 10485760),
        sync: createStorageArea("sync", 102400),
        onChanged: onStorageChanged,
    };
    if (isPage) {
        storage.session = createStorageArea("session", 10485760);
        storage.managed = createStorageArea("managed", 0, true);
    }

    const chrome = {
        runtime,
        i18n,
        storage,
        extension: {
            getURL: runtime.getURL,
            inIncognitoContext: false,
            sendMessage: runtime.sendMessage,
            onMessage,
        },
    };

    // Content scripts can get at the shadow roots of elements, including closed ones.
    if (!isPage) {
        chrome.dom = {
            openOrClosedShadowRoot(element) {
                return element instanceof Element ? element.shadowRoot : null;
            },
        };
    }

    if (isPage) {
        runtime.openOptionsPage = browserFunction("runtime.openOptionsPage", 0);
        runtime.reload = () => location.reload();
        runtime.setUninstallURL = noop();
        runtime.requestUpdateCheck = noop("no_update");

        // tabs and windows

        const [onTabUpdated] = createEvent();
        const [onTabActivated] = createEvent();
        const [onTabRemoved] = createEvent();
        const [onTabCreated] = createEvent();
        chrome.tabs = {
            TAB_ID_NONE: -1,
            query: browserFunction("tabs.query", 1),
            get: browserFunction("tabs.get", 1),
            create: browserFunction("tabs.create", 1),
            update(...args) {
                const callback = popCallback(args);
                const [tabId, properties] = typeof args[0] === "number" ? args : [null, args[0]];
                return finish(callback, callBrowser("tabs.update", tabId, properties));
            },
            reload(...args) {
                const callback = popCallback(args);
                const tabId = typeof args[0] === "number" ? args[0] : null;
                return finish(callback, callBrowser("tabs.reload", tabId));
            },
            remove: browserFunction("tabs.remove", 1),
            sendMessage(tabId, message, ...rest) {
                const callback = popCallback(rest);
                const options = rest[0] || {};
                if (options.frameId !== undefined && options.frameId !== 0)
                    return finish(
                        callback,
                        Promise.reject(new Error("Messages can only go to a tab's top-level frame"))
                    );
                return sendMessage({ tabId, frameId: options.frameId }, message, callback);
            },
            onUpdated: onTabUpdated,
            onActivated: onTabActivated,
            onRemoved: onTabRemoved,
            onCreated: onTabCreated,
        };
        chrome.windows = {
            WINDOW_ID_NONE: -1,
            WINDOW_ID_CURRENT: -2,
            create(...args) {
                const callback = popCallback(args);
                const info = args[0] || {};
                const url = Array.isArray(info.url) ? info.url[0] : info.url;
                const promise = callBrowser("tabs.create", { url, active: true }).then(tab => ({
                    id: 1,
                    focused: true,
                    tabs: [tab],
                }));
                return finish(callback, promise);
            },
            update: noop({ id: 1, focused: true }),
            get: noop({ id: 1, focused: true }),
            getCurrent: noop({ id: 1, focused: true }),
            getLastFocused: noop({ id: 1, focused: true }),
            getAll: noop([{ id: 1, focused: true }]),
            onFocusChanged: createEvent()[0],
            onRemoved: createEvent()[0],
        };

        // permissions: an extension has what its manifest asks for.

        const grantedPermissions = {
            permissions: Array.isArray(manifest.permissions) ? manifest.permissions.slice() : [],
            origins: Array.isArray(manifest.host_permissions)
                ? manifest.host_permissions.slice()
                : (manifest.permissions || []).filter(permission => /^(\*|https?|file|<all_urls>)/.test(permission)),
        };
        // A granted host pattern covers a requested one if it covers its scheme (where * is http and https, and
        // <all_urls> is every scheme) and its host (where *.example.com is example.com and its subdomains). Host
        // permissions are for any path.
        const parsePattern = pattern => {
            const match = /^([^:]+):\/\/([^/]*)\//.exec(pattern);
            if (!match) return null;
            const hostAndPort = /^(.*?)(?::(\d+|\*))?$/.exec(match[2].toLowerCase());
            return { scheme: match[1].toLowerCase(), host: hostAndPort[1], port: hostAndPort[2] };
        };
        const coversHost = (granted, requested) => {
            if (granted === "*" || granted === requested) return true;
            if (!granted.startsWith("*.")) return false;
            const domain = granted.slice(2);
            const requestedDomain = requested.startsWith("*.") ? requested.slice(2) : requested;
            return requestedDomain === domain || requestedDomain.endsWith("." + domain);
        };
        // A grant without a port, or with any port, is for every port. Otherwise, the ports have to be the same.
        const coversPort = (granted, requested) => granted === undefined || granted === "*" || granted === requested;
        const patternContains = (granted, requested) => {
            if (granted === requested || granted === "<all_urls>") return true;
            const grantedPattern = parsePattern(granted);
            const requestedPattern = parsePattern(requested);
            if (!grantedPattern || !requestedPattern) return false;
            const coversScheme =
                grantedPattern.scheme === requestedPattern.scheme ||
                (grantedPattern.scheme === "*" && /^(https?|\*)$/.test(requestedPattern.scheme));
            return (
                coversScheme &&
                coversHost(grantedPattern.host, requestedPattern.host) &&
                coversPort(grantedPattern.port, requestedPattern.port)
            );
        };
        const containsPermissions = permissions =>
            (permissions.permissions || []).every(name => grantedPermissions.permissions.includes(name)) &&
            (permissions.origins || []).every(origin =>
                grantedPermissions.origins.some(granted => patternContains(granted, origin))
            );
        chrome.permissions = {
            getAll: noop(grantedPermissions),
            contains(permissions, callback) {
                return settle(callback, () => containsPermissions(permissions || {}));
            },
            // NB: There is no prompt to grant more, so an extension gets nothing besides what its manifest asks for.
            request(permissions, callback) {
                return settle(callback, () => containsPermissions(permissions || {}));
            },
            remove: noop(false),
            onAdded: createEvent()[0],
            onRemoved: createEvent()[0],
        };

        // declarativeNetRequest

        chrome.declarativeNetRequest = {
            MAX_NUMBER_OF_DYNAMIC_RULES: 30000,
            MAX_NUMBER_OF_DYNAMIC_AND_SESSION_RULES: 30000,
            MAX_NUMBER_OF_SESSION_RULES: 5000,
            MAX_NUMBER_OF_ENABLED_STATIC_RULESETS: 50,
            MAX_NUMBER_OF_REGEX_RULES: 1000,
            GUARANTEED_MINIMUM_STATIC_RULES: 30000,
            DYNAMIC_RULESET_ID: "_dynamic",
            SESSION_RULESET_ID: "_session",
            getDynamicRules: browserFunction("declarativeNetRequest.getDynamicRules", 1),
            getSessionRules: browserFunction("declarativeNetRequest.getSessionRules", 1),
            updateDynamicRules: browserFunction("declarativeNetRequest.updateDynamicRules", 1),
            updateSessionRules: browserFunction("declarativeNetRequest.updateSessionRules", 1),
            getEnabledRulesets: browserFunction("declarativeNetRequest.getEnabledRulesets", 0),
            updateEnabledRulesets: browserFunction("declarativeNetRequest.updateEnabledRulesets", 1),
            getAvailableStaticRuleCount: browserFunction("declarativeNetRequest.getAvailableStaticRuleCount", 0),
            getDisabledRuleIds: browserFunction("declarativeNetRequest.getDisabledRuleIds", 1),
            updateStaticRules: browserFunction("declarativeNetRequest.updateStaticRules", 1),
            isRegexSupported: browserFunction("declarativeNetRequest.isRegexSupported", 1),
            getMatchedRules: noop({ rulesMatchedInfo: [] }),
            setExtensionActionOptions: noop(),
            testMatchOutcome: browserFunction("declarativeNetRequest.testMatchOutcome", 1),
            onRuleMatchedDebug: createEvent()[0],
        };

        // scripting: registered content scripts are kept, but only the manifest's run.

        const localFunction =
            compute =>
            (...args) => {
                const callback = popCallback(args);
                return settle(callback, () => compute(...args));
            };
        let registeredContentScripts = [];
        const notSupported =
            name =>
            (...args) =>
                settle(popCallback(args), () => {
                    throw new Error(name + " is not supported");
                });
        chrome.scripting = {
            registerContentScripts: localFunction(scripts => {
                registeredContentScripts = registeredContentScripts.concat(scripts || []);
            }),
            updateContentScripts: localFunction(scripts => {
                for (const script of scripts || []) {
                    const index = registeredContentScripts.findIndex(existing => existing.id === script.id);
                    if (index !== -1)
                        registeredContentScripts[index] = { ...registeredContentScripts[index], ...script };
                }
            }),
            getRegisteredContentScripts: localFunction((filter = {}) =>
                Array.isArray(filter.ids)
                    ? registeredContentScripts.filter(script => filter.ids.includes(script.id))
                    : registeredContentScripts.slice()
            ),
            unregisterContentScripts: localFunction((filter = {}) => {
                registeredContentScripts = Array.isArray(filter.ids)
                    ? registeredContentScripts.filter(script => !filter.ids.includes(script.id))
                    : [];
            }),
            executeScript: notSupported("scripting.executeScript"),
            insertCSS: notSupported("scripting.insertCSS"),
            removeCSS: notSupported("scripting.removeCSS"),
        };

        // alarms

        const [onAlarm, dispatchAlarm] = createEvent();
        const alarms = new Map();
        const clearAlarm = name => {
            const alarm = alarms.get(name);
            if (!alarm) return false;
            clearTimeout(alarm.timer);
            alarms.delete(name);
            return true;
        };
        const describe = ({ name, scheduledTime, periodInMinutes }) => ({ name, scheduledTime, periodInMinutes });
        chrome.alarms = {
            create(...args) {
                const callback = popCallback(args);
                const name = typeof args[0] === "string" ? args.shift() : "";
                const info = args[0] || {};
                return settle(callback, () => {
                    clearAlarm(name);
                    const delay =
                        info.when !== undefined
                            ? info.when - Date.now()
                            : (info.delayInMinutes ?? info.periodInMinutes ?? 0) * 60000;
                    const alarm = {
                        name,
                        periodInMinutes: info.periodInMinutes,
                        scheduledTime: Date.now() + Math.max(delay, 0),
                    };
                    const fire = () => {
                        if (alarm.periodInMinutes) {
                            alarm.scheduledTime = Date.now() + alarm.periodInMinutes * 60000;
                            alarm.timer = setTimeout(fire, alarm.periodInMinutes * 60000);
                        } else {
                            alarms.delete(name);
                        }
                        dispatchAlarm(describe(alarm));
                    };
                    alarm.timer = setTimeout(fire, Math.max(delay, 0));
                    alarms.set(name, alarm);
                });
            },
            get: (name, callback) =>
                settle(callback, () => (alarms.has(name) ? describe(alarms.get(name)) : undefined)),
            getAll: callback => settle(callback, () => Array.from(alarms.values()).map(describe)),
            clear: (name, callback) => settle(callback, () => clearAlarm(name ?? "")),
            clearAll: callback =>
                settle(callback, () => {
                    for (const name of Array.from(alarms.keys())) clearAlarm(name);
                    return true;
                }),
            onAlarm,
        };

        // The toolbar button shows no badge, title or icon of the extension's choosing yet.
        const action = {
            setIcon: noop(),
            setTitle: noop(),
            getTitle: noop(manifest.name),
            setBadgeText: noop(),
            getBadgeText: noop(""),
            setBadgeBackgroundColor: noop(),
            setBadgeTextColor: noop(),
            setPopup: noop(),
            enable: noop(),
            disable: noop(),
        };
        [action.onClicked, browserEvents["action.onClicked"]] = createEvent();
        chrome.action = action;
        chrome.browserAction = action;
        chrome.commands = { getAll: noop([]), onCommand: createEvent()[0] };

        // A popup is as large as its page would like to be, as in Chrome. Other pages say so for nothing.
        let reportedSize = "";
        const reportPreferredSize = () => {
            const root = document.documentElement;
            if (!root) return;
            const previousWidth = root.style.width;
            root.style.width = "max-content";
            const rect = root.getBoundingClientRect();
            root.style.width = previousWidth;
            const size = Math.ceil(rect.width) + "x" + Math.ceil(rect.height);
            if (size === reportedSize) return;
            reportedSize = size;
            callBrowser("popup.setPreferredSize", Math.ceil(rect.width), Math.ceil(rect.height)).catch(() => {});
        };
        addEventListener("load", () => {
            reportPreferredSize();
            if (document.body) new ResizeObserver(reportPreferredSize).observe(document.body);
        });
        chrome.contextMenus = {
            create: () => 0,
            update: noop(),
            remove: noop(),
            removeAll: noop(),
            onClicked: createEvent()[0],
        };
    }

    function dispatch(type, ...args) {
        if (type === "event") {
            const [name, argumentsJSON] = args;
            if (browserEvents[name]) browserEvents[name](...JSON.parse(argumentsJSON));
            return;
        }

        if (type === "storage") {
            const [areaName, changesJSON] = args;
            const changes = JSON.parse(changesJSON);
            dispatchStorageChanged(changes, areaName);
            if (storageAreaDispatchers[areaName]) storageAreaDispatchers[areaName](changes);
            return;
        }

        if (type === "result") {
            const [callId, succeeded, resultJSON] = args;
            const call = pendingCalls.get(callId);
            if (!call) return;
            pendingCalls.delete(callId);
            const result = resultJSON === "" ? undefined : JSON.parse(resultJSON);
            if (succeeded) call.resolve(result);
            else call.reject(new Error(String(result)));
            return;
        }

        if (type === "message") {
            // Gives back how the listeners answered: "r" and the response, "async" if one will respond later, or "none".
            const [deliveryId, messageJSON, senderJSON] = args;
            const message = JSON.parse(messageJSON);
            const sender = JSON.parse(senderJSON);
            let response;
            let responded = false;
            let willRespond = false;
            const sendResponse = value => {
                if (responded) return;
                responded = true;
                const serialized = value === undefined ? undefined : JSON.stringify(value);
                if (willRespond) native.respond(deliveryId, serialized ?? "");
                else response = serialized;
            };
            for (const listener of messageListeners.slice()) {
                let result;
                try {
                    result = listener(message, sender, sendResponse);
                } catch (error) {
                    reportError(error);
                    continue;
                }
                if (result === true) willRespond = true;
                else if (result && typeof result.then === "function") {
                    willRespond = true;
                    result.then(sendResponse, error => sendResponse({ error: String(error) }));
                }
            }
            if (responded) return "r" + (response ?? "");
            if (willRespond) return "async";
            return messageListeners.length ? "none" : "nolistener";
        }
    }

    return [chrome, dispatch];
});
