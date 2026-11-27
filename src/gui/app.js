/**
 * app.js - Browser frontend for kaiman.
 * Summary: Manages services and the gateway through NativeBridge.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

var currentState = { services: [] };
var themePending = false;
var editedAuthentication = null;
var slugTouched = false;
var connectionRevision = 0;
var serviceChecks = Object.create(null);

/**
 * Find one element by its document identifier.
 * @param id Element identifier.
 * @return The element, or null.
 */
function byId(id) {
    return document.getElementById(id);
}

/**
 * Call one exposed NativeBridge method.
 * @param method Method name.
 * @param params Parameters to pass.
 * @return A promise resolving to the bridge result.
 */
async function bridge(method, params) {
    if (!window.NativeBridge ||
        typeof window.NativeBridge[method] !== "function") {
        throw new Error("NativeBridge method is unavailable: " + method);
    }
    return await window.NativeBridge[method](params);
}

/**
 * Describe one bridge failure.
 * @param error Rejected value.
 * @return Human readable error text.
 */
function describeError(error) {
    if (error && typeof error.message === "string" && error.message !== "") {
        return error.message;
    }
    return String(error);
}

/**
 * Present one status message.
 * @param text Message text.
 * @param kind Status kind.
 * @return void
 */
function showMessage(text, kind) {
    var status = byId("status");

    status.textContent = text;
    status.setAttribute("data-kind", kind);
}

/**
 * Assign one input value.
 * @param input Input element.
 * @param value Value to assign.
 * @return void
 */
function setValue(input, value) {
    input.value = value === null || value === undefined ? "" : String(value);
}

/**
 * Convert one display name into a stable slug.
 * @param text Display name.
 * @return Generated slug, or "service" when no characters are usable.
 */
function slugify(text) {
    var value = String(text === null || text === undefined ? "" : text)
        .toLowerCase().trim()
        .replace(/[^a-z0-9._-]+/g, "-")
        .replace(/^-+|-+$/g, "");

    return value === "" ? "service" : value;
}

/**
 * Resolve one unused slug against the loaded services.
 * @param slug Preferred slug.
 * @return Preferred slug, or the same base with a numeric suffix.
 */
function uniqueSlug(slug) {
    var services = currentState.services || [];
    var match = /^(.*)-(\d+)$/.exec(slug);
    var base = match !== null ? match[1] : slug;
    var number = match !== null ? parseInt(match[2], 10) + 1 : 2;
    var candidate = base + "-" + number;
    var used = Object.create(null);
    var index;

    for (index = 0; index < services.length; index++) {
        used[services[index].name] = true;
    }
    if (!used[slug]) return slug;
    while (used[candidate]) {
        number++;
        candidate = base + "-" + number;
    }
    return candidate;
}

/**
 * Preview the slug that an empty slug field will use.
 * @return void
 */
function refreshSlugPlaceholder() {
    var slug = byId("field-slug");
    var name = byId("field-display-name").value.trim();

    if (slugTouched || slug.readOnly || slug.value !== "") return;
    slug.placeholder = name === "" ? "" : uniqueSlug(slugify(name));
}

/**
 * Track explicit slug edits and refresh the empty-field preview.
 * @return void
 */
function handleSlugInput() {
    slugTouched = byId("field-slug").value.trim() !== "";
    refreshSlugPlaceholder();
}

/**
 * Enable or disable every button.
 * @param busy Whether the interface is busy.
 * @return void
 */
function setBusy(busy) {
    var buttons = document.querySelectorAll("button");
    var index;

    for (index = 0; index < buttons.length; index++) {
        buttons[index].disabled = busy;
    }
    if (typeof currentState.running !== "boolean") {
        byId("toggle-gateway").disabled = true;
    }
    renderApi(busy);
}

/**
 * Paint the saved health state without replacing the service item.
 * @param name Configured service slug.
 * @return void
 */
