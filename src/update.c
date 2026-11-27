/**
 * update.c - Desktop software updates.
 * Summary: Discovers GitHub releases and verifies staged distributions.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "update.h"
#include "libkaiman.h"
#include "json.h"
#include "upstream.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <bcrypt.h>
#include <direct.h>
#else
#include <dirent.h>
#include <gio/gio.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define UPDATE_MAX_SIZE (512U * 1024U * 1024U)
#ifdef _WIN32
#define UPDATE_EXE "kaiman.exe"
#define UPDATE_INSTALLER "installer.exe"
#define UPDATE_ASSET "kaiman-windows-x86_64.zip"
#else
#define UPDATE_EXE "kaiman"
#define UPDATE_INSTALLER "installer"
#define UPDATE_ASSET "kaiman-linux-x86_64.zip"
#endif

/**
 * Parse a numeric version into three components.
 * @param text Version text, optionally prefixed with v.
 * @param parts Destination components.
 * @return 1 on success, otherwise 0.
 */
static int update_version(const char *text, unsigned long parts[3]) {
    char *end;
    size_t index;

    if (text == NULL) return 0;
    if (*text == 'v') text++;
    for (index = 0U; index < 3U; index++) {
        unsigned long long value;

        if (!isdigit((unsigned char)*text)) return 0;
        errno = 0;
        value = strtoull(text, &end, 10);
        if (errno != 0 || value > UINT_MAX) return 0;
        parts[index] = (unsigned long)value;
        if (index == 2U) return *end == '\0';
        if (*end != '.') return 0;
        text = end + 1;
    }
    return 0;
}

/**
 * Compare numeric release versions, with an optional v prefix.
 * @param left First version.
 * @param right Second version.
 * @return 1 when left is newer, otherwise 0.
 */
int kc_update_newer(const char *left, const char *right) {
    unsigned long a[3];
    unsigned long b[3];
    size_t index;

    if (!update_version(left, a) || !update_version(right, b)) return 0;
    for (index = 0U; index < 3U; index++) {
        if (a[index] != b[index]) return a[index] > b[index];
    }
    return 0;
}

/**
 * Join an update directory and relative filename.
 * @param directory Base directory.
 * @param name Relative filename.
 * @return Owned path, or NULL on failure.
 */
char *kc_update_path(const char *directory, const char *name) {
    char *path;
    size_t size;

    if (directory == NULL || name == NULL) return NULL;
    size = strlen(directory) + strlen(name) + 2U;
    path = (char *)malloc(size);
    if (path != NULL) (void)snprintf(path, size, "%s/%s", directory, name);
    return path;
}

/**
 * Resolve the parent of the private gateway PID file.
 * @return Owned directory, or NULL on failure.
 */
static char *update_state(void) {
    char *path = kc_util_pid_path();
    char *end = path != NULL ? strrchr(path, '/') : NULL;

    if (end == NULL) {
        free(path);
        return NULL;
    }
    *end = '\0';
    return path;
}

/**
 * Resolve the private update staging directory.
 * @return Owned path, or NULL on failure.
 */
char *kc_update_directory(void) {
    char *state = update_state();
    char *path = kc_update_path(state, "update");

    free(state);
    return path;
}

/**
 * Determine whether two absolute paths identify the same executable.
 * @param left First path.
 * @param right Second path.
 * @return 1 on match, otherwise 0.
 */
static int update_same(const char *left, const char *right) {
#ifdef _WIN32
    char a[32768];
    char b[32768];
    DWORD na = GetFullPathNameA(left, sizeof(a), a, NULL);
    DWORD nb = GetFullPathNameA(right, sizeof(b), b, NULL);

    return na != 0 && nb != 0 && na < sizeof(a) && nb < sizeof(b) &&
        _stricmp(a, b) == 0;
#else
    struct stat a;
    struct stat b;

    return stat(left, &a) == 0 && stat(right, &b) == 0 &&
        a.st_dev == b.st_dev && a.st_ino == b.st_ino;
#endif
}

/**
 * Verify the current installer belongs to the staging directory.
 * @return 1 when running from the staged release.
 */
int kc_update_staged(void) {
    char *stage = kc_update_directory();
    char *expected = kc_update_path(stage, "kaiman/" UPDATE_INSTALLER);
    char *current = kc_util_executable_path();
    int result = expected != NULL && current != NULL &&
        update_same(expected, current);

    free(current);
    free(expected);
    free(stage);
    return result;
}

