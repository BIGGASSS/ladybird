// The chrome.* API that content scripts see. This evaluates to a function, which the engine calls in the extension's
// isolated world, and which returns the API object and a function the engine calls when the extension's storage changes.
(function (native, extensionId, manifestJSON, messagesJSON, uiLanguage) {
    "use strict";

    const messages = JSON.parse(messagesJSON);
    const baseURL = "chrome-extension://" + extensionId + "/";
    const noReceiverError = "Could not establish connection. Receiving end does not exist.";

    let lastError;

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
        return [event, dispatch];
    }

    // Settles an API call the way Chrome does: with a callback, if there is one, that runs with runtime.lastError set
    // to any error; otherwise with a promise.
    function settle(callback, compute) {
        let result;
        let error;
        try {
            result = compute();
        } catch (thrown) {
            error = thrown instanceof Error ? thrown : new Error(String(thrown));
        }

        if (typeof callback !== "function") return error ? Promise.reject(error) : Promise.resolve(result);

        Promise.resolve().then(() => {
            lastError = error ? { message: error.message } : undefined;
            try {
                if (error) callback();
                else callback(result);
            } catch (thrown) {
                reportError(thrown);
            } finally {
                lastError = undefined;
            }
        });
        return undefined;
    }

    function popCallback(args) {
        return typeof args[args.length - 1] === "function" ? args.pop() : undefined;
    }

    // runtime

    const [onMessage] = createEvent();
    const [onConnect] = createEvent();

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

    const runtime = {
        id: extensionId,
        getManifest() {
            return JSON.parse(manifestJSON);
        },
        getURL(path) {
            return baseURL + String(path).replace(/^\/+/, "");
        },
        // There are no background pages or service workers, so nothing ever receives a message.
        sendMessage(...args) {
            const callback = popCallback(args);
            return settle(callback, () => {
                throw new Error(noReceiverError);
            });
        },
        connect(...args) {
            const info = args.find(arg => arg && typeof arg === "object");
            return createDisconnectedPort(info && info.name !== undefined ? String(info.name) : "");
        },
        onMessage,
        onConnect,
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

    function createStorageArea(areaName, quotaBytes) {
        const read = () => JSON.parse(native.storageGet(extensionId, areaName));
        const write = changes => native.storageApply(extensionId, areaName, JSON.stringify(changes));
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

    function dispatchStorageChange(areaName, changesJSON) {
        const changes = JSON.parse(changesJSON);
        dispatchStorageChanged(changes, areaName);
        if (storageAreaDispatchers[areaName]) storageAreaDispatchers[areaName](changes);
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

    return [chrome, dispatchStorageChange];
});