function renderServiceHealth(name) {
    var items = byId("service-list").querySelectorAll(".service-item");
    var check = serviceChecks[name];
    var result = check ? check.result : null;
    var label = "Not checked yet";
    var index;
    var dot;

    if (result) {
        label = result.online ? "Online: " + result.models +
            (result.models === 1 ? " model" : " models") + " available." :
            "Offline: " + (result.message || "Connection failed.");
    }
    for (index = 0; index < items.length; index++) {
        if (items[index].getAttribute("data-name") !== name) continue;
        dot = items[index].querySelector(".service-health");
        dot.setAttribute("data-online", result && result.online ?
            "true" : "false");
        dot.setAttribute("aria-label", label);
        dot.title = label;
    }
}

/**
 * Synchronize health checks with saved services and invalidate edited entries.
 * @param services Current saved services.
 * @return void
 */
function syncServiceHealth(services) {
    var present = Object.create(null);
    var index;
    var service;
    var check;
    var signature;
    var name;

    for (index = 0; index < services.length; index++) {
        service = services[index];
        name = service.name;
        present[name] = true;
        signature = JSON.stringify(service);
        check = serviceChecks[name];
        if (!check) {
            check = {
                signature: signature,
                revision: 0,
                started: 0,
                pending: false,
                polling: false,
                present: true,
                next: 0,
                result: null
            };
            serviceChecks[name] = check;
        } else if (!check.present || check.signature !== signature) {
            check.signature = signature;
            check.revision++;
            check.result = null;
            check.next = 0;
        }
        check.present = true;
    }
    for (name in serviceChecks) {
        check = serviceChecks[name];
        if (present[name]) continue;
        if (check.present) check.revision++;
        check.present = false;
        if (!check.pending && !check.polling) delete serviceChecks[name];
    }
}

/**
 * Start or collect one native service check without blocking other checks.
 * @param name Configured service slug.
 * @param check Mutable service health state.
 * @return A promise resolving after one polling step.
 */
async function pollServiceHealth(name, check) {
    var result;

    check.polling = true;
    if (!check.pending) check.started = check.revision;
    try {
        result = await bridge("serviceHealth", { name: name });
    } catch (error) {
        result = { online: false, message: describeError(error) };
    }
    check.pending = result.pending === true;
    check.polling = false;
    if (check.pending) return;
    if (!check.present) {
        delete serviceChecks[name];
        return;
    }
    if (check.started !== check.revision) {
        check.next = 0;
        return;
    }
    check.result = result;
    check.next = Date.now() + 30000;
    renderServiceHealth(name);
}

/**
 * Poll active checks and schedule independent checks every 30 seconds.
 * @return void
 */
function pollServices() {
    var now = Date.now();
    var name;
    var check;

    for (name in serviceChecks) {
        check = serviceChecks[name];
        if (!check.polling && (check.pending ||
            (check.present && check.next <= now))) {
            pollServiceHealth(name, check);
        }
    }
}

/**
 * Render the configured services.
 * @param services Services from the state response.
 * @return void
 */
function renderServices(services) {
    var list = byId("service-list");
    var index;
    var item;
    var name;
    var detail;
    var title;
    var dot;

    list.textContent = "";
    if (services.length === 0) {
        item = document.createElement("p");
        item.className = "service-empty";
        item.textContent = "No services configured.";
        list.appendChild(item);
        return;
    }
    for (index = 0; index < services.length; index++) {
        name = document.createElement("span");
        name.className = "service-name";
        name.textContent = services[index]["display-name"] ||
            services[index].name;
        detail = document.createElement("span");
        detail.className = "service-detail";
        detail.textContent = (services[index].name || "") + " · " +
            (services[index].adapter || "no provider");
        item = document.createElement("button");
        item.type = "button";
        item.className = "service-item";
        item.setAttribute("data-name", services[index].name);
        title = document.createElement("span");
        title.className = "service-title";
        dot = document.createElement("span");
        dot.className = "service-health";
        dot.setAttribute("role", "img");
        title.appendChild(name);
        title.appendChild(dot);
        item.appendChild(title);
        item.appendChild(detail);
        list.appendChild(item);
        renderServiceHealth(services[index].name);
    }
}