/**
 * Resolve the registered installation for the current executable.
 * @return Owned directory, or NULL when running a portable build.
 */
char *kc_update_installed(void) {
    char *state = update_state();
    char *registry = kc_update_path(state, "install.path");
    char *directory = NULL;
    char *executable = kc_util_executable_path();
    char *installed = NULL;
    char *marker = NULL;
    char *contents = NULL;
    size_t length = 0U;
    size_t marker_size = 0U;

    if (registry != NULL) directory = kc_util_read_file(registry, &length);
    if (directory != NULL && strlen(directory) == length && length > 0U) {
        installed = kc_update_path(directory, UPDATE_EXE);
        marker = kc_update_path(directory, "kaiman.install");
        if (marker != NULL) contents = kc_util_read_file(marker, &marker_size);
    }
    if (installed == NULL || executable == NULL || contents == NULL ||
        marker_size != 7U || memcmp(contents, "kaiman\n", 7U) != 0 ||
        !update_same(executable, installed)) {
        free(directory);
        directory = NULL;
    }
    free(contents);
    free(marker);
    free(installed);
    free(executable);
    free(registry);
    free(state);
    return directory;
}

/**
 * Read a string member of a release metadata object.
 * @param object Borrowed JSON object.
 * @param name Member name.
 * @return Borrowed string, or NULL.
 */
static const char *update_text(kc_json_value_t *object, const char *name) {
    return kc_json_text(kc_json_member(object, name));
}

/**
 * Validate the SHA-256 digest published for a GitHub asset.
 * @param digest Digest text including its algorithm prefix.
 * @return 1 when supported, otherwise 0.
 */
static int update_digest(const char *digest) {
    size_t index;

    if (digest == NULL || strlen(digest) != 71U ||
        strncmp(digest, "sha256:", 7U) != 0) return 0;
    for (index = 7U; index < 71U; index++) {
        if (!isxdigit((unsigned char)digest[index])) return 0;
    }
    return 1;
}

/**
 * Select a compatible release with its platform-specific asset.
 * @param update Destination release metadata.
 * @param release Borrowed release object.
 * @return void
 */
static void update_select(kc_update_t *update, kc_json_value_t *release) {
    const char *tag = update_text(release, "tag_name");
    const char *notes = update_text(release, "body");
    kc_json_value_t *assets = kc_json_member(release, "assets");
    size_t index;

    if (tag == NULL || strlen(tag) >= sizeof(update->version) ||
        kc_json_boolean(kc_json_member(release, "draft")) ||
        !kc_update_newer(tag, kc_kaiman_version()) ||
        (update->version[0] != '\0' &&
        !kc_update_newer(tag, update->version))) return;
    for (index = 0U; index < kc_json_length(assets); index++) {
        kc_json_value_t *asset = kc_json_item(assets, index);
        const char *name = update_text(asset, "name");
        const char *digest = update_text(asset, "digest");
        const char *url = update_text(asset, "browser_download_url");
        double size = kc_json_number(kc_json_member(asset, "size"));
        char expected[512];

        if (name == NULL || strcmp(name, UPDATE_ASSET) != 0 ||
            !update_digest(digest) || size <= 0.0 ||
            size > (double)UPDATE_MAX_SIZE || size != (double)(size_t)size) {
            continue;
        }
        (void)snprintf(expected, sizeof(expected),
            "https://github.com/kaisarcode/kaiman/releases/download/%s/%s",
            tag, UPDATE_ASSET);
        if (url == NULL || strcmp(url, expected) != 0) continue;
        (void)snprintf(update->version, sizeof(update->version), "%s", tag);
        (void)snprintf(update->url, sizeof(update->url), "%s", url);
        (void)snprintf(update->digest, sizeof(update->digest), "%s", digest + 7);
        (void)snprintf(update->notes, sizeof(update->notes), "%s",
            notes != NULL ? notes : "");
        update->size = (size_t)size;
        return;
    }
}

/**
 * Discover the highest compatible release containing a verified asset.
 * @param update Receives release metadata.
 * @return 1 when available, 0 when current, -1 on failure.
 */
