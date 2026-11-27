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
#include "child.h"
#include "update.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <shlobj.h>
#else
#include <dirent.h>
#include <gtk/gtk.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define INSTALL_EXE "kaiman.exe"
#define UNINSTALL_EXE "uninstaller.exe"
#define INSTALL_EXT ".dll"
#define INSTALL_ICON "gui/tray-started.ico"
#else
#define INSTALL_EXE "kaiman"
#define UNINSTALL_EXE "uninstaller"
#define INSTALL_EXT ".so"
#define INSTALL_ICON "gui/tray-started.png"
#endif

#define INSTALL_WIDTH 460
#define INSTALL_HEIGHT 440

typedef struct {
    char *source;
    char *directory;
    char *registry;
    char *response;
    char *installed_version;
    const char *blocked;
    atomic_int stop;
    atomic_int ready;
    atomic_int remove_runtime;
    atomic_int apply_update;
    int uninstall;
    int update;
} kc_install_t;

static const char *const install_files[] = {
    INSTALL_EXE, "libkaiman" INSTALL_EXT, "libhttp" INSTALL_EXT,
    "libnetl" INSTALL_EXT, "libtls" INSTALL_EXT, "libwvw" INSTALL_EXT,
    "libtray" INSTALL_EXT, "gui/index.html", "gui/app.js", "gui/styles.css",
    "gui/logo-46.png", "gui/logo-92.png", "gui/logo-128.png", "gui/logo.ico",
    "gui/tray-started.png", "gui/tray-stopped.png", "gui/tray-started.ico",
    "gui/tray-stopped.ico", UNINSTALL_EXE, "libmenu" INSTALL_EXT,
    "gui/install.html", "gui/install.js", "gui/install.css"
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
 * Resolve the installation registry in the private state directory.
 * @return Owned registry path, or NULL on failure.
 */
static char *install_registry_path(void) {
    char *path = kc_util_pid_path();
    char *end;
    char *result;

    if (path == NULL) return NULL;
    end = strrchr(path, '/');
    if (end == NULL) {
        free(path);
        return NULL;
    }
    *end = '\0';
    result = install_join(path, "install.path");
    free(path);
    return result;
}

/**
 * Read the registered installation directory.
 * @param registry Registry file path.
 * @param out Receives an owned absolute directory when installed.
 * @return 1 when installed, 0 when absent, -1 when invalid.
 */
static int install_registry_read(const char *registry, char **out) {
    struct stat info;
    char *data;
    size_t size = 0U;

    *out = NULL;
    if (registry == NULL) return -1;
    if (stat(registry, &info) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(info.st_mode)) return -1;
    data = kc_util_read_file(registry, &size);
    if (data == NULL || strlen(data) != size || !install_absolute(data)) {
        free(data);
        return -1;
    }
    *out = data;
    return 1;
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
 * Reserve one installation directory without replacing an existing record.
 * @param registry Registry file path.
 * @param directory Absolute installation directory.
 * @return 1 on success, otherwise 0.
 */
static int install_registry_claim(const char *registry, const char *directory) {
    size_t size = strlen(directory);
    FILE *file;
    int fd;
    int ok;

#ifdef _WIN32
    fd = _open(registry, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
        _S_IREAD | _S_IWRITE);
    if (fd < 0) return 0;
    file = _fdopen(fd, "wb");
    if (file == NULL) _close(fd);
#else
    fd = open(registry, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) return 0;
    file = fdopen(fd, "wb");
    if (file == NULL) close(fd);
#endif
    ok = file != NULL && fwrite(directory, 1U, size, file) == size;
    if (file != NULL && fclose(file) != 0) ok = 0;
    if (!ok) remove(registry);
    return ok;
}

/**
 * Validate the platform menu category.
 * @param category Menu category identifier or Windows folder.
 * @return 1 when valid, otherwise 0.
 */
static int install_category_valid(const char *category) {
    if (category == NULL || *category == '\0' ||
        strcmp(category, ".") == 0 || strcmp(category, "..") == 0 ||
        strpbrk(category, "<>:\"/\\|?*;\r\n") != NULL) return 0;
#ifdef _WIN32
    return 1;
#else
    static const char *const categories[] = {
        "AudioVideo", "Development", "Education", "Game", "Graphics",
        "Network", "Office", "Science", "Settings", "System", "Utility"
    };
    size_t index;

    for (index = 0U; index < sizeof(categories) / sizeof(categories[0]);
        index++) {
        if (strcmp(category, categories[index]) == 0) return 1;
    }
    return 0;
#endif
}

/**
 * Copy the runtime and register its native menu entry.
 * @param state Installer context.
 * @param directory Destination directory.
 * @param category Native menu category.
 * @return Error description, or NULL on success.
 */
static const char *install_copy_runtime(kc_install_t *state, const char *directory,
    const char *category) {
    kc_menu_entry_t entry;
    char *assets;
    char *executable;
    char *icon;
    char *command;
    const char *error = NULL;
    size_t index;

    if (!install_absolute(directory)) return "Enter an absolute directory.";
    if (!install_category_valid(category)) {
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
    if (error == NULL) {
        char *marker = install_join(directory, "kaiman.install");

        if (marker == NULL || !kc_util_write_private(marker, "kaiman\n", 7U)) {
            error = "Unable to record installation.";
        }
        free(marker);
    }
    return error;
}

/**
 * Install only when no installation is registered.
 * @param state Installer context.
 * @param directory Destination directory.
 * @param category Native menu category.
 * @return Error message, or NULL on success.
 */
static const char *install_runtime(kc_install_t *state, const char *directory,
    const char *category) {
    const char *error;
    char *registered = NULL;
    int status;

    if (!install_absolute(directory)) return "Enter an absolute directory.";
    status = install_registry_read(state->registry, &registered);
    free(registered);
    if (status == 1) return "Kaiman is already installed.";
    if (status != 0) return "Unable to read the installation registry.";
    if (!install_registry_claim(state->registry, directory)) {
        return "Unable to reserve the installation registry.";
    }
    error = install_copy_runtime(state, directory, category);
    if (error != NULL) {
        if (remove(state->registry) != 0) {
            return "Installation failed and the registry could not be cleared.";
        }
    }
    return error;
}

/**
 * Resolve the parent directory of an owned file path.
 * @param path Owned path.
 * @return Owned parent path, or NULL on failure.
 */
static char *uninstall_parent(char *path) {
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
 * Validate the installation marker before removing any files.
 * @param directory Installed runtime directory.
 * @return 1 for a registered installation, otherwise 0.
 */
static int uninstall_registered(const char *directory) {
    char *path = install_join(directory, "kaiman.install");
    size_t size = 0U;
    char *data = path != NULL ? kc_util_read_file(path, &size) : NULL;
    int valid = data != NULL && size == 7U &&
        memcmp(data, "kaiman\n", 7U) == 0;

    free(data);
    free(path);
    return valid;
}

/**
 * Read a saved process identifier.
 * @param directory Private state directory.
 * @param name PID filename.
 * @return Process identifier or zero.
 */
static unsigned long long uninstall_pid(const char *directory,
    const char *name) {
    char *path = install_join(directory, name);
    size_t size = 0U;
    char *data = path != NULL ? kc_util_read_file(path, &size) : NULL;
    unsigned long long pid = 0ULL;
    char *end;

    if (data != NULL) {
        pid = strtoull(data, &end, 10);
        if (end == data) pid = 0ULL;
        while (*end && isspace((unsigned char)*end)) end++;
        if (*end != '\0') pid = 0ULL;
    }
    free(data);
    free(path);
    return pid;
}

/**
 * Wait for a process to exit before removing runtime files.
 * @param pid Process identifier.
 * @return 1 when stopped, otherwise 0.
 */
static int uninstall_wait(unsigned long long pid) {
    int index;

    for (index = 0; index < 300 && kc_util_pid_alive(pid); index++) {
        kc_util_sleep(100);
    }
    return !kc_util_pid_alive(pid);
}

/**
 * Request that Kaiman and its gateway stop.
 * @return Error message, or NULL on success.
 */
static const char *uninstall_stop(void) {
    char *directory = uninstall_parent(kc_util_pid_path());
    char *request;
    unsigned long long pid;
    int status;

    if (directory == NULL) return "Unable to resolve private state.";
    pid = uninstall_pid(directory, "kaiman-gui.pid");
    if (kc_util_pid_alive(pid)) {
        request = install_join(directory, "kaiman-gui.quit");
        if (request == NULL || !kc_util_write_private(request, "", 0U)) {
            free(request);
            free(directory);
            return "Unable to request application exit.";
        }
        status = uninstall_wait(pid);
        remove(request);
        free(request);
        if (!status) {
            free(directory);
            return "Kaiman is still running.";
        }
    }
    pid = uninstall_pid(directory, "kaiman.pid");
    free(directory);
    status = kc_kaiman_stop();
    if (status != KC_KAIMAN_OK && status != KC_KAIMAN_NOT_FOUND) {
        return "Unable to stop the gateway.";
    }
    if (!uninstall_wait(pid)) return "The gateway is still running.";
    return NULL;
}

/**
 * Remove a named file, accepting an already absent file.
 * @param directory Base directory.
 * @param name Relative filename.
 * @return 1 on success, otherwise 0.
 */
static int uninstall_file(const char *directory, const char *name) {
    char *path = install_join(directory, name);
    int ok = path != NULL && (remove(path) == 0 || errno == ENOENT);

    free(path);
    return ok;
}

/**
 * Remove an empty directory without deleting unrelated files.
 * @param path Directory path.
 * @return void
 */
static void uninstall_empty(const char *path) {
#ifdef _WIN32
    (void)_rmdir(path);
#else
    (void)rmdir(path);
#endif
}

/**
 * Remove saved local session references.
 * @param directory Private state directory.
 * @return 1 on success, otherwise 0.
 */
static int uninstall_sessions(const char *directory) {
    char *path = install_join(directory, "sessions");
    int ok = 1;

    if (path == NULL) return 0;
#ifdef _WIN32
    {
        char *pattern = install_join(path, "*.session");
        WIN32_FIND_DATAA entry;
        HANDLE search;

        if (pattern == NULL) {
            free(path);
            return 0;
        }
        search = FindFirstFileA(pattern, &entry);
        free(pattern);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    !uninstall_file(path, entry.cFileName)) ok = 0;
            } while (FindNextFileA(search, &entry));
            if (GetLastError() != ERROR_NO_MORE_FILES) ok = 0;
            FindClose(search);
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND &&
            GetLastError() != ERROR_PATH_NOT_FOUND) {
            ok = 0;
        }
    }
#else
    {
        DIR *dir = opendir(path);
        struct dirent *entry;

        if (dir != NULL) {
            while ((entry = readdir(dir)) != NULL) {
                size_t length = strlen(entry->d_name);

                if (length > 8U &&
                    strcmp(entry->d_name + length - 8U, ".session") == 0 &&
                    !uninstall_file(path, entry->d_name)) ok = 0;
            }
            closedir(dir);
        } else if (errno != ENOENT) {
            ok = 0;
        }
    }
#endif
    if (ok) uninstall_empty(path);
    free(path);
    return ok;
}

/**
 * Remove private configuration and system credentials.
 * @return Error message, or NULL on success.
 */
static const char *uninstall_data(void) {
    kc_kaiman_t *manager = NULL;
    kc_kaiman_service_t **services = NULL;
    size_t count;
    char *config;
    char *state;
    const char *error = NULL;

    if (kc_kaiman_open(&manager, NULL) != KC_KAIMAN_OK) {
        return "Unable to open saved configuration.";
    }
    config = uninstall_parent(kc_util_strdup(kc_kaiman_path(manager)));
    state = uninstall_parent(kc_util_pid_path());
    if (config == NULL || state == NULL) {
        error = "Unable to resolve private data.";
        goto done;
    }
    for (;;) {
        if (kc_kaiman_get_services(manager, &services, &count) != KC_KAIMAN_OK) {
            error = "Unable to read saved services.";
            goto done;
        }
        if (count == 0U) break;
        if (kc_kaiman_service_remove(services[0]) != KC_KAIMAN_OK) {
            error = "Unable to remove a saved service or credential.";
            goto done;
        }
    }
    if (!uninstall_file(config, "services.ini") ||
        !uninstall_file(config, "gui.theme") ||
        !uninstall_sessions(state) ||
        !uninstall_file(state, "kaiman.pid") ||
        !uninstall_file(state, "kaiman-gui.pid") ||
        !uninstall_file(state, "kaiman-gui.focus") ||
        !uninstall_file(state, "kaiman-gui.quit")) {
        error = "Unable to remove private data.";
        goto done;
    }
    uninstall_empty(config);
    uninstall_empty(state);

done:
    kc_kaiman_close(manager);
    free(config);
    free(state);
    return error;
}

/**
 * Prepare removal following explicit confirmation in the window.
 * @param state Uninstaller context.
 * @param purge Whether to remove private application data.
 * @return Error message, or NULL on success.
 */
static const char *uninstall_prepare(kc_install_t *state, int purge) {
    const char *error;
    char *registered = NULL;
    int status = install_registry_read(state->registry, &registered);

    if (status != 1) {
        return status == 0 ? "Kaiman is not installed." :
            "Unable to read the installation registry.";
    }
    if (strcmp(state->directory, registered) != 0 ||
        !uninstall_registered(registered)) {
        free(registered);
        return "The registered installation is missing or has changed.";
    }
    free(registered);
    error = uninstall_stop();
    if (error != NULL) return error;
    if (purge) {
        error = uninstall_data();
        if (error != NULL) return error;
    }
    if (kc_menu_delete("com.kaisarcode.kaiman") != KC_MENU_OK) {
        return "Unable to remove the application menu entry.";
    }
    atomic_store(&state->remove_runtime, 1);
    return NULL;
}

#ifdef _WIN32

/**
 * Append one fixed runtime filename to the deferred cleanup command.
 * @param command Open command buffer.
 * @param name Relative runtime filename.
 * @return KC_KAIMAN_OK or an error status.
 */
static int uninstall_delete_command(kc_buffer_t *command, const char *name) {
    int status = kc_buffer_text(command, "del /F /Q \"!KC_KAIMAN_REMOVE!\\");
    const char *cursor;

    for (cursor = name; status == KC_KAIMAN_OK && *cursor; cursor++) {
        char ch = *cursor == '/' ? '\\' : *cursor;

        status = kc_buffer_append(command, &ch, 1U);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(command, "\" >nul 2>&1 & if exist ");
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(command, "\"!KC_KAIMAN_REMOVE!\\");
    }
    for (cursor = name; status == KC_KAIMAN_OK && *cursor; cursor++) {
        char ch = *cursor == '/' ? '\\' : *cursor;

        status = kc_buffer_append(command, &ch, 1U);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(command, "\" set KC_KAIMAN_PENDING=1 & ");
    }
    return status;
}

/**
 * Dispatch cleanup after the Windows process releases its executable.
 * @param directory Installed directory.
 * @param registry Installation registry path.
 * @return 1 when cleanup was launched, otherwise 0.
 */
static int uninstall_windows(const char *directory, const char *registry) {
    static const char *const optional[] = {"libgcc_s_seh-1.dll",
        "libstdc++-6.dll", "libwinpthread-1.dll"};
    char system[MAX_PATH];
    char *shell = NULL;
    char *line = NULL;
    kc_buffer_t *command = NULL;
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process = {0};
    size_t index;
    int status;
    int result = 0;

    if (GetSystemDirectoryA(system, sizeof(system)) == 0 ||
        !SetEnvironmentVariableA("KC_KAIMAN_REMOVE", directory) ||
        !SetEnvironmentVariableA("KC_KAIMAN_REGISTRY", registry)) return 0;
    shell = install_join(system, "cmd.exe");
    if (shell == NULL) goto done;
    status = kc_buffer_open(&command);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(command,
            "\"%s\" /D /V:ON /S /C \"for /L %%i in (1,1,30) do @("
            "set KC_KAIMAN_PENDING=0 & ",
            shell);
    }
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < sizeof(install_files) / sizeof(install_files[0]); index++) {
        status = uninstall_delete_command(command, install_files[index]);
    }
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < sizeof(optional) / sizeof(optional[0]); index++) {
        status = uninstall_delete_command(command, optional[index]);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(command,
            "if \"!KC_KAIMAN_PENDING!\"==\"0\" ("
            "del /F /Q \"!KC_KAIMAN_REMOVE!\\kaiman.install\" >nul 2>&1 & "
            "rd \"!KC_KAIMAN_REMOVE!\\gui\" >nul 2>&1 & "
            "rd \"!KC_KAIMAN_REMOVE!\" >nul 2>&1 & "
            "del /F /Q \"!KC_KAIMAN_REGISTRY!\" >nul 2>&1 & "
            "exit /b 0) & "
            "ping -n 2 127.0.0.1 >nul) & exit /b 1\"");
    }
    if (status != KC_KAIMAN_OK) goto done;
    line = kc_util_strdup(kc_buffer_value(command));
    if (line == NULL) goto done;
    startup.cb = sizeof(startup);
    if (CreateProcessA(shell, line, NULL, NULL, FALSE, CREATE_NO_WINDOW,
        NULL, system, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        result = 1;
    }

done:
    free(shell);
    free(line);
    kc_buffer_close(command);
    return result;
}

