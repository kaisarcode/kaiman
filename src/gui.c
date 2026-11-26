/**
 * gui.c - Graphical frontend for kaiman.
 * Summary: Manages services and the gateway through a native WebView.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#include "gui.h"

#include "json.h"
#include "libkaiman.h"
#include "libwvw.h"
#include "util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <shellapi.h>
#elif defined(__APPLE__)
#include <crt_externs.h>
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#else
#include <gio/gio.h>
#endif

typedef struct {
    volatile sig_atomic_t stop;
} kc_gui_state_t;

static kc_gui_state_t *kc_gui_active = NULL;
static char *kc_gui_result = NULL;

/**
 * Copy the active bridge result into stable storage.
 * @param text Serialized JSON value.
 * @return KC_KAIMAN_OK or an error status.
 */
static int set_result(const char *text) {
    char *copy = kc_util_strdup(text);

    if (copy == NULL) return KC_KAIMAN_ENOMEM;
    kc_kaiman_free(kc_gui_result);
    kc_gui_result = copy;
    return KC_KAIMAN_OK;
}

/**
 * Store a successful bridge response.
 * @return KC_KAIMAN_OK or an error status.
 */
static int set_success(void) {
    return set_result("{\"ok\":true}");
}

/**
 * Store a serialized bridge error.
 * @param kaiman Open manager, or NULL.
 * @param status Status returned by the library.
 * @param override Detail message, or NULL to use the library detail.
 * @return The reported error status, or an error status from storage.
 */
static int set_error(const kc_kaiman_t *kaiman, int status,
    const char *override) {
    kc_buffer_t *buffer = NULL;
    const char *detail = override;
    char *escaped = NULL;
    int result;

    if (status == KC_KAIMAN_OK) status = KC_KAIMAN_EINVAL;
    if (detail == NULL) detail = kc_kaiman_error(kaiman);
    if (detail == NULL) detail = kc_kaiman_strerror(status);
    result = kc_buffer_open(&buffer);
    if (result != KC_KAIMAN_OK) return result;
    if (kc_json_escape(detail, &escaped) != KC_KAIMAN_OK) escaped = NULL;
    if (escaped != NULL) {
        result = kc_buffer_format(buffer,
            "{\"ok\":false,\"status\":%d,\"message\":\"%s\"}",
            status, escaped);
    } else {
        result = kc_buffer_format(buffer,
            "{\"ok\":false,\"status\":%d,\"message\":\"operation failed\"}",
            status);
    }
    kc_kaiman_free(escaped);
    if (result == KC_KAIMAN_OK) result = set_result(kc_buffer_value(buffer));
    kc_buffer_close(buffer);
    if (result != KC_KAIMAN_OK) return result;
    return status;
}

/**
 * Append one escaped JSON string.
 * @param buffer Open buffer.
 * @param text Text to append, or NULL for JSON null.
 * @return KC_KAIMAN_OK or an error status.
 */
static int append_json_string(kc_buffer_t *buffer, const char *text) {
    char *escaped = NULL;
    int result;

    if (text == NULL) return kc_buffer_text(buffer, "null");
    result = kc_json_escape(text, &escaped);
    if (result != KC_KAIMAN_OK) return result;
    result = kc_buffer_format(buffer, "\"%s\"", escaped);
    kc_kaiman_free(escaped);
    return result;
}

/**
 * Append every configured service as a JSON array.
 * @param buffer Open buffer.
 * @param kaiman Open manager.
 * @return KC_KAIMAN_OK or an error status.
 */
