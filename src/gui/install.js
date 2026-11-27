/**
 * install.js - Desktop installer interface.
 * Summary: Collects installation options and invokes native operations.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

var form = document.getElementById("install-form");
var directory = document.getElementById("directory");
var category = document.getElementById("category");
var result = document.getElementById("install-result");
var button = document.getElementById("install");
var browse = document.getElementById("browse");
var closeButton = document.getElementById("close");
var installed = false;
var blocked = false;
var uninstall = false;
var update = false;
var purge = document.getElementById("purge");

/**
 * Show an installation result.
 * @param message Message to display.
 * @param success Whether the operation succeeded.
 * @return void
 */
function showResult(message, success) {
    result.textContent = message;
    result.dataset.kind = success ? "success" : "error";
    result.hidden = !message;
}

/**
 * Select an installation directory.
 * @return A promise resolving after selection.
 */
async function handleBrowse() {
    try {
        var path = await window.NativeBridge.browse();

        if (path) directory.value = path;
    } catch (error) {
        showResult("Unable to open the folder chooser.", false);
    }
}

/**
 * Copy the application and create the menu entry.
 * @param event Form submission event.
 * @return A promise resolving after installation.
 */
async function handleInstall(event) {
    event.preventDefault();
    if (installed || blocked) {
        await window.NativeBridge.close();
        return;
    }
    button.disabled = true;
    browse.disabled = true;
    closeButton.disabled = true;
    directory.disabled = true;
    category.disabled = true;
    purge.disabled = true;
    showResult("", false);
    try {
        var error;

        if (uninstall) {
            button.textContent = "Uninstalling...";
            error = await window.NativeBridge.uninstall({
                purge: purge.checked
            });
        } else if (update) {
            button.textContent = "Updating...";
            error = await window.NativeBridge.update();
        } else {
            error = await window.NativeBridge.install({
                directory: directory.value.trim(),
                category: category.value.trim()
            });
        }
        if (error) {
            showResult(error, false);
        } else if (uninstall || update) {
            installed = true;
            await window.NativeBridge.close();
        } else {
            installed = true;
            button.textContent = "Close";
            showResult("Installation complete.", true);
        }
    } catch (error) {
        showResult(uninstall ? "Uninstallation failed." :
            "Installation failed.", false);
    } finally {
        button.disabled = false;
        closeButton.disabled = false;
        browse.disabled = installed || uninstall || update;
        directory.disabled = installed || uninstall || update;
        category.disabled = installed || uninstall || update;
        purge.disabled = installed;
        if (uninstall && !installed) button.textContent = "Uninstall";
        if (update && !installed) button.textContent = "Update";
    }
}

/**
 * Close the installer.
 * @return void
 */
function handleClose() {
    if (window.NativeBridge && window.NativeBridge.close) {
        window.NativeBridge.close();
    } else if (window.NativeBridge && window.NativeBridge.Window) {
        window.NativeBridge.Window.close();
    }
}

/**
 * Initialize the installer with native defaults.
 * @return A promise resolving when ready.
 */
async function run() {
    form.addEventListener("submit", handleInstall);
    browse.addEventListener("click", handleBrowse);
    closeButton.addEventListener("click", handleClose);
    try {
        if (!window.NativeBridge || !window.NativeBridge.directory) {
            throw new Error("Native bridge unavailable.");
        }
        var mode = await window.NativeBridge.mode();

        uninstall = mode === "uninstall";
        update = mode === "update";
        if (await window.NativeBridge.platform() === "windows") {
            category.hidden = true;
            category.disabled = true;
            category = document.getElementById("category-windows");
            category.hidden = false;
            category.disabled = false;
            document.querySelector("#category-field label").htmlFor =
                category.id;
        }
        directory.value = await window.NativeBridge.directory();
        var message = await window.NativeBridge.blocked();
        if (uninstall) {
            document.title = "kaiman uninstaller";
            document.getElementById("category-field").hidden = true;
            document.getElementById("purge-option").hidden = false;
            browse.hidden = true;
            directory.readOnly = true;
            button.classList.remove("button-primary");
            button.classList.add("button-danger");
            button.textContent = "Uninstall";
        }
        if (update) {
            document.title = "kaiman update";
            document.getElementById("category-field").hidden = true;
            category.disabled = true;
            browse.hidden = true;
            directory.readOnly = true;
            button.textContent = "Update";
            var version = document.getElementById("update-version");
            var installedVersion = await window.NativeBridge.installedVersion();
            var candidateVersion = await window.NativeBridge.version();

            if (installedVersion && candidateVersion) {
                version.textContent = installedVersion + " \u2192 " + candidateVersion;
                version.hidden = false;
            }
        }
        if (message) {
            blocked = true;
            directory.disabled = true;
            category.disabled = true;
            purge.disabled = true;
            browse.hidden = true;
            button.textContent = "Close";
            button.classList.remove("button-danger");
            button.classList.remove("button-primary");
            showResult(message, false);
        }
        button.disabled = false;
        browse.disabled = uninstall || update || blocked;
    } catch (error) {
        showResult(error.message || "Installer initialization failed.", false);
    }
    if (window.NativeBridge && window.NativeBridge.ready) {
        await window.NativeBridge.ready();
    }
    document.body.classList.remove("installer-loading");
}

run();