#endif

/**
 * Remove installed files after closing the uninstaller interface.
 * @param directory Installed runtime directory.
 * @param registry Installation registry path.
 * @return 1 on success or successful cleanup dispatch, otherwise 0.
 */
static int uninstall_runtime(const char *directory, const char *registry) {
#ifdef _WIN32
    return uninstall_windows(directory, registry);
#else
    size_t index;
    char *assets;
    int ok = 1;

    for (index = 0U; index < sizeof(install_files) / sizeof(install_files[0]);
        index++) {
        if (strcmp(install_files[index], UNINSTALL_EXE) == 0) continue;
        if (!uninstall_file(directory, install_files[index])) ok = 0;
    }
    if (!ok) return 0;
    if (!uninstall_file(directory, UNINSTALL_EXE) ||
        !uninstall_file(directory, "kaiman.install")) return 0;
    assets = install_join(directory, "gui");
    if (assets != NULL) uninstall_empty(assets);
    free(assets);
    uninstall_empty(directory);
    return remove(registry) == 0 || errno == ENOENT;
#endif
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

    if (strcmp(method, "mode") == 0) {
        message = state->uninstall ? "uninstall" :
            state->update ? "update" : "install";
    } else if (strcmp(method, "installedVersion") == 0) {
        message = state->installed_version;
    } else if (strcmp(method, "version") == 0) {
        message = kc_kaiman_version();
    } else if (strcmp(method, "platform") == 0) {
#ifdef _WIN32
        message = "windows";
#else
        message = "linux";
#endif
    } else if (strcmp(method, "blocked") == 0) {
        message = state->blocked;
    } else if (strcmp(method, "directory") == 0) {
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
    } else if (strcmp(method, "update") == 0 && state->update &&
        state->blocked == NULL) {
        atomic_store(&state->apply_update, 1);
    } else if (strcmp(method, "uninstall") == 0 && state->uninstall) {
        if (params_json == NULL || kc_json_parse(params_json,
            strlen(params_json), &params) != KC_KAIMAN_OK) {
            message = "Invalid removal parameters.";
        } else {
            message = uninstall_prepare(state,
                kc_json_boolean(kc_json_member(params, "purge")));
        }
    } else if (strcmp(method, "install") == 0 && !state->uninstall) {
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
 * Query the version of the installed executable before changing its runtime.
 * @param executable Absolute installed executable path.
 * @return Owned version string, or NULL on failure.
 */
static char *install_version(const char *executable) {
    kc_child_t *child = NULL;
    char *args[] = {(char *)executable, "--version", NULL};
    char *version = NULL;
    double deadline = kc_util_seconds() + 10.0;

    if (kc_child_open(&child, args) == KC_KAIMAN_OK) {
        if (kc_child_line(child, deadline, &version) != KC_KAIMAN_OK ||
            kc_child_wait(child, deadline) != KC_KAIMAN_OK) {
            free(version);
            version = NULL;
        }
    }
    kc_child_close(child);
    return version;
}

/**
 * Check whether a runtime file exists as a regular file.
 * @param directory Runtime directory.
 * @param name Relative filename.
 * @return 1 when present, otherwise 0.
 */
static int install_exists(const char *directory, const char *name) {
    char *path = install_join(directory, name);
    struct stat info;
    int result = path != NULL && stat(path, &info) == 0 &&
        S_ISREG(info.st_mode);

    free(path);
    return result;
}

/**
 * Copy an update while retaining the original files for rollback.
 * @param state Installer context.
 * @param backup Private backup directory.
 * @return 1 on success, 0 on safe failure, -1 when rollback fails.
 */
static int install_replace(kc_install_t *state, const char *backup) {
    const size_t required = sizeof(install_files) / sizeof(install_files[0]);
#ifdef _WIN32
    static const char *const optional[] = {"libgcc_s_seh-1.dll",
        "libstdc++-6.dll", "libwinpthread-1.dll"};
#endif
    const char *names[sizeof(install_files) / sizeof(install_files[0])
#ifdef _WIN32
        + sizeof(optional) / sizeof(optional[0])
#endif
    ];
    unsigned char present[sizeof(names) / sizeof(names[0])] = {0};
    char *assets = install_join(backup, "gui");
    size_t count = required;
    size_t index;
    int result = 0;

    if (assets == NULL || !install_parents(assets)) goto done;
    for (index = 0U; index < required; index++) {
        names[index] = install_files[index];
    }
#ifdef _WIN32
    for (index = 0U; index < sizeof(optional) / sizeof(optional[0]); index++) {
        if (install_exists(state->source, optional[index])) {
            names[count++] = optional[index];
        }
    }
#endif
    for (index = 0U; index < count; index++) {
        const char *name = names[index];

        if (!install_exists(state->source, name)) goto done;
        present[index] = (unsigned char)install_exists(state->directory, name);
        if (present[index] && !install_copy(state->directory, backup, name)) {
            goto done;
        }
    }
    for (index = 0U; index < count; index++) {
        if (!install_copy(state->source, state->directory, names[index])) break;
    }
    if (index == count) {
        result = 1;
    } else {
        size_t changed;

        for (changed = 0U; changed <= index; changed++) {
            const char *name = names[changed];

            if (present[changed]) {
                if (!install_copy(backup, state->directory, name)) result = -1;
            } else if (!uninstall_file(state->directory, name)) {
                result = -1;
            }
        }
    }

done:
    free(assets);
    return result;
}

/**
 * Display a native error when an update cannot restart the application.
 * @param message Error description.
 * @return void
 */
static void install_update_error(const char *message) {
#ifdef _WIN32
    (void)MessageBoxA(NULL, message, "kaiman update", MB_OK | MB_ICONERROR);
#else
    if (gtk_init_check(NULL, NULL)) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message);

        (void)gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    } else {
        fprintf(stderr, "kaiman update: %s\n", message);
    }
#endif
}