static int append_services(kc_buffer_t *buffer, kc_kaiman_t *kaiman) {
    static const char *const keys[] = {"name", "adapter", "url", "model",
        "env", "token-command", "scheme", "user"};
    kc_kaiman_service_t **services = NULL;
    size_t count = 0U;
    size_t index;
    size_t field;
    int result;

    result = kc_kaiman_get_services(kaiman, &services, &count);
    if (result != KC_KAIMAN_OK) return result;
    result = kc_buffer_text(buffer, "[");
    if (result != KC_KAIMAN_OK) return result;
    for (index = 0U; index < count; index++) {
        if (index > 0U) {
            result = kc_buffer_text(buffer, ",");
            if (result != KC_KAIMAN_OK) return result;
        }
        result = kc_buffer_text(buffer, "{");
        if (result != KC_KAIMAN_OK) return result;
        for (field = 0U; field < sizeof(keys) / sizeof(keys[0]); field++) {
            if (field > 0U) {
                result = kc_buffer_text(buffer, ",");
                if (result != KC_KAIMAN_OK) return result;
            }
            result = kc_buffer_format(buffer, "\"%s\":", keys[field]);
            if (result != KC_KAIMAN_OK) return result;
            result = append_json_string(buffer,
                kc_kaiman_service_get(services[index], keys[field]));
            if (result != KC_KAIMAN_OK) return result;
        }
        result = kc_buffer_format(buffer, ",\"timeout\":%d}",
            kc_kaiman_service_timeout(services[index]));
        if (result != KC_KAIMAN_OK) return result;
    }
    return kc_buffer_text(buffer, "]");
}

/**
 * Read one required string member from bridge parameters.
 * @param params Borrowed JSON object.
 * @param name Member name.
 * @return Borrowed string, or NULL when absent or mistyped.
 */
static const char *require_string(const kc_json_value_t *params,
    const char *name) {
    kc_json_value_t *value = kc_json_member(params, name);

    if (kc_json_type(value) != KC_JSON_STRING) return NULL;
    return kc_json_text(value);
}

/**
 * Parse bridge parameters as one JSON document.
 * @param params_json Serialized parameters.
 * @param out Receives an owned JSON value.
 * @return KC_KAIMAN_OK or an error status.
 */
static int parse_params(const char *params_json, kc_json_value_t **out) {
    if (params_json == NULL) return KC_KAIMAN_EINVAL;
    return kc_json_parse(params_json, strlen(params_json), out);
}

/**
 * Apply editable service fields from bridge parameters.
 * @param service Managed service.
 * @param params Borrowed JSON object.
 * @return KC_KAIMAN_OK or an error status.
 */
static int apply_service_params(kc_kaiman_service_t *service,
    const kc_json_value_t *params) {
    static const char *const keys[] = {"adapter", "url", "model", "env",
        "token-command", "scheme", "user"};
    kc_json_value_t *value;
    char timeout[32];
    double number;
    size_t index;
    int result;

    for (index = 0U; index < sizeof(keys) / sizeof(keys[0]); index++) {
        value = kc_json_member(params, keys[index]);
        if (value == NULL) continue;
        if (kc_json_type(value) != KC_JSON_STRING) return KC_KAIMAN_EINVAL;
        result = kc_kaiman_service_set(service, keys[index],
            kc_json_text(value));
        if (result != KC_KAIMAN_OK) return result;
    }
    value = kc_json_member(params, "timeout");
    if (value == NULL) return KC_KAIMAN_OK;
    if (kc_json_type(value) != KC_JSON_NUMBER) return KC_KAIMAN_EINVAL;
    number = kc_json_number(value);
    if (number < 1.0 || number > 600.0 ||
        number != (double)(int)number) {
        return KC_KAIMAN_EINVAL;
    }
    (void)snprintf(timeout, sizeof(timeout), "%d", (int)number);
    return kc_kaiman_service_set(service, "timeout", timeout);
}

/**
 * Serve the state bridge method.
 * @return KC_KAIMAN_OK or an error status.
 */
