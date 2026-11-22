/**
 * libkv.js - WebAssembly JavaScript projection.
 * Summary: Binary key-value collection with storage-independent snapshots.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

import createModule from "./libkv.mjs";

const wasm = await createModule();

/**
 * Read a 32-bit native pointer or length.
 * @param ptr Native address containing a 32-bit value.
 * @return Unsigned integer.
 */
function uint(ptr) {
    return wasm.getValue(ptr, "i32") >>> 0;
}

/**
 * Allocate one temporary native buffer.
 * @param size Buffer size.
 * @return Native pointer.
 */
function allocate(size) {
    const ptr = wasm._malloc(Math.max(1, size));
    if (!ptr) throw new Error("Out of WebAssembly memory");
    return ptr;
}

/**
 * Copy one JavaScript string into native memory.
 * @param value Key string.
 * @return Native pointer.
 */
function cstring(value) {
    if (typeof value !== "string" || value.length === 0 || value.includes("\0")) {
        throw new TypeError("Expected nonempty string key without NUL");
    }
    const size = wasm.lengthBytesUTF8(value) + 1;
    const ptr = allocate(size);
    wasm.stringToUTF8(value, ptr, size);
    return ptr;
}

/**
 * Check the native return status.
 * @param status Native status.
 * @return None.
 */
function check(status) {
    if (status !== 0) throw new Error(wasm.UTF8ToString(wasm._kc_kv_strerror(status)));
}

/**
 * Copy one Uint8Array into native memory.
 * @param value Input bytes.
 * @return Temporary native pointer.
 */
function copy(value) {
    if (!(value instanceof Uint8Array)) throw new TypeError("Expected Uint8Array");
    const ptr = allocate(value.length);
    wasm.HEAPU8.set(value, ptr);
    return ptr;
}

/**
 * Construct the public JavaScript API for an owned native handle.
 * @param initial Native collection handle.
 * @return Collection operations.
 */
function collection(initial) {
    let handle = initial;

    /**
     * Return the active native handle.
     * @return Active native handle.
     */
    const valid = () => {
        if (!handle) throw new Error("Collection is closed");
        return handle;
    };
    return {

        /**
         * Insert or replace binary data.
         * @param key Record key.
         * @param value Binary value.
         * @return None.
         */
        set(key, value) {
            const name = cstring(key);
            let data = 0;
            try {
                data = copy(value);
                check(wasm._kc_kv_set(valid(), name, data, value.length));
            } finally {
                if (data) wasm._free(data);
                wasm._free(name);
            }
        },

        /**
         * Return a copied value or null for a missing key.
         * @param key Record key.
         * @return Uint8Array or null.
         */
        get(key) {
            const name = cstring(key);
            const out = allocate(4), size = allocate(4);
            try {
                const status = wasm._kc_kv_get(valid(), name, out, size);
                if (status === 1) return null;
                check(status);
                return wasm.HEAPU8.slice(uint(out), uint(out) + uint(size));
            } finally {
                wasm._free(size);
                wasm._free(out);
                wasm._free(name);
            }
        },

        /**
         * Delete one record.
         * @param key Record key.
         * @return True if the record existed.
         */
        del(key) {
            const name = cstring(key);
            try {
                const status = wasm._kc_kv_del(valid(), name);
                if (status === 1) return false;
                check(status);
                return true;
            } finally { wasm._free(name); }
        },

        /**
         * Count records in the collection.
         * @return Number of records.
         */
        count() { return wasm._kc_kv_count(valid()); },

        /**
         * Copy all records in ascending key order.
         * @return Array of key and Uint8Array pairs.
         */
        entries() {
            const key = allocate(4), value = allocate(4), size = allocate(4);
            try {
                return Array.from({length: wasm._kc_kv_count(valid())}, (_, i) => {
                    check(wasm._kc_kv_at(valid(), i, key, value, size));
                    const ptr = uint(value);
                    return [wasm.UTF8ToString(uint(key)), wasm.HEAPU8.slice(ptr, ptr + uint(size))];
                });
            } finally {
                wasm._free(size);
                wasm._free(value);
                wasm._free(key);
            }
        },

        /**
         * Export a portable in-memory binary snapshot.
         * @return Uint8Array containing the complete collection.
         */
        export() {
            const out = allocate(4), size = allocate(4);
            try {
                check(wasm._kc_kv_export(valid(), out, size));
                const ptr = uint(out);
                return wasm.HEAPU8.slice(ptr, ptr + uint(size));
            } finally {
                wasm._free(size);
                wasm._free(out);
            }
        },

        /**
         * Release the collection.
         * @return None.
         */
        close() {
            if (handle) { wasm._kc_kv_close(handle); handle = 0; }
        }
    };
}

export const kv = {

    /**
     * Open an empty collection or restore one from binary bytes.
     * @param bytes Optional serialized collection.
     * @return Collection handle.
     */
    open(bytes) {
        if (bytes !== undefined && !(bytes instanceof Uint8Array))
            throw new TypeError("Expected Uint8Array");
        const out = allocate(4);
        const options = bytes === undefined ? 0 : allocate(8);
        let data = 0;
        try {
            if (options) {
                data = copy(bytes);
                wasm.setValue(options, data, "i32");
                wasm.setValue(options + 4, bytes.length, "i32");
            }
            check(wasm._kc_kv_open(out, options));
            return collection(uint(out));
        } finally {
            if (data) wasm._free(data);
            if (options) wasm._free(options);
            wasm._free(out);
        }
    },

    /**
     * Return the generated build version.
     * @return Semantic version string.
     */
    version() { return wasm.UTF8ToString(wasm._kc_kv_version()); }
};

export default kv;