/**
 * Apply a staged or manually launched distribution to the installation.
 * @param state Installer context.
 * @param staged Whether the application downloaded the release.
 * @return 0 on success, otherwise 1.
 */
static int install_update(kc_install_t *state, int staged) {
    char *stage = kc_update_directory();
    char *expected_path = kc_update_path(stage, "version");
    char *result_path = kc_update_path(stage, "result");
    char *pid_path = kc_update_path(stage, "pid");
    char *backup = kc_update_path(stage, "backup");
    char *executable = install_join(state->directory, INSTALL_EXE);
    char *expected = NULL;
    char *version = NULL;
    char pid[32];
    size_t size = 0U;
    const char *message = "Unable to prepare the update.";
    const char *candidate = kc_kaiman_version();
    int result = 1;
    int changed = 0;
    int stopped = 0;
    int owned = staged;
    char *registered = NULL;

    if (expected_path == NULL || result_path == NULL || pid_path == NULL ||
        backup == NULL || executable == NULL) goto done;
    if (install_registry_read(state->registry, &registered) != 1 ||
        strcmp(registered, state->directory) != 0 ||
        !uninstall_registered(registered)) {
        message = "The registered installation is missing or has changed.";
        goto done;
    }
    if (!staged) {
#ifdef _WIN32
        owned = _mkdir(stage) == 0;
#else
        owned = mkdir(stage, 0700) == 0;
#endif
        if (!owned) {
            message = "An update is already staged.";
            goto done;
        }
    }
    (void)snprintf(pid, sizeof(pid), "%llu\n", kc_util_process_id());
    if (!kc_util_write_private(pid_path, pid, strlen(pid))) goto done;
    if (staged) expected = kc_util_read_file(expected_path, &size);
    version = install_version(executable);
    if (version == NULL || !kc_update_newer(candidate, version) ||
        (staged && (expected == NULL || strlen(expected) != size ||
        strcmp(expected[0] == 'v' ? expected + 1 : expected, candidate) != 0))) {
        message = "The downloaded version does not match the release.";
        goto done;
    }
    message = uninstall_stop();
    if (message != NULL) goto done;
    stopped = 1;
    changed = install_replace(state, backup);
    if (changed < 0) {
        message = "Update rollback failed. The backup has been retained.";
        goto done;
    }
    message = changed ? "Update complete." :
        "Update failed. The previous installation was preserved.";
    result = changed ? 0 : 1;

done:
    if (owned && changed >= 0 && result_path != NULL && message != NULL) {
        if (staged || stopped) {
            (void)kc_util_write_private(result_path, message, strlen(message));
        } else {
            (void)kc_update_remove(stage);
        }
    }
    if (stopped && changed >= 0) {
        if (!kc_update_launch(executable, "gui")) {
            install_update_error("Unable to restart Kaiman. Open it from the application menu.");
            result = 1;
        }
    } else if (message != NULL) {
        install_update_error(message);
    }
    free(registered);
    free(version);
    free(expected);
    free(executable);
    free(backup);
    free(pid_path);
    free(result_path);
    free(expected_path);
    free(stage);
    return result;
}