static int bridge_state(void) {
    kc_kaiman_t *kaiman = NULL;
    kc_buffer_t *buffer = NULL;
    char *description = NULL;
    int running = 0;
    int status_result;
    int result;

    status_result = kc_kaiman_open(&kaiman, NULL);
    if (status_result != KC_KAIMAN_OK) {
        return set_error(NULL, status_result, NULL);
    }
    status_result = kc_kaiman_status(&description);
    if (status_result == KC_KAIMAN_OK && description != NULL &&
        strncmp(description, "running", 7) == 0) {
        running = 1;
    }
    result = kc_buffer_open(&buffer);
    if (result == KC_KAIMAN_OK) {
        result = kc_buffer_text(buffer, "{\"ok\":true,\"path\":");
    }
    if (result == KC_KAIMAN_OK) {
        result = append_json_string(buffer, kc_kaiman_path(kaiman));
    }
    if (result == KC_KAIMAN_OK) {
        result = kc_buffer_text(buffer, ",\"address\":");
    }
    if (result == KC_KAIMAN_OK) {
        result = append_json_string(buffer, kc_kaiman_address(kaiman));
    }
    if (result == KC_KAIMAN_OK) {
        result = kc_buffer_text(buffer, ",\"version\":");
    }
    if (result == KC_KAIMAN_OK) {
        result = append_json_string(buffer, kc_kaiman_version());
    }
    if (result == KC_KAIMAN_OK) {
        result = kc_buffer_text(buffer, ",\"status\":");
    }
    if (result == KC_KAIMAN_OK) {
        result = append_json_string(buffer, description);
    }
    if (result == KC_KAIMAN_OK) {
        result = kc_buffer_format(buffer, ",\"running\":%s,\"services\":",
            running != 0 ? "true" : "false");
    }
    if (result == KC_KAIMAN_OK) result = append_services(buffer, kaiman);
    if (result == KC_KAIMAN_OK) result = kc_buffer_text(buffer, "}");
    if (result == KC_KAIMAN_OK) result = set_result(kc_buffer_value(buffer));
    kc_kaiman_free(description);
    kc_kaiman_close(kaiman);
    kc_buffer_close(buffer);
    if (result != KC_KAIMAN_OK) {
        (void)set_error(NULL, result, NULL);
    }
    return result;
}

/**
 * Serve the addService bridge method.
 * @param params_json Serialized parameters.
 * @return KC_KAIMAN_OK or an error status.
 */
