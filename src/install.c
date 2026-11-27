/**
 * install.c - Desktop installer for kaiman.
 * Summary: Copies the desktop runtime and registers its application menu.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "libkaiman.h"
#include "libmenu.h"
#include "libwvw.h"
#include "json.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <shlobj.h>
#else
#include <gtk/gtk.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define INSTALL_EXE "kaiman.exe"
#define INSTALL_EXT ".dll"
#define INSTALL_ICON "gui/tray-started.ico"
#else
#define INSTALL_EXE "kaiman"
#define INSTALL_EXT ".so"
#define INSTALL_ICON "gui/tray-started.png"
#endif

#define INSTALL_WIDTH 460
#define INSTALL_HEIGHT 440

typedef struct {
    char *source;
    char *directory;
    char *response;
    atomic_int stop;
    atomic_int ready;
} kc_install_t;

static const char *const install_files[] = {
    INSTALL_EXE, "libkaiman" INSTALL_EXT, "libhttp" INSTALL_EXT,
    "libnetl" INSTALL_EXT, "libtls" INSTALL_EXT, "libwvw" INSTALL_EXT,
    "libtray" INSTALL_EXT, "gui/index.html", "gui/app.js", "gui/styles.css",
    "gui/logo-46.png", "gui/logo-92.png", "gui/logo-128.png", "gui/logo.ico",
    "gui/tray-started.png", "gui/tray-stopped.png", "gui/tray-started.ico",
    "gui/tray-stopped.ico"
#ifdef _WIN32
    , "WebView2Loader.dll"
#endif
};

/**
 * Join a directory and relative path.
 * @param directory Base directory.
 * @param name Relative path.
 * @return Owned path, or NULL on failure.
 */
static char *install_join(const char *directory, const char *name) {
    size_t length = strlen(directory) + strlen(name) + 2U;
    char *path = (char *)malloc(length);

    if (path != NULL) (void)snprintf(path, length, "%s/%s", directory, name);
    return path;
}

/**
 * Resolve the directory containing the installer.
 * @return Owned directory, or NULL on failure.
 */
static char *install_source(void) {
    char *path = kc_util_executable_path();
    char *end;

    if (path == NULL) return NULL;
    end = strrchr(path, '/');
#ifdef _WIN32
    {
        char *other = strrchr(path, '\\');

        if (other != NULL && (end == NULL || other > end)) end = other;
    }
#endif
    if (end == NULL) {
        free(path);
        return NULL;
    }
    *end = '\0';
    return path;
}

/**
 * Resolve the default per-user installation directory.
 * @return Owned directory, or NULL on failure.
 */
static char *install_default(void) {
#ifdef _WIN32
    const char *home = getenv("LOCALAPPDATA");

    return home != NULL && *home != '\0' ?
        install_join(home, "kaisarcode/kaiman") : NULL;
#else
    const char *home = getenv("HOME");

    return home != NULL && *home != '\0' ?
        install_join(home, ".local/opt/kaisarcode/kaiman") : NULL;
#endif
}

/**
 * Create one directory or accept an existing directory.
 * @param path Directory path.
 * @return 1 on success, otherwise 0.
 */
static int install_mkdir(const char *path) {
    struct stat info;

    if (stat(path, &info) == 0) return S_ISDIR(info.st_mode);
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0755) == 0;
#endif
}

/**
 * Create an absolute directory and its parents.
 * @param path Absolute directory path.
 * @return 1 on success, otherwise 0.
 */
static int install_parents(const char *path) {
    char *copy = kc_util_strdup(path);
    char *cursor;
    int result = 1;

    if (copy == NULL) return 0;
    for (cursor = copy + 1; *cursor != '\0'; cursor++) {
#ifdef _WIN32
        if (*cursor == '\\') *cursor = '/';
        if (*cursor == '/' && cursor == copy + 2 && copy[1] == ':') continue;
#endif
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (*copy != '\0' && !install_mkdir(copy)) result = 0;
        *cursor = '/';
        if (!result) break;
    }
    if (result) result = install_mkdir(copy);
    free(copy);
    return result;
}

/**
 * Validate an absolute installation path.
 * @param path Candidate directory.
 * @return 1 when supported, otherwise 0.
 */
static int install_absolute(const char *path) {
    if (path == NULL || strpbrk(path, "\r\n\"") != NULL) return 0;
#ifdef _WIN32
    return strlen(path) > 3U && isalpha((unsigned char)path[0]) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/');
#else
    return path[0] == '/' && path[1] != '\0';
#endif
}

/**
 * Copy one runtime file without truncating the source.
 * @param source Source directory.
 * @param directory Destination directory.
 * @param name Relative runtime filename.
 * @return 1 on success, otherwise 0.
 */