/**
 * Check whether the selected provider uses automatic local connection data.
 * @return True for a local provider.
 */
function localProvider() {
    var provider = byId("field-adapter").value;

    return provider === "opencode-local" || provider === "codex-local";
}

/**
 * Show only the fields used by the selected authentication method.
 * @return void
 */
function renderAuthentication() {
    var local = localProvider();
    var scheme = local ? "none" : byId("field-scheme").value;
    var authenticated = scheme === "bearer" || scheme === "basic";
    var basic = scheme === "basic";
    var credential = basic ? "Password" : "API key";
    var saved = editedAuthentication !== null &&
        editedAuthentication.scheme === scheme &&
        editedAuthentication.configured;
    var secret = byId("field-secret");

    byId("provider-url").hidden = local;
    byId("field-url").disabled = local;
    byId("auth-scheme").hidden = local;
    byId("field-scheme").disabled = local;
    byId("auth-user").hidden = !basic;
    byId("field-user").disabled = !basic;
    byId("field-user").required = basic;
    byId("auth-secret").hidden = !authenticated;
    secret.disabled = !authenticated;
    secret.required = authenticated && !saved;
    secret.placeholder = authenticated && saved ? "Credential configured" : "";
    byId("secret-label").textContent = credential;
}

/**
 * Show the selected provider's default URL without filling the input.
 * @return void
 */
function renderDefaultUrl() {
    var provider = byId("field-adapter").value;
    var defaults = currentState.defaults || {};

    byId("field-url").placeholder = defaults[provider] || "";
}

/**
 * Apply connection defaults when choosing an OpenCode provider.
 * @return void
 */
function handleProvider() {
    var provider = byId("field-adapter").value;

    renderDefaultUrl();
    if (localProvider()) {
        byId("field-scheme").value = "none";
        byId("field-url").value = "";
        byId("field-user").value = "";
    } else if (provider === "opencode-cloud") {
        byId("field-scheme").value = "bearer";
    } else if (provider === "opencode") {
        byId("field-scheme").value = "basic";
        byId("field-user").value = "opencode";
    } else {
        renderAuthentication();
        return;
    }
    byId("field-secret").value = "";
    editedAuthentication = null;
    renderAuthentication();
}

/**
 * Display the result of a connection check in the service editor.
 * @param text Status text, or empty to hide the result.
 * @param kind Status kind.
 * @return void
 */
function showConnection(text, kind) {
    var result = byId("connection-result");

    result.textContent = text;
    result.setAttribute("data-kind", kind);
    result.hidden = text === "";
}

/**
 * Invalidate the previous result when service fields change.
 * @return void
 */
function clearConnection() {
    connectionRevision++;
    showConnection("", "info");
}

/**
 * Check the current service fields without applying changes.
 * @return A promise resolving after the test completes.
 */
async function handleTestConnection() {
    var params;
    var result;
    var revision;

    if (!byId("service-form").reportValidity()) return;
    params = readForm();
    if (params === null) return;
    revision = ++connectionRevision;
    setBusy(true);
    showConnection("Testing connection...", "info");
    try {
        result = await bridge("testService", params);
        if (revision === connectionRevision) {
            showConnection("Connection successful. " + result.models +
                (result.models === 1 ? " model" : " models") +
                " available.", "success");
        }
    } catch (error) {
        if (revision === connectionRevision) {
            showConnection("Connection failed: " + describeError(error),
                "error");
        }
    } finally {
        params.secret = null;
        setBusy(false);
    }
}

/**
 * Reset the service form to its add state.
 * @return void
 */