static int bridge_add_service(const char *params_json) {
    kc_json_value_t *params = NULL;
    kc_kaiman_t *kaiman = NULL;
    kc_kaiman_service_t *service = NULL;
    kc_kaiman_descriptor_t descriptor;
    const char *name;
    int result;

    result = parse_params(params_json, &params);
    if (result != KC_KAIMAN_OK) return set_error(NULL, result,
        "expected one JSON object");
    name = require_string(params, "name");
    if (name == NULL) {
        result = KC_KAIMAN_EINVAL;
        (void)set_error(NULL, result, "service name is required");
        goto done;
    }
    result = kc_kaiman_open(&kaiman, NULL);
    if (result != KC_KAIMAN_OK) {
        (void)set_error(NULL, result, NULL);
        goto done;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.name = name;
    result = kc_kaiman_add_service(kaiman, &descriptor, &service);
    if (result == KC_KAIMAN_OK) {
        result = apply_service_params(service, params);
    }
    if (result == KC_KAIMAN_OK) result = set_success();
    else (void)set_error(kaiman, result, NULL);

done:
    kc_kaiman_close(kaiman);
    kc_json_free(params);
    return result;
}

/**
 * Serve the setService bridge method.
 * @param params_json Serialized parameters.
 * @return KC_KAIMAN_OK or an error status.
 */
static int bridge_set_service(const char *params_json) {
    kc_json_value_t *params = NULL;
    kc_kaiman_t *kaiman = NULL;
    kc_kaiman_service_t *service = NULL;
    const char *name;
    int result;

    result = parse_params(params_json, &params);
    if (result != KC_KAIMAN_OK) return set_error(NULL, result,
        "expected one JSON object");
    name = require_string(params, "name");
    if (name == NULL) {
        result = KC_KAIMAN_EINVAL;
        (void)set_error(NULL, result, "service name is required");
        goto done;
    }
    result = kc_kaiman_open(&kaiman, NULL);
    if (result != KC_KAIMAN_OK) {
        (void)set_error(NULL, result, NULL);
        goto done;
    }
    result = kc_kaiman_get_service(kaiman, name, &service);
    if (result == KC_KAIMAN_OK) result = apply_service_params(service, params);
    if (result == KC_KAIMAN_OK) result = set_success();
    else (void)set_error(kaiman, result, NULL);

done:
    kc_kaiman_close(kaiman);
    kc_json_free(params);
    return result;
}

/**
 * Serve the removeService bridge method.
 * @param params_json Serialized parameters.
 * @return KC_KAIMAN_OK or an error status.
 */
static int bridge_remove_service(const char *params_json) {
    kc_json_value_t *params = NULL;
    kc_kaiman_t *kaiman = NULL;
    kc_kaiman_service_t *service = NULL;
    const char *name;
    int result;

    result = parse_params(params_json, &params);
    if (result != KC_KAIMAN_OK) return set_error(NULL, result,
        "expected one JSON object");
    name = require_string(params, "name");
    if (name == NULL) {
        result = KC_KAIMAN_EINVAL;
        (void)set_error(NULL, result, "service name is required");
        goto done;
    }
    result = kc_kaiman_open(&kaiman, NULL);
    if (result != KC_KAIMAN_OK) {
        (void)set_error(NULL, result, NULL);
        goto done;
    }
    result = kc_kaiman_get_service(kaiman, name, &service);
    if (result == KC_KAIMAN_OK) result = kc_kaiman_service_remove(service);
    if (result == KC_KAIMAN_OK) result = set_success();
    else (void)set_error(kaiman, result, NULL);

done:
    kc_kaiman_close(kaiman);
    kc_json_free(params);
    return result;
}

/**
 * Serve the setListen bridge method.
 * @param params_json Serialized parameters.
 * @return KC_KAIMAN_OK or an error status.
 */
static int bridge_set_listen(const char *params_json) {
    kc_json_value_t *params = NULL;
    kc_kaiman_t *kaiman = NULL;
    const char *listen;
    int result;

    result = parse_params(params_json, &params);
    if (result != KC_KAIMAN_OK) return set_error(NULL, result,
        "expected one JSON object");
    listen = require_string(params, "listen");
    if (listen == NULL) {
        result = KC_KAIMAN_EINVAL;
        (void)set_error(NULL, result, "listen address is required");
        goto done;
    }
    result = kc_kaiman_open(&kaiman, NULL);
    if (result != KC_KAIMAN_OK) {
        (void)set_error(NULL, result, NULL);
        goto done;
    }
    result = kc_kaiman_listen(kaiman, listen);
    if (result == KC_KAIMAN_OK) result = set_success();
    else (void)set_error(kaiman, result, NULL);

done:
    kc_kaiman_close(kaiman);
    kc_json_free(params);
    return result;
}

/**
 * Serve the startGateway bridge method.
 * @return KC_KAIMAN_OK or an error status.
 */
static int bridge_start_gateway(void) {
    int result = kc_kaiman_start(NULL);

    if (result == KC_KAIMAN_OK) return set_success();
    (void)set_error(NULL, result, NULL);
    return result;
}

/**
 * Serve the stopGateway bridge method.
 * @return KC_KAIMAN_OK or an error status.
 */
static int bridge_stop_gateway(void) {
    int result = kc_kaiman_stop();

    if (result == KC_KAIMAN_OK) return set_success();
    (void)set_error(NULL, result, NULL);
    return result;
}

/**
 * Serve the quit bridge method.
 * @param state Active GUI state.
 * @return KC_KAIMAN_OK or an error status.
 */
static int bridge_quit(kc_gui_state_t *state) {
    if (state == NULL) return KC_WVW_ERROR;
    state->stop = 1;
    return set_success();
}

/**
 * Open the project documentation using the system browser.
 * @return KC_KAIMAN_OK or an error status.
 */
static int bridge_open_documentation(void) {
    const char *url =
        "https://gitlab.com/kaisarcode/kaiman.c/-/blob/slave/README.md";
#ifdef _WIN32
    if ((INT_PTR)ShellExecuteA(NULL, "open", url, NULL, NULL,
        SW_SHOWNORMAL) <= 32) {
        return set_error(NULL, KC_KAIMAN_EIO,
            "Unable to open documentation.");
    }
#elif defined(__APPLE__)
    char *args[] = {"open", (char *)url, NULL};
    pid_t pid;
    pid_t waited;
    int status;

    if (posix_spawn(&pid, "/usr/bin/open", NULL, NULL, args,
        *_NSGetEnviron()) != 0) {
        return set_error(NULL, KC_KAIMAN_EIO,
            "Unable to open documentation.");
    }
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return set_error(NULL, KC_KAIMAN_EIO,
            "Unable to open documentation.");
    }
#else
    if (!g_app_info_launch_default_for_uri(url, NULL, NULL)) {
        return set_error(NULL, KC_KAIMAN_EIO,
            "Unable to open documentation.");
    }
#endif
    return set_success();
}