static int install_copy(const char *source, const char *directory,
    const char *name) {
    char *from = install_join(source, name);
    char *to = install_join(directory, name);
    struct stat src;
    int result = 0;
#ifndef _WIN32
    struct stat dst;
#endif

    if (from == NULL || to == NULL || stat(from, &src) != 0 ||
        !S_ISREG(src.st_mode)) goto done;
#ifdef _WIN32
    {
        char a[32768];
        char b[32768];
        DWORD na = GetFullPathNameA(from, sizeof(a), a, NULL);
        DWORD nb = GetFullPathNameA(to, sizeof(b), b, NULL);

        if (na == 0 || nb == 0 || na >= sizeof(a) || nb >= sizeof(b)) {
            goto done;
        }
        if (_stricmp(a, b) == 0) result = 1;
        else result = CopyFileA(from, to, FALSE) != 0;
    }
#else
    if (stat(to, &dst) == 0 && src.st_dev == dst.st_dev &&
        src.st_ino == dst.st_ino) {
        result = 1;
    } else {
        char *temp = install_join(directory, ".kaiman-copy-XXXXXX");
        int fd = temp != NULL ? mkstemp(temp) : -1;
        FILE *in = fopen(from, "rb");
        FILE *out = fd >= 0 ? fdopen(fd, "wb") : NULL;
        char buffer[32768];
        size_t count;

        if (out == NULL && fd >= 0) close(fd);
        if (in != NULL && out != NULL) {
            result = 1;
            while ((count = fread(buffer, 1U, sizeof(buffer), in)) != 0U) {
                if (fwrite(buffer, 1U, count, out) != count) {
                    result = 0;
                    break;
                }
            }
            if (ferror(in) || fflush(out) != 0 ||
                fchmod(fd, src.st_mode & 0777) != 0) result = 0;
        }
        if (in != NULL) fclose(in);
        if (out != NULL && fclose(out) != 0) result = 0;
        if (result && rename(temp, to) != 0) result = 0;
        if (temp != NULL) remove(temp);
        free(temp);
    }
#endif

done:
    free(from);
    free(to);
    return result;
}

/**
 * Construct the native menu command for the installed GUI.
 * @param executable Absolute installed executable path.
 * @return Owned command, or NULL on failure.
 */
static char *install_command(const char *executable) {
    kc_buffer_t *buffer = NULL;
    const char *cursor;
    char *command = NULL;
    int status;

    status = kc_buffer_open(&buffer);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "\"");
    for (cursor = executable; status == KC_KAIMAN_OK && *cursor; cursor++) {
#ifndef _WIN32
        if (strchr("\\\"$", *cursor) != NULL || *cursor == 96) {
            status = kc_buffer_text(buffer, "\\");
        }
        if (status == KC_KAIMAN_OK && *cursor == '%') {
            status = kc_buffer_text(buffer, "%");
        }
#endif
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_append(buffer, cursor, 1U);
        }
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "\" gui");
    if (status == KC_KAIMAN_OK) {
        command = kc_util_strdup(kc_buffer_value(buffer));
    }
    kc_buffer_close(buffer);
    return command;
}

/**
 * Copy the runtime and register its native menu entry.
 * @param state Installer context.
 * @param directory Destination directory.
 * @param category Native menu category.
 * @return Error description, or NULL on success.
 */
static const char *install_runtime(kc_install_t *state, const char *directory,
    const char *category) {
    kc_menu_entry_t entry;
    char *assets;
    char *executable;
    char *icon;
    char *command;
    const char *error = NULL;
    size_t index;

    if (!install_absolute(directory)) return "Enter an absolute directory.";
    if (category == NULL || *category == '\0' ||
        strcmp(category, ".") == 0 || strcmp(category, "..") == 0 ||
        strpbrk(category, "<>:\"/\\|?*;\r\n") != NULL) {
        return "Enter a valid menu category.";
    }
    assets = install_join(directory, "gui");
    if (assets == NULL || !install_parents(assets)) {
        free(assets);
        return "Unable to create the installation directory.";
    }
    free(assets);
    for (index = 0U; index < sizeof(install_files) / sizeof(install_files[0]);
        index++) {
        if (!install_copy(state->source, directory, install_files[index])) {
            return install_files[index];
        }
    }
#ifdef _WIN32
    {
        static const char *const optional[] = {"libgcc_s_seh-1.dll",
            "libstdc++-6.dll", "libwinpthread-1.dll"};

        for (index = 0U; index < sizeof(optional) / sizeof(optional[0]);
            index++) {
            char *path = install_join(state->source, optional[index]);
            struct stat info;
            int exists = path != NULL && stat(path, &info) == 0;

            free(path);
            if (exists && !install_copy(state->source, directory,
                optional[index])) return optional[index];
        }
    }
#endif
    executable = install_join(directory, INSTALL_EXE);
    icon = install_join(directory, INSTALL_ICON);
    command = executable != NULL ? install_command(executable) : NULL;
    if (executable == NULL || icon == NULL || command == NULL) {
        error = "Unable to construct the menu entry.";
    } else {
        memset(&entry, 0, sizeof(entry));
        entry.id = "com.kaisarcode.kaiman";
        entry.name = "kaiman";
        entry.description = "AI in a Snap";
        entry.command = command;
        entry.icon = icon;
        entry.category = category;
        if (kc_menu_delete(entry.id) != KC_MENU_OK ||
            kc_menu_add(&entry) != KC_MENU_OK) {
            error = "Files copied, but menu registration failed.";
        }
    }
    free(executable);
    free(icon);
    free(command);
    return error;
}