function clearForm() {
    var form = byId("service-form");
    var items = document.querySelectorAll(".service-item");
    var index;

    clearConnection();
    form.reset();
    slugTouched = false;
    setValue(byId("field-display-name"), "");
    setValue(byId("field-slug"), "");
    byId("field-slug").placeholder = "";
    byId("field-adapter").value = "openai";
    byId("field-scheme").value = "none";
    editedAuthentication = null;
    renderDefaultUrl();
    renderAuthentication();
    byId("field-timeout").value = "60";
    byId("field-slug").readOnly = false;
    byId("editor-heading").textContent = "Add service";
    byId("remove-service").hidden = true;
    for (index = 0; index < items.length; index++) {
        items[index].removeAttribute("aria-current");
    }
}

/**
 * Load one service into the editor.
 * @param service Service to edit.
 * @return void
 */
function fillForm(service) {
    var fields = ["display-name", "adapter", "url", "scheme", "user"];
    var items = document.querySelectorAll(".service-item");
    var index;

    clearConnection();
    setValue(byId("field-slug"), service.name);
    for (index = 0; index < fields.length; index++) {
        setValue(byId("field-" + fields[index]), service[fields[index]]);
    }
    if (byId("field-display-name").value === "") {
        setValue(byId("field-display-name"), service.name);
    }
    if (!byId("field-scheme").value) {
        byId("field-scheme").value = "bearer";
    }
    editedAuthentication = {
        scheme: byId("field-scheme").value,
        configured: Boolean(service.keyring || service.env ||
            service["token-command"])
    };
    byId("field-secret").value = "";
    renderDefaultUrl();
    renderAuthentication();
    setValue(byId("field-timeout"), service.timeout);
    slugTouched = true;
    byId("field-slug").placeholder = "";
    byId("field-slug").readOnly = true;
    byId("editor-heading").textContent = "Edit " + service.name;
    byId("remove-service").hidden = false;
    for (index = 0; index < items.length; index++) {
        if (items[index].getAttribute("data-name") === service.name) {
            items[index].setAttribute("aria-current", "true");
        } else {
            items[index].removeAttribute("aria-current");
        }
    }
}

/**
 * Read bridge parameters from the service form.
 * @return Parameters, or null when the name is missing.
 */
function readForm() {
    var params = {};
    var timeout;

    params["display-name"] = byId("field-display-name").value.trim();
    if (params["display-name"] === "") {
        showMessage("A service name is required.", "error");
        return null;
    }
    if (byId("field-slug").readOnly) {
        params.name = byId("field-slug").value.trim();
    } else if (byId("field-slug").value.trim() === "") {
        params.name = uniqueSlug(slugify(params["display-name"]));
    } else {
        params.name = uniqueSlug(slugify(byId("field-slug").value));
    }
    params.adapter = byId("field-adapter").value;
    params.scheme = localProvider() ? "none" : byId("field-scheme").value;
    params.url = localProvider() ? "" : byId("field-url").value.trim();
    params.secret = byId("field-secret").disabled ||
        byId("field-secret").value === "" ? null : byId("field-secret").value;
    params.user = byId("field-user").disabled ? "" :
        byId("field-user").value;
    timeout = parseInt(byId("field-timeout").value, 10);
    params.timeout = isNaN(timeout) ? 0 : timeout;
    return params;
}

/**
 * Resolve a client URL from a configured listener address.
 * @param address Listener host and port.
 * @return HTTP origin, or empty when no usable URL can be formed.
 */