/**
 * Dispatch one NativeBridge method to the library.
 * @param wvw Open WebView, unused.
 * @param method Method name.
 * @param params_json Serialized parameters.
 * @param out_result_json Receives the serialized result.
 * @param userdata Active GUI state.
 * @return KC_WVW_OK on success, or KC_WVW_ERROR on failure.
 */
static int bridge_callback(kc_wvw_t *wvw, const char *method,
    const char *params_json, const char **out_result_json, void *userdata) {
    kc_gui_state_t *state = (kc_gui_state_t *)userdata;
    int result;

    (void)wvw;
    if (out_result_json == NULL) return KC_WVW_ERROR;
    *out_result_json = "null";
    if (method == NULL) return KC_WVW_ERROR;
    if (strcmp(method, "state") == 0) result = bridge_state();
    else if (strcmp(method, "addService") == 0) {
        result = bridge_add_service(params_json);
    } else if (strcmp(method, "setService") == 0) {
        result = bridge_set_service(params_json);
    } else if (strcmp(method, "removeService") == 0) {
        result = bridge_remove_service(params_json);
    } else if (strcmp(method, "setListen") == 0) {
        result = bridge_set_listen(params_json);
    } else if (strcmp(method, "startGateway") == 0) {
        result = bridge_start_gateway();
    } else if (strcmp(method, "stopGateway") == 0) {
        result = bridge_stop_gateway();
    } else if (strcmp(method, "openDocumentation") == 0) {
        result = bridge_open_documentation();
    } else if (strcmp(method, "quit") == 0) {
        result = bridge_quit(state);
    } else {
        result = set_error(NULL, KC_KAIMAN_EINVAL, "unknown GUI method");
    }
    *out_result_json = kc_gui_result != NULL ? kc_gui_result : "null";
    return result == KC_KAIMAN_OK ? KC_WVW_OK : KC_WVW_ERROR;
}

/**
 * Report whether one byte is safe without percent encoding in a file URL.
 * @param value Byte value.
 * @return 1 when safe, 0 otherwise.
 */
static int url_byte_safe(unsigned char value) {
    if (value >= 'a' && value <= 'z') return 1;
    if (value >= 'A' && value <= 'Z') return 1;
    if (value >= '0' && value <= '9') return 1;
    if (value == '/' || value == ':' || value == '.' || value == '_') {
        return 1;
    }
    if (value == '-' || value == '~') return 1;
    return 0;
}

/**
 * Append one absolute path as a trusted file URL.
 * @param buffer Open buffer.
 * @param path Absolute path to the local asset.
 * @return KC_KAIMAN_OK or an error status.
 */
static int append_file_url(kc_buffer_t *buffer, const char *path) {
    const unsigned char *cursor;
    int result;

#ifdef _WIN32
    result = kc_buffer_text(buffer, "file:///");
#else
    result = kc_buffer_text(buffer, "file://");
#endif
    if (result != KC_KAIMAN_OK) return result;
    for (cursor = (const unsigned char *)path; *cursor != '\0'; cursor++) {
        unsigned char value = *cursor;

        if (value == '\\') value = '/';
        if (url_byte_safe(value)) {
            result = kc_buffer_append(buffer, &value, 1U);
        } else {
            result = kc_buffer_format(buffer, "%%%02X", (unsigned int)value);
        }
        if (result != KC_KAIMAN_OK) return result;
    }
    return KC_KAIMAN_OK;
}

/**
 * Resolve a bundled GUI asset relative to the executable.
 * @param name Asset filename.
 * @return Owned path, or NULL on failure.
 */
static char *gui_asset_path(const char *name) {
    kc_buffer_t *asset = NULL;
    char *executable = NULL;
    char *result = NULL;
    char *separator;
    int status;

    executable = kc_util_executable_path();
    if (executable == NULL) return NULL;
    status = kc_buffer_open(&asset);
    if (status != KC_KAIMAN_OK) goto done;
    separator = strrchr(executable, '/');
#ifdef _WIN32
    {
        char *windows_separator = strrchr(executable, '\\');

        if (windows_separator != NULL &&
            (separator == NULL || windows_separator > separator)) {
            separator = windows_separator;
        }
    }
#endif
    if (separator != NULL) {
        status = kc_buffer_format(asset, "%.*s%cgui/%s",
            (int)(separator - executable), executable, *separator, name);
    } else {
        status = kc_buffer_format(asset, "gui/%s", name);
    }
    if (status != KC_KAIMAN_OK) goto done;
    result = kc_util_strdup(kc_buffer_value(asset));

done:
    kc_buffer_close(asset);
    kc_kaiman_free(executable);
    return result;
}

