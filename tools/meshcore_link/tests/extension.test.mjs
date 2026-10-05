import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import vm from "node:vm";

test("extension owns one attached listener, exposes tools and bounds idle notifications", async () => {
    const tools = [];
    const prompts = [];
    const logs = [];
    const events = new Map();
    const lines = new EventEmitter();
    const child = new EventEmitter();
    child.stdout = {};
    child.stderr = { resume() {} };
    let ended = false;
    child.stdin = { end() { ended = true; } };
    child.kill = () => {};
    let spawns = 0;
    let acknowledged = 0;
    let received = 1;
    const operations = [];
    const hooks = new Map();
    const process = {
        env: { MESHCORE_LINK_LAUNCHER: "/private/launcher.json" },
        getuid: () => 1000,
        on: (kind, callback) => hooks.set(kind, callback),
    };
    const session = {
        send: async options => { prompts.push(options); },
        log: async (...args) => { logs.push(args); },
        on: (kind, callback) => events.set(kind, callback),
    };
    const context = vm.createContext({ process, Buffer, setTimeout, clearTimeout });
    const modules = {
        "@github/copilot-sdk/extension": {
            joinSession: async options => { tools.push(...options.tools); return session; },
        },
        "node:child_process": {
            spawn: (...args) => {
                spawns++;
                assert.deepEqual(Array.from(args[1]).slice(-1), ["--attached"]);
                assert.equal(args[2].stdio.join(","), "pipe,pipe,pipe");
                return child;
            },
        },
        "node:net": {
            createConnection: () => {
                const connection = new EventEmitter();
                connection.setTimeout = () => {};
                connection.destroy = error => connection.emit("error", error);
                connection.end = line => {
                    const request = JSON.parse(line);
                    operations.push(request);
                    let result;
                    if (request.operation === "notifications") {
                        result = { messages: received > acknowledged
                            ? [{ text: "Untrusted payload", feed: "Example-Base" }] : [],
                        cursor: received };
                    } else if (request.operation === "advance") {
                        acknowledged = request.args.cursor;
                        result = {};
                    } else result = { operation: request.operation };
                    queueMicrotask(() => {
                        connection.emit("data", Buffer.from(JSON.stringify({ ok: true, result })));
                        connection.emit("end");
                    });
                };
                queueMicrotask(() => connection.emit("connect"));
                return connection;
            },
        },
        "node:readline": { createInterface: () => lines },
        "node:fs": {
            statSync: () => ({ isFile: () => true, mode: 0o600, uid: 1000 }),
            readFileSync: () => JSON.stringify({
                root: "/project", python: "/python", config: "/private/config", state: "/private/state",
            }),
        },
        "node:path": { resolve: (...parts) => parts.join("/") },
    };
    const extension = new vm.SourceTextModule(
        readFileSync(new URL("../extension.mjs", import.meta.url), "utf8"), { context });
    await extension.link(async specifier => {
        if (specifier === "./notifications.mjs") {
            const module = new vm.SourceTextModule(
                readFileSync(new URL("../notifications.mjs", import.meta.url), "utf8"), { context });
            await module.link(() => { throw Error("Unexpected import"); });
            return module;
        }
        const exports = modules[specifier];
        assert.ok(exports, specifier);
        return new vm.SyntheticModule(Object.keys(exports), function () {
            for (const [key, value] of Object.entries(exports)) this.setExport(key, value);
        }, { context });
    });
    await extension.evaluate();
    assert.equal(spawns, 1);
    assert.deepEqual(tools.map(t => t.name),
        ["meshcore_send", "meshcore_inbox", "meshcore_link_status"]);
    lines.emit("line", JSON.stringify({ event: "ready" }));
    for (let i = 0; i < 10; i++) await new Promise(setImmediate);
    assert.equal(prompts.length, 1);
    assert.equal(prompts[0].mode, "enqueue");
    assert.equal(prompts[0].source, undefined);
    assert.match(prompts[0].prompt, /untrusted radio data/);
    assert.equal(acknowledged, 1);
    received++;
    for (let i = 0; i < 100; i++) lines.emit("line", '{"event":"inbox"}');
    for (let i = 0; i < 10; i++) await new Promise(setImmediate);
    assert.equal(prompts.length, 1);
    events.get("session.idle")();
    for (let i = 0; i < 10; i++) await new Promise(setImmediate);
    assert.equal(prompts.length, 2);
    assert.equal(acknowledged, 2);
    const send = await tools[0].handler({ text: "One directed message" });
    assert.equal(JSON.parse(send).operation, "send");
    assert.equal(operations.filter(r => r.operation === "send").length, 1);
    assert.equal(JSON.parse(await tools[2].handler({})).operation, "status");
    hooks.get("SIGINT")();
    assert.equal(ended, true);
});