int kc_update_check(kc_update_t *update) {
    static const kc_upstream_field_t headers[] = {
        {"Accept", "application/vnd.github+json"}
    };
    kc_upstream_request_t request = {0};
    kc_upstream_response_t response = {0};
    kc_json_value_t *root = NULL;
    char *installed = kc_update_installed();
    size_t index;
    int result = -1;

    if (installed == NULL) return 0;
    free(installed);
    request.url = "https://api.github.com/repos/kaisarcode/kaiman/releases?per_page=100";
    request.headers = headers;
    request.header_count = sizeof(headers) / sizeof(headers[0]);
    request.timeout = 15;
    if (kc_upstream_send(&request, &response) != KC_KAIMAN_OK ||
        response.status != 200 ||
        kc_json_parse(response.body, response.body_size, &root) != KC_KAIMAN_OK ||
        kc_json_type(root) != KC_JSON_ARRAY) {
        update->error = "Unable to check for updates.";
        goto done;
    }
    for (index = 0U; index < kc_json_length(root); index++) {
        update_select(update, kc_json_item(root, index));
    }
    result = update->version[0] != '\0';

done:
    kc_json_free(root);
    kc_upstream_response_free(&response);
    return result;
}

typedef struct {
    kc_update_t *update;
    FILE *file;
    int status;
} kc_download_t;

/**
 * Record the status before receiving the response body.
 * @param status HTTP response status.
 * @param content_type Response content type, unused.
 * @param userdata Download context.
 * @return void
 */
static void update_download_status(int status, const char *content_type,
    void *userdata) {
    kc_download_t *download = (kc_download_t *)userdata;

    (void)content_type;
    download->status = status;
}

/**
 * Stream release bytes into the private archive.
 * @param data Borrowed response bytes.
 * @param size Byte count.
 * @param userdata Download context.
 * @return 0 on success, -1 on failure.
 */
static int update_download_chunk(const void *data, size_t size, void *userdata) {
    kc_download_t *download = (kc_download_t *)userdata;
    size_t received = atomic_load(&download->update->received);

    if (download->status != 200) return 0;
    if (received > download->update->size ||
        size > download->update->size - received ||
        fwrite(data, 1U, size, download->file) != size) return -1;
    atomic_store(&download->update->received, received + size);
    return 0;
}

/**
 * Accept HTTPS redirects only to GitHub release asset hosts.
 * @param url Redirect target.
 * @return 1 when permitted, otherwise 0.
 */
static int update_url(const char *url) {
    static const char *const hosts[] = {
        "https://github.com/",
        "https://release-assets.githubusercontent.com/",
        "https://objects.githubusercontent.com/"
    };
    size_t index;

    if (url == NULL || strpbrk(url, "\r\n") != NULL) return 0;
    for (index = 0U; index < sizeof(hosts) / sizeof(hosts[0]); index++) {
        if (strncmp(url, hosts[index], strlen(hosts[index])) == 0) return 1;
    }
    return 0;
}

/**
 * Download the selected asset, following a bounded number of redirects.
 * @param update Release and progress context.
 * @param path Destination archive.
 * @return 1 on success, otherwise 0.
 */
static int update_fetch(kc_update_t *update, const char *path) {
    kc_download_t download = {0};
    kc_upstream_request_t request = {0};
    char *url = kc_util_strdup(update->url);
    int index;
    int result = 0;

    download.update = update;
    download.file = fopen(path, "wb");
    if (download.file == NULL) {
        free(url);
        return 0;
    }
    request.timeout = 300;
    request.stream_only = 1;
    request.chunk = update_download_chunk;
    request.status = update_download_status;
    request.userdata = &download;
    for (index = 0; index < 5 && update_url(url); index++) {
        kc_upstream_response_t response = {0};
        char *next = NULL;
        int status;

        request.url = url;
        status = kc_upstream_send(&request, &response);
        if (status == KC_KAIMAN_OK && response.status >= 300 &&
            response.status < 400 && update_url(response.location)) {
            next = kc_util_strdup(response.location);
        }
        if (status == KC_KAIMAN_OK && response.status == 200 &&
            atomic_load(&update->received) == update->size) result = 1;
        kc_upstream_response_free(&response);
        free(url);
        url = next;
        if (result || url == NULL) break;
    }
    free(url);
    if (fclose(download.file) != 0) result = 0;
    return result;
}

/**
 * Calculate the SHA-256 digest of a downloaded archive.
 * @param path Archive path.
 * @param hex Receives 64 hexadecimal characters and a terminator.
 * @return 1 on success, otherwise 0.
 */
