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
        name.textContent = services[index].name;
        detail = document.createElement("span");
        detail.className = "service-detail";
        detail.textContent = (services[index].adapter || "no adapter") +
            " / " + (services[index].model || "no model");
        item = document.createElement("button");
        item.type = "button";
        item.className = "service-item";
        item.setAttribute("data-name", services[index].name);
        item.appendChild(name);
        item.appendChild(detail);
        list.appendChild(item);
    }
}

/**
 * Show only the fields used by the selected authentication method.
 * @return void
 */
function renderAuthentication() {
    var scheme = byId("field-scheme").value;
    var authenticated = scheme === "bearer" || scheme === "basic";
    var basic = scheme === "basic";
    var credential = basic ? "Password" : "API key";
    var saved = editedAuthentication !== null &&
        editedAuthentication.scheme === scheme &&
        editedAuthentication.configured;
    var secret = byId("field-secret");

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
 * Reset the service form to its add state.
 * @return void
 */
function clearForm() {
    var form = byId("service-form");
    var items = document.querySelectorAll(".service-item");
    var index;

    form.reset();
    byId("field-adapter").value = "openai";
    byId("field-scheme").value = "none";
    editedAuthentication = null;
    renderAuthentication();
    byId("field-timeout").value = "60";
    byId("field-name").readOnly = false;
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
    var fields = ["adapter", "url", "model", "scheme", "user"];
    var items = document.querySelectorAll(".service-item");
    var index;

    setValue(byId("field-name"), service.name);
    for (index = 0; index < fields.length; index++) {
        setValue(byId("field-" + fields[index]), service[fields[index]]);
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
    renderAuthentication();
    setValue(byId("field-timeout"), service.timeout);
    byId("field-name").readOnly = true;
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

    params.name = byId("field-name").value.trim();
    if (params.name === "") {
        showMessage("A service name is required.", "error");
        return null;
    }
    params.adapter = byId("field-adapter").value;
    params.scheme = byId("field-scheme").value;
    if (byId("field-url").value !== "") {
        params.url = byId("field-url").value;
    }
    params.model = byId("field-model").value;
    params.secret = byId("field-secret").disabled ||
        byId("field-secret").value === "" ? null : byId("field-secret").value;
    params.user = byId("field-user").disabled ? "" :
        byId("field-user").value;
    timeout = parseInt(byId("field-timeout").value, 10);
    params.timeout = isNaN(timeout) ? 0 : timeout;
    return params;
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
    toggle.textContent = result.running ? "Stop" : "Start";
    toggle.classList.toggle("button-primary", !result.running);
    toggle.classList.toggle("button-danger", result.running);
    renderServices(result.services || []);
    state.textContent = result.running ? "Running" : "Stopped";
    state.className = result.running ? "state state-running" :
        "state state-stopped";
    state.title = result.status || "";
    setValue(byId("gateway-address"), result.address);
    setValue(byId("config-path"), result.path);
    setValue(byId("version"), result.version);
    setValue(byId("listen"), result.address);
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
    byId("field-name").focus();
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
    var name = byId("field-name").value.trim();
    var dialog = byId("remove-dialog");

    if (name === "") {
        showMessage("Select a service to remove.", "error");
        return;
    }
    byId("remove-message").textContent = "Remove service " + name + "?";
    dialog.returnValue = "";
    dialog.showModal();
}

/**
 * Remove the selected service only after explicit confirmation.
 * @return A promise resolving after the removal completes.
 */
async function handleRemoveConfirm() {
    var dialog = byId("remove-dialog");
    var name = byId("field-name").value.trim();

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
    await perform("setListen", { listen: listen }, "Address saved.");
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
    byId("service-list").addEventListener("click", handleServiceSelect);
    byId("new-service").addEventListener("click", handleNew);
    byId("service-form").addEventListener("submit", handleSubmit);
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