/**
 * Select a directory using the native folder chooser.
 * @return Owned selected path, or NULL on cancellation.
 */
static char *install_browse(void) {
#ifdef _WIN32
    BROWSEINFOA info;
    LPITEMIDLIST item;
    char path[MAX_PATH];

    memset(&info, 0, sizeof(info));
    info.lpszTitle = "Installation directory";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    item = SHBrowseForFolderA(&info);
    if (item == NULL) return NULL;
    if (!SHGetPathFromIDListA(item, path)) path[0] = '\0';
    CoTaskMemFree(item);
    return path[0] != '\0' ? kc_util_strdup(path) : NULL;
#else
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Installation directory",
        NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Cancel",
        GTK_RESPONSE_CANCEL, "Select", GTK_RESPONSE_ACCEPT, NULL);
    char *path = NULL;

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *selected = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(dialog));

        path = kc_util_strdup(selected);
        g_free(selected);
    }
    gtk_widget_destroy(dialog);
    return path;
#endif
}

/**
 * Encode one bridge string result or null.
 * @param state Installer context.
 * @param value String to serialize, or NULL.
 * @return KC_WVW_OK on success, otherwise KC_WVW_ERROR.
 */
static int install_reply(kc_install_t *state, const char *value) {
    char *escaped = NULL;
    kc_buffer_t *buffer = NULL;
    int status = KC_KAIMAN_OK;

    free(state->response);
    state->response = NULL;
    if (value == NULL) {
        state->response = kc_util_strdup("null");
    } else {
        status = kc_json_escape(value, &escaped);
        if (status == KC_KAIMAN_OK) status = kc_buffer_open(&buffer);
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_format(buffer, "\"%s\"", escaped);
        }
        if (status == KC_KAIMAN_OK) {
            state->response = kc_util_strdup(kc_buffer_value(buffer));
        }
    }
    free(escaped);
    kc_buffer_close(buffer);
    return state->response != NULL ? KC_WVW_OK : KC_WVW_ERROR;
}

/**
 * Serve installer actions from the local WebView.
 * @param wvw Installer window.
 * @param method Bridge method.
 * @param params_json Serialized arguments.
 * @param out Receives the serialized response.
 * @param userdata Installer context.
 * @return KC_WVW_OK on success, otherwise KC_WVW_ERROR.
 */
static int install_bridge(kc_wvw_t *wvw, const char *method,
    const char *params_json, const char **out, void *userdata) {
    kc_install_t *state = (kc_install_t *)userdata;
    kc_json_value_t *params = NULL;
    const char *message = NULL;
    char *chosen = NULL;
    int result;

    if (strcmp(method, "directory") == 0) {
        message = state->directory;
    } else if (strcmp(method, "browse") == 0) {
        chosen = install_browse();
        message = chosen;
    } else if (strcmp(method, "close") == 0) {
        atomic_store(&state->stop, 1);
    } else if (strcmp(method, "ready") == 0) {
#ifndef _WIN32
        GdkDisplay *display = gdk_display_get_default();
        GdkMonitor *monitor = display != NULL ?
            gdk_display_get_primary_monitor(display) : NULL;
        GdkRectangle area;

        if (monitor != NULL) {
            gdk_monitor_get_workarea(monitor, &area);
            (void)kc_wvw_set_position(wvw,
                area.x + (area.width - INSTALL_WIDTH) / 2,
                area.y + (area.height - INSTALL_HEIGHT) / 2);
        }
#endif
        atomic_store(&state->ready, 1);
    } else if (strcmp(method, "install") == 0) {
        if (params_json == NULL || kc_json_parse(params_json,
            strlen(params_json), &params) != KC_KAIMAN_OK) {
            message = "Invalid installation parameters.";
        } else {
            message = install_runtime(state,
                kc_json_text(kc_json_member(params, "directory")),
                kc_json_text(kc_json_member(params, "category")));
        }
    } else {
        return KC_WVW_ERROR;
    }
    (void)wvw;
    result = install_reply(state, message);
    *out = state->response != NULL ? state->response : "null";
    free(chosen);
    kc_json_free(params);
    return result;
}