static int update_hash(const char *path, char hex[65]) {
    FILE *file = fopen(path, "rb");
    unsigned char buffer[32768];
    unsigned char digest[32];
    size_t count;
    size_t index;
    int ok = 0;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;

    if (file != NULL && BCryptOpenAlgorithmProvider(&algorithm,
        BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) == 0) {
        ok = 1;
        while ((count = fread(buffer, 1U, sizeof(buffer), file)) != 0U) {
            if (BCryptHashData(hash, buffer, (ULONG)count, 0) != 0) {
                ok = 0;
                break;
            }
        }
        if (ferror(file) || BCryptFinishHash(hash, digest,
            sizeof(digest), 0) != 0) ok = 0;
    }
    if (hash != NULL) BCryptDestroyHash(hash);
    if (algorithm != NULL) BCryptCloseAlgorithmProvider(algorithm, 0);
#else
    GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
    gsize length = sizeof(digest);

    if (file != NULL && hash != NULL) {
        while ((count = fread(buffer, 1U, sizeof(buffer), file)) != 0U) {
            g_checksum_update(hash, buffer, count);
        }
        g_checksum_get_digest(hash, digest, &length);
        ok = !ferror(file) && length == sizeof(digest);
    }
    if (hash != NULL) g_checksum_free(hash);
#endif
    if (file != NULL) fclose(file);
    if (ok) {
        for (index = 0U; index < sizeof(digest); index++) {
            (void)snprintf(hex + index * 2U, 3U, "%02x",
                (unsigned int)digest[index]);
        }
    }
    return ok;
}

/**
 * Read a little-endian ZIP field.
 * @param bytes First byte of the field.
 * @param count Field size, at most four bytes.
 * @return Decoded unsigned integer.
 */
static uint32_t update_le(const unsigned char *bytes, size_t count) {
    uint32_t result = 0U;
    size_t index;

    for (index = 0U; index < count; index++) {
        result |= (uint32_t)bytes[index] << (index * 8U);
    }
    return result;
}

/**
 * Create a private staging directory without accepting an existing one.
 * @param path Directory path.
 * @return 1 on success, otherwise 0.
 */
static int update_mkdir(const char *path) {
#ifdef _WIN32
    return CreateDirectoryA(path, NULL) != 0;
#else
    return mkdir(path, 0700) == 0;
#endif
}

/**
 * Check that an archive entry is a safe relative package path.
 * @param name Entry name, beginning with kaiman/.
 * @return 1 when valid, otherwise 0.
 */