function apiOrigin(address) {
    var value = String(address || "").trim();
    var separator = value.lastIndexOf(":");
    var host = value.slice(0, separator);
    var port = value.slice(separator + 1);
    var parsed;
    var zone;

    if (separator < 1 || !/^\d+$/.test(port) ||
        Number(port) < 1 || Number(port) > 65535) return "";
    if (host.charAt(0) === "[" && host.slice(-1) === "]") {
        host = host.slice(1, -1);
    }
    if (!host || /[\s\/?#@\\\[\]]/.test(host)) return "";
    if (host.indexOf(":") !== -1) {
        zone = host.indexOf("%");
        if (zone !== -1) {
            if (!/^[a-zA-Z0-9_.-]+$/.test(host.slice(zone + 1))) return "";
            try {
                parsed = new URL("http://[" + host.slice(0, zone) + "]");
                return "http://" + parsed.hostname.slice(0, -1) +
                    "%25" + encodeURIComponent(host.slice(zone + 1)) +
                    "]:" + Number(port);
            } catch (error) {
                return "";
            }
        }
        host = "[" + host + "]";
    } else if (!/^[a-zA-Z0-9_.-]+$/.test(host)) {
        return "";
    }
    try {
        parsed = new URL("http://" + host);
    } catch (error) {
        return "";
    }
    host = parsed.hostname;
    if (host === "0.0.0.0") host = "127.0.0.1";
    if (host === "[::]") host = "[::1]";
    return "http://" + host + ":" + Number(port);
}

/**
 * Update the base URL and copy controls using the saved listener address.
 * @param busy Whether another interface operation is active.
 * @return void
 */
function renderApi(busy) {
    var origin = apiOrigin(currentState.address);
    var buttons = document.querySelectorAll(".api-copy");
    var index;

    byId("api-base-url").textContent = origin ? origin + "/v1" : "Unavailable";
    for (index = 0; index < buttons.length; index++) {
        buttons[index].disabled = busy || !origin;
    }
}

/**
 * Copy text on WebViews where the asynchronous clipboard API is unavailable.
 * @param text Text to copy.
 * @return True when the copy command succeeds.
 */
function copySelection(text) {
    var input = document.createElement("textarea");
    var focus = document.activeElement;
    var copied = false;
    var dialog = focus && focus.closest("dialog[open]");

    input.className = "clipboard-text";
    input.value = text;
    input.readOnly = true;
    (dialog || document.body).appendChild(input);
    try {
        input.select();
        copied = document.execCommand("copy");
    } finally {
        input.remove();
        if (focus) focus.focus();
    }
    return copied;
}

/**
 * Display feedback in the API dialog and the main status bar.
 * @param text Status message, or empty to clear it.
 * @param kind Status kind.
 * @return void
 */
function showApiMessage(text, kind) {
    var status = byId("api-status");

    status.textContent = text;
    status.setAttribute("data-kind", kind);
    status.hidden = text === "";
    if (text !== "") showMessage(text, kind);
}

/**
 * Open the API endpoint dialog.
 * @return void
 */
function handleApi() {
    showApiMessage("", "info");
    byId("api-dialog").showModal();
}

/**
 * Copy the full URL of the selected API endpoint.
 * @param event Button activation event.
 * @return A promise resolving after copying.
 */
async function handleApiCopy(event) {
    var origin = apiOrigin(currentState.address);
    var path = event.currentTarget.getAttribute("data-path");
    var url;

    if (!origin || !path) return;
    url = origin + path;
    try {
        if (!copySelection(url)) {
            if (!navigator.clipboard) throw new Error("Clipboard unavailable.");
            await navigator.clipboard.writeText(url);
        }
        showApiMessage("URL copied.", "success");
    } catch (error) {
        showApiMessage("Unable to copy URL: " + describeError(error),
            "error");
    }
}

/**
 * Open the HTTP API reference in the system browser.
 * @param event Link activation event.
 * @return A promise resolving after the request completes.
 */
async function handleApiDocumentation(event) {
    event.preventDefault();
    try {
        await bridge("openApiDocumentation", null);
    } catch (error) {
        showApiMessage(describeError(error), "error");
    }
}

/**
 * Reload manager and gateway state.
 * @return A promise resolving after the interface is updated.
 */
async function refresh() {
    var result = await bridge("state");
    var state = byId("gateway-state");
    var toggle = byId("toggle-gateway");

    currentState = result;
    renderDefaultUrl();
    toggle.textContent = result.running ? "Stop" : "Start";
    toggle.classList.toggle("button-primary", !result.running);
    toggle.classList.toggle("button-danger", result.running);
    syncServiceHealth(result.services || []);
    renderServices(result.services || []);
    pollServices();
    state.textContent = result.running ? "Running" : "Stopped";
    state.className = result.running ? "state state-running" :
        "state state-stopped";
    state.title = result.status || "";
    byId("gateway-address").textContent = result.address || "";
    byId("version").textContent = result.version || "";
    setValue(byId("listen"), result.address);
    renderApi(true);
}

/**
 * Run one bridge command and refresh the interface.
 * @param method Bridge method name.
 * @param params Parameters to pass.
 * @param message Message shown after success.
 * @return A promise resolving to true on success or false on failure.
 */
async function perform(method, params, message) {
    setBusy(true);
    showMessage("Working...", "info");
    try {
        await bridge(method, params);
        if (method === "startGateway" || method === "stopGateway") {
            currentState.running = undefined;
        }
        showMessage(message, "success");
        await refresh();
        return true;
    } catch (error) {
        showMessage(describeError(error), "error");
        return false;
    } finally {
        setBusy(false);
    }
}

/**
 * Select one service from the list.
 * @param event Click event.
 * @return void
 */
function handleServiceSelect(event) {
    var item = event.target;
    var index;

    while (item !== null && !item.classList.contains("service-item")) {
        item = item.parentElement;
    }
    if (item === null) return;
    for (index = 0; index < currentState.services.length; index++) {
        if (currentState.services[index].name ===
            item.getAttribute("data-name")) {
            fillForm(currentState.services[index]);
            return;
        }
    }
}

/**
 * Start a new service form.
 * @return void
 */
function handleNew() {
    clearForm();
    byId("field-display-name").focus();
    showMessage("Enter the new service details.", "info");
}

/**
 * Add or update the service in the form.
 * @param event Submit event.
 * @return A promise resolving after the save completes.
 */
async function handleSubmit(event) {
    var params = readForm();
    var editing = !byId("remove-service").hidden;

    event.preventDefault();
    if (params === null) return;
    if (await perform(editing ? "setService" : "addService", params,
        editing ? "Service updated." : "Service added.")) {
        if (!editing) clearForm();
        else {
            editedAuthentication = {
                scheme: params.scheme,
                configured: params.scheme !== "none"
            };
            byId("field-secret").value = "";
            renderAuthentication();
        }
    }
    params.secret = null;
}

/**
 * Open the confirmation dialog for the selected service.
 * @return void
 */
function handleRemove() {
    var name = byId("field-slug").value.trim();
    var dialog = byId("remove-dialog");
    var label = byId("field-display-name").value.trim() || name;

    if (name === "") {
        showMessage("Select a service to remove.", "error");
        return;
    }
    byId("remove-message").textContent = "Remove service " + label + "?";
    dialog.returnValue = "";
    dialog.showModal();
}

/**
 * Remove the selected service only after explicit confirmation.
 * @return A promise resolving after the removal completes.
 */
async function handleRemoveConfirm() {
    var dialog = byId("remove-dialog");
    var name = byId("field-slug").value.trim();

    if (dialog.returnValue !== "remove" || name === "") return;
    if (await perform("removeService", { name: name }, "Service removed.")) {
        clearForm();
    }
}

/**
 * Start or stop the gateway according to its reported state.
 * @return A promise resolving after the command completes.
 */
async function handleToggle() {
    if (currentState.running === true) {
        await perform("stopGateway", null, "Gateway stopped.");
    } else if (currentState.running === false) {
        await perform("startGateway", null, "Gateway started.");
    }
}

/**
 * Save the configured listen address.
 * @return A promise resolving after the command completes.
 */
async function handleListen() {
    var listen = byId("listen").value.trim();

    if (listen === "") {
        showMessage("A listen address is required.", "error");
        return;
    }
    await perform("setListen", { listen: listen }, currentState.running ?
        "Address saved. Restart gateway to apply." : "Address saved.");
}

/**
 * Apply the selected appearance and update the switch icon.
 * @param theme Theme name.
 * @return void
 */
function applyTheme(theme) {
    var light = theme === "light";
    var button = byId("toggle-theme");
    var label = light ? "Switch to dark theme" : "Switch to light theme";

    document.documentElement.setAttribute("data-theme",
        light ? "light" : "dark");
    byId("theme-sun").hidden = light;
    byId("theme-moon").hidden = !light;
    button.setAttribute("aria-label", label);
    button.title = label;
}

/**
 * Toggle the appearance and persist the manual preference.
 * @return A promise resolving after the preference is saved.
 */
async function handleTheme() {
    var theme = document.documentElement.getAttribute("data-theme") ===
        "light" ? "dark" : "light";

    if (themePending) return;
    themePending = true;
    applyTheme(theme);
    try {
        await bridge("setTheme", { theme: theme });
    } catch (error) {
        showMessage(describeError(error), "error");
    } finally {
        themePending = false;
    }
}

/**
 * Open the application information dialog.
 * @return void
 */
function handleAbout() {
    var version = currentState.version;

    byId("about-version").textContent = version ? "Version " + version :
        "Version unavailable";
    byId("about-error").hidden = true;
    byId("about-dialog").showModal();
}

/**
 * Open the documentation in the system browser.
 * @param event Link activation event.
 * @return A promise resolving after the request completes.
 */
async function handleDocumentation(event) {
    var error = byId("about-error");

    event.preventDefault();
    error.hidden = true;
    try {
        await bridge("openDocumentation", null);
    } catch (failure) {
        error.textContent = describeError(failure);
        error.hidden = false;
    }
}

/**
 * Attach interface event listeners.
 * @return void
 */
function bindEvents() {
    var copies = document.querySelectorAll(".api-copy");
    var index;

    for (index = 0; index < copies.length; index++) {
        copies[index].addEventListener("click", handleApiCopy);
    }
    byId("open-api").addEventListener("click", handleApi);
    byId("api-documentation").addEventListener("click", handleApiDocumentation);
    byId("service-list").addEventListener("click", handleServiceSelect);
    byId("new-service").addEventListener("click", handleNew);
    byId("service-form").addEventListener("submit", handleSubmit);
    byId("service-form").addEventListener("input", clearConnection);
    byId("service-form").addEventListener("change", clearConnection);
    byId("test-service").addEventListener("click", handleTestConnection);
    byId("field-adapter").addEventListener("change", handleProvider);
    byId("field-display-name").addEventListener("input",
        refreshSlugPlaceholder);
    byId("field-slug").addEventListener("input", handleSlugInput);
    byId("field-scheme").addEventListener("change", renderAuthentication);
    byId("remove-service").addEventListener("click", handleRemove);
    byId("remove-dialog").addEventListener("close", handleRemoveConfirm);
    byId("toggle-gateway").addEventListener("click", handleToggle);
    byId("apply-listen").addEventListener("click", handleListen);
    byId("toggle-theme").addEventListener("click", handleTheme);
    byId("open-about").addEventListener("click", handleAbout);
    byId("documentation").addEventListener("click", handleDocumentation);
}

/**
 * Initialize the interface.
 * @return void
 */
async function run() {
    bindEvents();
    clearForm();
    setBusy(true);
    try {
        applyTheme(await bridge("theme"));
    } catch (error) {
        applyTheme("dark");
    }
    try {
        await refresh();
        showMessage("", "info");
    } catch (error) {
        showMessage(describeError(error), "error");
    } finally {
        setBusy(false);
    }
}

run();
setInterval(pollServices, 1000);
