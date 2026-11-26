/**
 * libkaiman.js - WebAssembly JavaScript projection for kaiman.
 * Summary: Intention-level service management over the public kaiman API.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

import createModule from "./libkaiman.mjs";

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
 * @param value String to copy.
 * @return Native pointer.
 */
function cstring(value) {
    if (typeof value !== "string" || value.includes("\0")) {
        throw new TypeError("Expected string without NUL");
    }
    const size = wasm.lengthBytesUTF8(value) + 1;
    const ptr = allocate(size);
    wasm.stringToUTF8(value, ptr, size);
    return ptr;
}

/**
 * Read one borrowed native string.
 * @param ptr Native string pointer, or zero.
 * @return JavaScript string, or null.
 */
function text(ptr) {
    return ptr ? wasm.UTF8ToString(ptr) : null;
}

/**
 * Check a native status and throw on failure.
 * @param status Native status.
 * @return None.
 */
function check(status) {
    if (status === 0) return;
    throw new Error(text(wasm._kc_kaiman_strerror(status)) ||
        "Native error " + status);
}

/**
 * Wrap one native service handle with intention-level operations.
 * @param initial Native service handle.
 * @param owner Owning manager state.
 * @return Service operations.
 */
function service(initial, owner) {
    let handle = initial;
    let alive = true;

    /**
     * Return the active native handle or fail when removed.
     * @return Active native handle.
     */
    const valid = () => {
        if (!alive || owner.closed) throw new Error("Service is no longer managed");
        return handle;
    };

    return {

        /**
         * Set one field of this service.
         * @param key Field name.
         * @param value New value.
         * @return None.
         */
        set(key, value) {
            const name = cstring(key);
            const data = cstring(String(value));
            try { check(wasm._kc_kaiman_service_set(valid(), name, data)); }
            finally { wasm._free(data); wasm._free(name); }
        },

        /**
         * Read one field of this service.
         * @param key Field name.
         * @return Field value or null.
         */
        get(key) {
            const name = cstring(key);
            try { return text(wasm._kc_kaiman_service_get(valid(), name)); }
            finally { wasm._free(name); }
        },

        /**
         * Return the configured timeout.
         * @return Timeout in seconds.
         */
        timeout() { return wasm._kc_kaiman_service_timeout(valid()); },

        /**
         * Remove this service and invalidate its handle.
         * @return None.
         */
        remove() {
            check(wasm._kc_kaiman_service_remove(valid()));
            alive = false;
            owner.forget(handle);
        }
    };
}

/**
 * Wrap one native manager handle with intention-level operations.
 * @param initial Native manager handle.
 * @return Manager operations.
 */
function manager(initial) {
    let handle = initial;
    const wrappers = new Map();
    const owner = {
        closed: false,

        /**
         * Forget a removed service wrapper.
         * @param ptr Native service handle.
         * @return None.
         */
        forget(ptr) {
            for (const [key, value] of wrappers) {
                if (value.native === ptr) wrappers.delete(key);
            }
        }
    };

    /**
     * Return the active native handle or fail when closed.
     * @return Active native handle.
     */
    const valid = () => {
        if (owner.closed) throw new Error("Manager is closed");
        return handle;
    };

    /**
     * Wrap a native handle, preserving object identity.
     * @param ptr Native service handle.
     * @return Service operations, or null.
     */
    const wrap = ptr => {
        if (!ptr) return null;
        if (!wrappers.has(ptr)) {
            const wrapped = service(ptr, owner);
            wrapped.native = ptr;
            wrappers.set(ptr, wrapped);
        }
        return wrappers.get(ptr);
    };

    return {

        /**
         * Return all managed services in configuration order.
         * @return Array of service objects.
         */
        getServices() {
            const out = allocate(4);
            const count = allocate(4);
            try {
                check(wasm._kc_kaiman_get_services(valid(), out, count));
                const array = uint(out);
                return Array.from({length: uint(count)}, (_, index) =>
                    wrap(wasm.HEAPU32[(array >>> 2) + index]));
            } finally {
                wasm._free(count);
                wasm._free(out);
            }
        },

        /**
         * Return one service by name, or null when absent.
         * @param name Service name.
         * @return Service object or null.
         */
        getService(name) {
            const key = cstring(name);
            const out = allocate(4);
            try {
                const status = wasm._kc_kaiman_get_service(valid(), key, out);
                if (status === 1) return null;
                check(status);
                return wrap(uint(out));
            } finally {
                wasm._free(out);
                wasm._free(key);
            }
        },

        /**
         * Create one service from a plain descriptor.
         * @param descriptor Service fields.
         * @return New service object.
         */
        addService(descriptor) {
            if (descriptor === null || typeof descriptor !== "object") {
                throw new TypeError("Expected service descriptor");
            }
            const fields = [
                descriptor.name, descriptor.adapter, descriptor.url,
                descriptor.apiKeyEnv,
                descriptor.tokenCommand, descriptor.authScheme,
                descriptor.authUser
            ];
            const pointers = fields.map(value =>
                value === undefined || value === null ? 0 : cstring(value));
            const data = allocate(32);
            const out = allocate(4);
            try {
                pointers.forEach((ptr, index) =>
                    wasm.setValue(data + (index * 4), ptr, "i32"));
                wasm.setValue(data + 28,
                    descriptor.timeout === undefined ? 0 : descriptor.timeout,
                    "i32");
                check(wasm._kc_kaiman_add_service(valid(), data, out));
                return wrap(uint(out));
            } finally {
                pointers.forEach(ptr => { if (ptr) wasm._free(ptr); });
                wasm._free(out);
                wasm._free(data);
            }
        },

        /**
         * Read or set the gateway listen address.
         * @param address Optional new host and port.
         * @return Current address when no value is supplied.
         */
        listen(address) {
            if (address === undefined) {
                return text(wasm._kc_kaiman_address(valid()));
            }
            const value = cstring(String(address));
            try { check(wasm._kc_kaiman_listen(valid(), value)); }
            finally { wasm._free(value); }
        },

        /**
         * Reload the manager from its saved configuration.
         * @return None.
         */
        load() {
            check(wasm._kc_kaiman_load(valid()));
            wrappers.clear();
        },

        /**
         * Return the resolved configuration path.
         * @return Path string.
         */
        path() { return text(wasm._kc_kaiman_path(valid())); },

        /**
         * Return the most recent detailed error message.
         * @return Error string or null.
         */
        error() { return text(wasm._kc_kaiman_error(valid())); },

        /**
         * Close the manager and invalidate its services.
         * @return None.
         */
        close() {
            if (!owner.closed) {
                wasm._kc_kaiman_close(handle);
                owner.closed = true;
                wrappers.clear();
            }
        }
    };
}

export const kaiman = {

    /**
     * Open a service manager.
     * @param options Optional path and listen values.
     * @return Manager operations.
     */
    open(options) {
        const data = allocate(8);
        const out = allocate(4);
        let path = 0;
        let listen = 0;
        try {
            if (options !== undefined && options.path !== undefined) {
                path = cstring(String(options.path));
            }
            if (options !== undefined && options.listen !== undefined) {
                listen = cstring(String(options.listen));
            }
            wasm.setValue(data, path, "i32");
            wasm.setValue(data + 4, listen, "i32");
            check(wasm._kc_kaiman_open(out, data));
            return manager(uint(out));
        } finally {
            if (listen) wasm._free(listen);
            if (path) wasm._free(path);
            wasm._free(out);
            wasm._free(data);
        }
    },

    /**
     * Return the native library version.
     * @return Semantic version string.
     */
    version() { return text(wasm._kc_kaiman_version()); }
};

export default kaiman;