/**
 * Open the installer or apply a staged software update.
 * @param argc Argument count.
 * @param argv Argument values.
 * @return 0 after success or cancellation, otherwise 1.
 */
int main(int argc, char **argv) {
    static const char *const methods[] = {"mode", "platform", "directory",
        "browse", "blocked", "install", "uninstall", "update", "close",
        "ready", "installedVersion", "version"};
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
#ifdef KC_UNINSTALLER
    state.uninstall = 1;
#endif
    state.registry = install_registry_path();
    if (state.source == NULL || state.registry == NULL) goto done;
    if (!state.uninstall && argc == 2 && strcmp(argv[1], "--update") == 0) {
        if (!kc_update_staged()) {
            install_update_error("The update installer must run from its staging directory.");
        } else if (install_registry_read(state.registry, &state.directory) == 1 &&
            uninstall_registered(state.directory)) {
            result = install_update(&state, 1);
        } else {
            install_update_error("The registered installation is missing.");
        }
        free(state.directory);
        free(state.registry);
        free(state.source);
        return result;
    }
    {
        int registered = install_registry_read(state.registry, &state.directory);

        if (registered < 0) {
            state.blocked = "Unable to read the installation registry.";
        } else if (state.uninstall && registered == 0) {
            state.blocked = "Kaiman is not installed.";
        } else if (!state.uninstall && registered == 1) {
            char *path = install_join(state.directory, INSTALL_EXE);
            char *version = path != NULL && uninstall_registered(
                state.directory) ? install_version(path) : NULL;

            if (version != NULL &&
                kc_update_newer(kc_kaiman_version(), version)) {
                state.update = 1;
                state.installed_version = version;
                version = NULL;
            } else {
                state.blocked = "Kaiman is already installed.";
            }
            free(version);
            free(path);
        } else if (state.uninstall &&
            !uninstall_registered(state.directory)) {
            state.blocked = "The registered installation is missing.";
        }
        if (state.directory == NULL) {
            state.directory = state.uninstall ?
                kc_util_strdup("") : install_default();
        }
    }
    if (state.directory == NULL) goto done;
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
    options.title = state.uninstall ? "kaiman uninstaller" :
        state.update ? "kaiman update" : "kaiman installer";
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
    if (atomic_load(&state.apply_update)) {
        result = install_update(&state, 0);
    }
    if (atomic_load(&state.remove_runtime) &&
        !uninstall_runtime(state.directory, state.registry)) {
        fprintf(stderr, "kaiman: unable to remove installed files\n");
        result = 1;
    }
    free(state.response);
    free(state.installed_version);
    free(state.directory);
    free(state.registry);
    free(state.source);
    free(url);
    free(icon);
    return result;
}
