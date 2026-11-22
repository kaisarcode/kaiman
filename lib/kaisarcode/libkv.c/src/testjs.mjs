/**
 * testjs.mjs - Combined WebAssembly contract runner.
 * Summary: Runs C and JavaScript contracts with one case index and summary.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

import assert from "node:assert/strict";
import {spawnSync} from "node:child_process";
import {kv} from "../bin/wasm/wasm32/libkv.js";
import {readFileSync as readManifestVersionFile} from "node:fs";
const KC_MANIFEST_VERSION = JSON.parse(
    readManifestVersionFile(new URL("../manifest.json", import.meta.url), "utf8")
).version;

/**
 * Run the compiled C contracts and retain their individual case results.
 * @param script Node-compatible compiled C test executable.
 * @return Array of reported C test cases.
 */
function nativeCases(script) {
    const result = spawnSync(process.execPath, [script, "all"], {
        encoding: "utf8"
    });
    const lines = (result.stdout || "").trim().split(/\r?\n/);
    const entries = [];
    let summary = null;

    for (const line of lines) {
        const match = /^\[(\d+)\/(\d+)\] \[(PASS|FAIL)\] ([^:]+): (.+)$/.exec(line);
        if (match) {
            const [, current, total, state, name, detail] = match;
            if (Number(current) !== entries.length + 1 || Number(total) < 1) {
                throw new Error("Invalid C case numbering: " + line);
            }
            entries.push({name, detail, passed: state === "PASS", total: Number(total)});
        } else {
            const count = /^(\d+) passed, (\d+) failed$/.exec(line);
            if (count) summary = [Number(count[1]), Number(count[2])];
            else if (line) throw new Error("Unexpected C test output: " + line);
        }
    }
    const failures = entries.filter(entry => !entry.passed).length;
    if (result.error) throw result.error;
    if (entries.length === 0 || summary === null ||
        entries.some(entry => entry.total !== entries.length) ||
        summary[0] !== entries.length - failures || summary[1] !== failures ||
        (result.status === 0) !== (failures === 0)) {
        throw new Error("C test runner failed: " + (result.stderr || result.stdout || result.status));
    }
    return entries;
}

const javascriptCases = [
    {
        name: "javascript_open",
        detail: "opens an empty collection with a valid version",

        /**
         * Verifies opening an empty collection.
         * @return void
         */
        run() {
            const store = kv.open();
            try {
                assert.equal(store.count(), 0);
                assert.equal(store.get("missing"), null);
                assert.deepEqual(store.entries(), []);
                assert.equal(kv.version(), KC_MANIFEST_VERSION);
            } finally { store.close(); }
        }
    },
    {
        name: "javascript_binary",
        detail: "copies binary inputs and outputs without shared ownership",

        /**
         * Verifies binary values remain independently owned.
         * @return void
         */
        run() {
            const store = kv.open();
            try {
                const original = Uint8Array.of(0, 255, 7);
                store.set("binary", original);
                original[1] = 1;
                assert.deepEqual(store.get("binary"), Uint8Array.of(0, 255, 7));
                const output = store.get("binary");
                output[0] = 42;
                assert.deepEqual(store.get("binary"), Uint8Array.of(0, 255, 7));
            } finally { store.close(); }
        }
    },
    {
        name: "javascript_crud",
        detail: "replaces and deletes entries while distinguishing empty values",

        /**
         * Verifies entry replacement and deletion.
         * @return void
         */
        run() {
            const store = kv.open();
            try {
                store.set("item", Uint8Array.of(1));
                store.set("item", Uint8Array.of(2, 3));
                store.set("empty", new Uint8Array());
                assert.equal(store.count(), 2);
                assert.deepEqual(store.get("item"), Uint8Array.of(2, 3));
                assert.deepEqual(store.get("empty"), new Uint8Array());
                assert.equal(store.get("missing"), null);
                assert.equal(store.del("item"), true);
                assert.equal(store.del("item"), false);
                assert.equal(store.count(), 1);
            } finally { store.close(); }
        }
    },
    {
        name: "javascript_ordering",
        detail: "enumerates sorted keys and copied values",

        /**
         * Verifies sorted entry enumeration.
         * @return void
         */
        run() {
            const store = kv.open();
            try {
                for (const key of ["z", "aa", "A", "a"])
                    store.set(key, new TextEncoder().encode(key));
                const entries = store.entries();
                assert.deepEqual(entries.map(entry => entry[0]), ["A", "a", "aa", "z"]);
                entries[0][1][0] = 0;
                assert.deepEqual(store.get("A"), new TextEncoder().encode("A"));
            } finally { store.close(); }
        }
    },
    {
        name: "javascript_snapshot",
        detail: "round-trips portable binary data without sharing buffers",

        /**
         * Verifies portable snapshot round trips.
         * @return void
         */
        run() {
            const store = kv.open();
            let restored;
            try {
                store.set("a", Uint8Array.of(0, 255, 0));
                store.set("b", new Uint8Array());
                const binary = store.export();
                restored = kv.open(binary);
                assert.deepEqual(restored.get("a"), Uint8Array.of(0, 255, 0));
                assert.deepEqual(restored.get("b"), new Uint8Array());
                assert.deepEqual(restored.export(), binary);
                binary.fill(0);
                assert.deepEqual(restored.get("a"), Uint8Array.of(0, 255, 0));
                assert.equal(restored.count(), 2);
            } finally {
                restored?.close();
                store.close();
            }
        }
    },
    {
        name: "javascript_contract",
        detail: "rejects malformed snapshots and invalid JS arguments",

        /**
         * Verifies invalid JavaScript inputs are rejected.
         * @return void
         */
        run() {
            const store = kv.open();
            try {
                assert.throws(() => store.set("", Uint8Array.of(1)), TypeError);
                assert.throws(() => store.set("bad\0key", Uint8Array.of(1)), TypeError);
                assert.throws(() => store.set("ok", "string"), TypeError);
                assert.throws(() => store.get(42), TypeError);
                assert.throws(() => kv.open("string"), TypeError);
                store.set("key", Uint8Array.of(1));
                const binary = store.export();
                assert.throws(() => kv.open(binary.slice(0, -1)), /Invalid binary value/);
                assert.equal(store.count(), 1);
                assert.throws(() => store.del(""), TypeError);
            } finally { store.close(); }
            assert.throws(() => store.count(), /closed/);
            assert.throws(() => store.get("key"), /closed/);
            store.close();
        }
    }
];

/**
 * Print one unified C and JavaScript WASM test report.
 * @return None; exit status indicates whether any case failed.
 */
function main() {
    const script = process.argv[2];
    let entries;

    if (!script) throw new Error("Usage: node src/testjs.mjs path/to/kv_contract_test.js");
    entries = nativeCases(script);
    for (const item of javascriptCases) {
        let passed = true;

        try { item.run(); }
        catch (error) {
            passed = false;
            console.error("[FAIL] " + item.name + ": " + String(error));
        }
        entries.push({name: item.name, detail: item.detail, passed});
    }
    const total = entries.length;
    const failures = entries.filter(entry => !entry.passed).length;

    for (const [i, item] of entries.entries()) {
        console.log(`[${i + 1}/${total}] [${item.passed ? "PASS" : "FAIL"}] ${item.name}: ${item.detail}`);
    }
    console.log(`${total - failures} passed, ${failures} failed`);
    if (failures) process.exitCode = 1;
}

main();