/**
 * Encode a local HTML asset as a file URL.
 * @param source Installer directory.
 * @return Owned URL, or NULL on failure.
 */
static char *install_url(const char *source) {
    char *path = install_join(source, "gui/install.html");
    kc_buffer_t *buffer = NULL;
    const unsigned char *cursor;
    char *url = NULL;
    int status;

    if (path == NULL) return NULL;
    status = kc_buffer_open(&buffer);
#ifdef _WIN32
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "file:///");
#else
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "file://");
#endif
    for (cursor = (const unsigned char *)path;
        status == KC_KAIMAN_OK && *cursor; cursor++) {
        unsigned char byte = *cursor == '\\' ? '/' : *cursor;

        if (isalnum(byte) || strchr("/:-._~", byte) != NULL) {
            status = kc_buffer_append(buffer, &byte, 1U);
        } else {
            status = kc_buffer_format(buffer, "%%%02X", (unsigned int)byte);
        }
    }
    if (status == KC_KAIMAN_OK) url = kc_util_strdup(kc_buffer_value(buffer));
    free(path);
    kc_buffer_close(buffer);
    return url;
}

/**
 * Open the borderless installer and process its lifetime.
 * @return 0 after success or cancellation, otherwise 1.
 */
int main(void) {
    static const char *const methods[] = {"directory", "browse", "install",
        "close", "ready"};
    kc_install_t state = {0};
    kc_wvw_options_t options = {0};
    kc_wvw_bridge_options_t bridge = {0};
    kc_wvw_t *wvw = NULL;
    char *url = NULL;
    char *icon = NULL;
    int width = INSTALL_WIDTH;
    int height = INSTALL_HEIGHT;
    int borderless = 1;
    int hidden = 1;
    double deadline;
    int hide_on_close = 1;
    int result = 1;
    const char *detail = "unable to resolve installer assets";
    const char *native_error;

    state.source = install_source();
    state.directory = install_default();
    if (state.source == NULL || state.directory == NULL) goto done;
    url = install_url(state.source);
    icon = install_join(state.source, INSTALL_ICON);
    if (url == NULL || icon == NULL) goto done;
    {
        static const char *const assets[] = {"gui/install.html",
            "gui/install.js", "gui/install.css", "gui/styles.css",
            "gui/logo-128.png", INSTALL_ICON};
        size_t index;

        for (index = 0U; index < sizeof(assets) / sizeof(assets[0]); index++) {
            char *path = install_join(state.source, assets[index]);
            struct stat info;
            int exists = path != NULL && stat(path, &info) == 0 &&
                S_ISREG(info.st_mode);

            free(path);
            if (!exists) {
                detail = assets[index];
                goto done;
            }
        }
    }
    options.url = url;
    options.title = "kaiman installer";
    options.icon = icon;
    options.width = &width;
    options.height = &height;
    options.borderless = &borderless;
    options.hidden = &hidden;
    options.background = "141715";
    options.hide_on_close = &hide_on_close;
    options.allow_file = 1;
    detail = "unable to open installer window";
#ifdef _WIN32
    {
        int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

        options.posx = &x;
        options.posy = &y;
        if (kc_wvw_open(&wvw, &options) != KC_WVW_OK) goto done;
    }
#else
    if (kc_wvw_open(&wvw, &options) != KC_WVW_OK) goto done;
#endif
    bridge.methods = methods;
    bridge.method_count = sizeof(methods) / sizeof(methods[0]);
    bridge.callback = install_bridge;
    bridge.userdata = &state;
    detail = "unable to initialize installer bridge";
    if (kc_wvw_set_bridge_methods(wvw, &bridge) != KC_WVW_OK) goto done;
    detail = "unable to load installer interface";
    if (kc_wvw_navigate(wvw, url) != KC_WVW_OK) goto done;
    detail = "installer interface did not initialize";
    deadline = kc_util_seconds() + 15.0;
    while (!atomic_load(&state.ready) && kc_util_seconds() < deadline) {
        kc_util_sleep(25);
    }
    if (!atomic_load(&state.ready)) goto done;
    detail = "unable to show installer window";
    if (kc_wvw_show(wvw) != KC_WVW_OK) goto done;
    while (!atomic_load(&state.stop) && kc_wvw_is_visible(wvw) == 1) {
        kc_util_sleep(50);
    }
    result = 0;

done:
    if (result != 0) {
        native_error = wvw != NULL ? kc_wvw_get_error(wvw) : NULL;
        fprintf(stderr, "kaiman installer: %s%s%s\n", detail,
            native_error != NULL ? ": " : "",
            native_error != NULL ? native_error : "");
    }
    kc_wvw_close(wvw);
    free(state.response);
    free(state.directory);
    free(state.source);
    free(url);
    free(icon);
    return result;
}