static int update_entry(const char *name) {
    const char *part;
    const char *cursor;

    if (strncmp(name, "kaiman/", 7U) != 0) return 0;
    part = name;
    for (cursor = name; ; cursor++) {
        unsigned char ch = (unsigned char)*cursor;

        if (ch == '/' || ch == '\0') {
            size_t size = (size_t)(cursor - part);

            if (size == 0U) return ch == '\0' && cursor > name &&
                cursor[-1] == '/';
            if (part[0] == '.' || part[size - 1U] == '.') return 0;
            if (ch == '\0') return 1;
            part = cursor + 1;
        } else if (!((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.')) {
            return 0;
        }
    }
}

/**
 * Write one stored ZIP file into a freshly created staging tree.
 * @param file Open archive, positioned at the file contents.
 * @param path Destination path.
 * @param size Entry byte count.
 * @return 1 on success, otherwise 0.
 */
static int update_extract_file(FILE *file, const char *path, uint32_t size) {
    unsigned char bytes[32768];
    struct stat info;
    FILE *out;
    int ok = 1;

    if (stat(path, &info) == 0) return 0;
    out = fopen(path, "wb");
    if (out == NULL) return 0;
    while (size != 0U) {
        size_t count = size > sizeof(bytes) ? sizeof(bytes) : (size_t)size;

        if (fread(bytes, 1U, count, file) != count ||
            fwrite(bytes, 1U, count, out) != count) {
            ok = 0;
            break;
        }
        size -= (uint32_t)count;
    }
    if (fclose(out) != 0) ok = 0;
    return ok;
}

/**
 * Extract stored ZIP entries, rejecting unsupported formats and unsafe paths.
 * @param archive Verified distribution archive.
 * @param stage Private staging directory.
 * @return 1 on success, otherwise 0.
 */
static int update_extract(const char *archive, const char *stage) {
    FILE *file = fopen(archive, "rb");
    unsigned char header[30];
    char name[1024];
    int ok = 0;
    size_t entries = 0U;

    if (file == NULL) return 0;
    while (fread(header, 1U, 4U, file) == 4U) {
        uint32_t signature = update_le(header, 4U);
        uint32_t size;
        uint32_t length;
        uint32_t extra;
        char *path;
        int result;

        if (signature == 0x02014b50U && entries > 0U) {
            ok = 1;
            break;
        }
        if (signature != 0x04034b50U ||
            fread(header + 4U, 1U, 26U, file) != 26U ||
            (update_le(header + 6U, 2U) & ~0x0800U) != 0U ||
            update_le(header + 8U, 2U) != 0U ||
            ++entries > 4096U) break;
        size = update_le(header + 18U, 4U);
        length = update_le(header + 26U, 2U);
        extra = update_le(header + 28U, 2U);
        if (size != update_le(header + 22U, 4U) ||
            size > UPDATE_MAX_SIZE || length == 0U ||
            length >= sizeof(name) ||
            fread(name, 1U, length, file) != length) break;
        name[length] = '\0';
        if (strlen(name) != length || !update_entry(name) ||
            fseek(file, (long)extra, SEEK_CUR) != 0) break;
        path = kc_update_path(stage, name);
        if (path == NULL) break;
        if (name[length - 1U] == '/') {
            path[strlen(path) - 1U] = '\0';
            result = size == 0U && update_mkdir(path);
        } else {
            result = update_extract_file(file, path, size);
#ifndef _WIN32
            if (result && (strcmp(name, "kaiman/kaiman") == 0 ||
                strcmp(name, "kaiman/installer") == 0 ||
                strcmp(name, "kaiman/uninstaller") == 0)) {
                result = chmod(path, 0755) == 0;
            }
#endif
        }
        free(path);
        if (!result) break;
    }
    fclose(file);
    return ok;
}

/**
 * Launch an executable with a single fixed argument.
 * @param executable Absolute executable path.
 * @param argument Argument passed to the executable.
 * @param pid Optional installer process identifier destination.
 * @return 1 on success, otherwise 0.
 */
static int update_launch(const char *executable, const char *argument,
    unsigned long long *pid) {
#ifdef _WIN32
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process = {0};
    kc_buffer_t *command = NULL;
    int result = 0;

    startup.cb = sizeof(startup);
    if (kc_buffer_open(&command) == KC_KAIMAN_OK &&
        kc_buffer_format(command, "\"%s\" %s", executable, argument) ==
        KC_KAIMAN_OK) {
        result = CreateProcessA(executable, command->data, NULL, NULL,
            FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process) != 0;
        if (result) {
            if (pid != NULL) *pid = (unsigned long long)process.dwProcessId;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }
    kc_buffer_close(command);
    return result;
#else
    char *argv[] = {(char *)executable, (char *)argument, NULL};
    GPid child = 0;
    GSpawnFlags flags = pid != NULL ? G_SPAWN_DO_NOT_REAP_CHILD : 0;
    int result = g_spawn_async(NULL, argv, NULL, flags, NULL, NULL,
        pid != NULL ? &child : NULL, NULL) != 0;

    if (result && pid != NULL) *pid = (unsigned long long)child;
    return result;
#endif
}

/**
 * Launch an executable with a single fixed argument.
 * @param executable Absolute executable path.
 * @param argument Argument passed to the executable.
 * @return 1 on success, otherwise 0.
 */
int kc_update_launch(const char *executable, const char *argument) {
    return update_launch(executable, argument, NULL);
}

/**
 * Check whether the staged installer exited while the GUI is running.
 * @param update Active update context.
 * @return 1 when exited, otherwise 0.
 */
int kc_update_exited(kc_update_t *update) {
    if (update->installer_pid == 0ULL) return 1;
#ifdef _WIN32
    if (kc_util_pid_alive(update->installer_pid)) return 0;
#else
    {
        int status;
        pid_t pid = (pid_t)update->installer_pid;
        pid_t result = waitpid(pid, &status, WNOHANG);

        if (result == 0 || (result < 0 && errno == EINTR)) return 0;
        if (result < 0 && kc_util_pid_alive(update->installer_pid)) return 0;
        g_spawn_close_pid((GPid)pid);
    }
#endif
    update->installer_pid = 0ULL;
    return 1;
}

/**
 * Remove the owned staging tree without following symbolic links.
 * @param directory Staging directory.
 * @return 1 on success, otherwise 0.
 */
int kc_update_remove(const char *directory) {
    int result = 1;
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(directory);
    WIN32_FIND_DATAA entry;
    HANDLE search;
    char *pattern;

    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ||
            GetLastError() == ERROR_PATH_NOT_FOUND;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return DeleteFileA(directory) != 0;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return RemoveDirectoryA(directory) != 0;
    }
    pattern = kc_update_path(directory, "*");
    if (pattern == NULL) return 0;
    search = FindFirstFileA(pattern, &entry);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do {
        char *path;

        if (strcmp(entry.cFileName, ".") == 0 ||
            strcmp(entry.cFileName, "..") == 0) continue;
        path = kc_update_path(directory, entry.cFileName);
        if (path == NULL || !kc_update_remove(path)) result = 0;
        free(path);
    } while (FindNextFileA(search, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) result = 0;
    FindClose(search);
    return result && RemoveDirectoryA(directory) != 0;
#else
    struct stat info;
    DIR *dir;
    struct dirent *entry;

    if (lstat(directory, &info) != 0) return errno == ENOENT;
    if (!S_ISDIR(info.st_mode)) return unlink(directory) == 0;
    dir = opendir(directory);
    if (dir == NULL) return 0;
    while ((entry = readdir(dir)) != NULL) {
        char *path;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        path = kc_update_path(directory, entry->d_name);
        if (path == NULL || !kc_update_remove(path)) result = 0;
        free(path);
    }
    closedir(dir);
    return result && rmdir(directory) == 0;
#endif
}

/**
 * Download, verify and extract the selected release, then launch its installer.
 * @param update Selected release and progress destination.
 * @return 1 when handed off, otherwise 0.
 */
int kc_update_download(kc_update_t *update) {
    char *stage = kc_update_directory();
    char *archive = NULL;
    char *installer = NULL;
    char *expected = NULL;
    char digest[65];
    size_t index;
    int owned = 0;
    int result = 0;

    atomic_store(&update->received, 0U);
    update->error = "Unable to prepare the update directory.";
    if (stage == NULL || !update_mkdir(stage)) goto done;
    owned = 1;
    archive = kc_update_path(stage, "release.zip");
    installer = kc_update_path(stage, "kaiman/" UPDATE_INSTALLER);
    expected = kc_update_path(stage, "version");
    if (archive == NULL || installer == NULL || expected == NULL) goto done;
    update->error = "Unable to download the release.";
    if (!update_fetch(update, archive)) goto done;
    update->error = "Release checksum verification failed.";
    if (!update_hash(archive, digest)) goto done;
    for (index = 0U; index < 64U; index++) {
        if (digest[index] != tolower((unsigned char)update->digest[index])) {
            goto done;
        }
    }
    update->error = "Invalid or unsupported distribution archive.";
    if (!update_extract(archive, stage)) goto done;
    update->error = "Unable to launch the update installer.";
    if (!kc_util_write_private(expected, update->version,
        strlen(update->version)) || !update_launch(installer, "--update",
            &update->installer_pid)) {
        goto done;
    }
    update->error = NULL;
    result = 1;

done:
    if (!result && owned) (void)kc_update_remove(stage);
    free(expected);
    free(installer);
    free(archive);
    free(stage);
    return result;
}

/**
 * Collect the result of a completed update and clean its staging tree.
 * @return Owned message, or NULL when no completed update exists.
 */
char *kc_update_collect(void) {
    char *stage = kc_update_directory();
    char *path = kc_update_path(stage, "result");
    char *pid_path = kc_update_path(stage, "pid");
    char *message = NULL;
    char *pid_text = NULL;
    size_t size = 0U;
    unsigned long long pid = 0ULL;
    int index;

    if (path != NULL) message = kc_util_read_file(path, &size);
    if (message == NULL || strlen(message) != size) {
        free(message);
        message = NULL;
        goto done;
    }
    if (pid_path != NULL) pid_text = kc_util_read_file(pid_path, &size);
    if (pid_text != NULL) pid = strtoull(pid_text, NULL, 10);
    for (index = 0; index < 50 && kc_util_pid_alive(pid); index++) {
        kc_util_sleep(100);
    }
    if (!kc_util_pid_alive(pid)) (void)kc_update_remove(stage);

done:
    free(pid_text);
    free(pid_path);
    free(path);
    free(stage);
    return message;
}