/**
 * Resolve the file URL of the bundled GUI entry document.
 * @return Owned URL, or NULL on failure.
 */
static char *gui_index_url(void) {
    kc_buffer_t *url = NULL;
    char *path = gui_asset_path("index.html");
    char *result = NULL;

    if (path == NULL) return NULL;
    if (kc_buffer_open(&url) == KC_KAIMAN_OK &&
        append_file_url(url, path) == KC_KAIMAN_OK) {
        result = kc_util_strdup(kc_buffer_value(url));
    }
    kc_buffer_close(url);
    kc_kaiman_free(path);
    return result;
}

/**
 * Request that the active GUI stop after the current callback.
 * @param signum Signal number, unused.
 * @return void
 */
static void gui_signal(int signum) {
    (void)signum;
    if (kc_gui_active != NULL) kc_gui_active->stop = 1;
}

/**
 * Run the graphical service manager.
 * @return 0 on success, 1 on failure.
 */
int kc_kaiman_gui_run(void) {
    static const char *const methods[] = {"state", "addService",
        "setService", "removeService", "setListen", "startGateway",
        "stopGateway", "openDocumentation", "quit"};
    kc_gui_state_t state;
    kc_wvw_t *wvw = NULL;
    kc_wvw_options_t options;
    kc_wvw_bridge_options_t bridge;
    char *url = NULL;
    char *icon = NULL;
    const char *detail;
    int width = 1120;
    int height = 760;
    int visible;
    int result = 1;

    state.stop = 0;
    url = gui_index_url();
#ifdef _WIN32
    icon = gui_asset_path("logo.ico");
#else
    icon = gui_asset_path("logo-128.png");
#endif
    if (url == NULL || icon == NULL) {
        fprintf(stderr, "kaiman: unable to resolve GUI assets\n");
        goto done;
    }
    memset(&options, 0, sizeof(options));
    options.url = url;
    options.title = "kaiman";
    options.icon = icon;
    options.width = &width;
    options.height = &height;
    options.allow_file = 1;
    if (kc_wvw_open(&wvw, &options) != KC_WVW_OK) {
        detail = wvw != NULL ? kc_wvw_get_error(wvw) : NULL;
        fprintf(stderr, "kaiman: %s\n",
            detail != NULL ? detail : "unable to open GUI");
        goto done;
    }
    memset(&bridge, 0, sizeof(bridge));
    bridge.methods = methods;
    bridge.method_count = sizeof(methods) / sizeof(methods[0]);
    bridge.callback = bridge_callback;
    bridge.userdata = &state;
    if (kc_wvw_set_bridge_methods(wvw, &bridge) != KC_WVW_OK) {
        detail = kc_wvw_get_error(wvw);
        fprintf(stderr, "kaiman: %s\n",
            detail != NULL ? detail : "unable to initialize GUI bridge");
        goto done;
    }
    if (kc_wvw_navigate(wvw, url) != KC_WVW_OK) {
        detail = kc_wvw_get_error(wvw);
        fprintf(stderr, "kaiman: %s\n",
            detail != NULL ? detail : "unable to load GUI assets");
        goto done;
    }
    kc_gui_active = &state;
    signal(SIGINT, gui_signal);
#ifndef _WIN32
    signal(SIGTERM, gui_signal);
#endif
    while (state.stop == 0) {
        visible = kc_wvw_is_visible(wvw);
        if (visible == 1 ||
            (visible == 0 && kc_wvw_is_minimized(wvw) == 1)) {
            kc_util_sleep(100);
        } else {
            state.stop = 1;
        }
    }
    result = 0;

done:
    kc_gui_active = NULL;
    kc_wvw_close(wvw);
    kc_kaiman_free(url);
    kc_kaiman_free(icon);
    kc_kaiman_free(kc_gui_result);
    kc_gui_result = NULL;
    return result;
}
