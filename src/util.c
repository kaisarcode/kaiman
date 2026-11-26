/**
 * util.c - Small shared helpers for kaiman.
 * Summary: Implements buffers, encoding, timing and private file helpers.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "util.h"

#include "libkaiman.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#define UTIL_BASE64_TABLE \
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

/**
 * Create each parent directory of a file path.
 * @param path File path.
 * @return 1 on success, 0 on failure.
 */
static int util_parent(const char *path) {
    char *copy = kc_util_strdup(path);
    char *cursor;

    if (copy == NULL) return 0;
    for (cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/' && *cursor != '\\') continue;
        *cursor = '\0';
#ifdef _WIN32
        if (_mkdir(copy) != 0 && errno != EEXIST) {
#else
        if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
#endif
            free(copy);
            return 0;
        }
        *cursor = '/';
    }
    free(copy);
    return 1;
}

/**
 * Open an empty growable byte buffer.
 * @param out Receives the buffer, or NULL on failure.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_buffer_open(kc_buffer_t **out) {
    kc_buffer_t *buffer;

    if (out == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    buffer = (kc_buffer_t *)calloc(1, sizeof(*buffer));
    if (buffer == NULL) return KC_KAIMAN_ENOMEM;
    *out = buffer;
    return KC_KAIMAN_OK;
}

/**
 * Append bytes to one growable buffer.
 * @param buffer Open buffer.
 * @param data Bytes to append.
 * @param size Byte count.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_buffer_append(kc_buffer_t *buffer, const void *data, size_t size) {
    char *larger;
    size_t needed;
    size_t capacity;

    if (buffer == NULL || (data == NULL && size != 0U)) {
        return KC_KAIMAN_EINVAL;
    }
    if (size > (size_t)-1 - buffer->size - 1U) return KC_KAIMAN_ENOMEM;
    needed = buffer->size + size + 1U;
    if (needed <= buffer->capacity) {
        memcpy(buffer->data + buffer->size, data, size);
        buffer->size += size;
        buffer->data[buffer->size] = '\0';
        return KC_KAIMAN_OK;
    }
    capacity = buffer->capacity == 0U ? 64U : buffer->capacity;
    while (capacity < needed) {
        if (capacity > (size_t)-1 / 2U) {
            capacity = needed;
            break;
        }
        capacity *= 2U;
    }
    larger = (char *)realloc(buffer->data, capacity);
    if (larger == NULL) return KC_KAIMAN_ENOMEM;
    buffer->data = larger;
    buffer->capacity = capacity;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return KC_KAIMAN_OK;
}

/**
 * Append a NUL terminated string without its terminator.
 * @param buffer Open buffer.
 * @param text Text to append.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_buffer_text(kc_buffer_t *buffer, const char *text) {
    if (text == NULL) return KC_KAIMAN_EINVAL;
    return kc_buffer_append(buffer, text, strlen(text));
}

/**
 * Append formatted text to one buffer.
 * @param buffer Open buffer.
 * @param format Printf style format.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_buffer_format(kc_buffer_t *buffer, const char *format, ...) {
    va_list args;
    va_list copy;
    int length;
    char *text;
    int status;

    if (buffer == NULL || format == NULL) return KC_KAIMAN_EINVAL;
    va_start(args, format);
    va_copy(copy, args);
    length = vsnprintf(NULL, 0U, format, copy);
    va_end(copy);
    if (length < 0) {
        va_end(args);
        return KC_KAIMAN_EFORMAT;
    }
    text = (char *)malloc((size_t)length + 1U);
    if (text == NULL) {
        va_end(args);
        return KC_KAIMAN_ENOMEM;
    }
    (void)vsnprintf(text, (size_t)length + 1U, format, args);
    va_end(args);
    status = kc_buffer_append(buffer, text, (size_t)length);
    free(text);
    return status;
}

/**
 * Borrow the NUL terminated contents of one buffer.
 * @param buffer Open buffer.
 * @return NUL terminated bytes, or NULL for an invalid buffer.
 */
const char *kc_buffer_value(const kc_buffer_t *buffer) {
    if (buffer == NULL || buffer->data == NULL) return NULL;
    return buffer->data;
}

/**
 * Release one growable buffer.
 * @param buffer Buffer to close, or NULL.
 * @return void
 */
void kc_buffer_close(kc_buffer_t *buffer) {
    if (buffer == NULL) return;
    free(buffer->data);
    free(buffer);
}

/**
 * Copy a NUL terminated string.
 * @param text Text to copy, or NULL.
 * @return Owned copy, or NULL on failure.
 */
char *kc_util_strdup(const char *text) {
    size_t length;
    char *copy;

    if (text == NULL) return NULL;
    length = strlen(text) + 1U;
    copy = (char *)malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

/**
 * Encode bytes as standard base64.
 * @param data Bytes to encode.
 * @param size Byte count.
 * @return Owned NUL terminated base64, or NULL on failure.
 */
char *kc_util_base64(const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t length;
    size_t index;
    size_t offset = 0U;
    char *result;

    if (data == NULL && size != 0U) return NULL;
    length = ((size + 2U) / 3U) * 4U;
    result = (char *)malloc(length + 1U);
    if (result == NULL) return NULL;
    for (index = 0U; index < size; index += 3U) {
        unsigned long value = (unsigned long)bytes[index] << 16;
        if (index + 1U < size) value |= (unsigned long)bytes[index + 1U] << 8;
        if (index + 2U < size) value |= (unsigned long)bytes[index + 2U];
        result[offset++] = UTIL_BASE64_TABLE[(value >> 18) & 0x3fUL];
        result[offset++] = UTIL_BASE64_TABLE[(value >> 12) & 0x3fUL];
        result[offset++] = index + 1U < size
            ? UTIL_BASE64_TABLE[(value >> 6) & 0x3fUL] : '=';
        result[offset++] = index + 2U < size
            ? UTIL_BASE64_TABLE[value & 0x3fUL] : '=';
    }
    result[offset] = '\0';
    return result;
}

/**
 * Resolve the private session path of one service.
 * @param name Service name.
 * @return Owned path, or NULL on failure.
 */
char *kc_util_session_path(const char *name) {
    const char *base = getenv("XDG_STATE_HOME");
    const char *home;
    kc_buffer_t *buffer = NULL;
    int status;

    if (base == NULL || *base == '\0') {
        home = getenv("HOME");
#ifdef _WIN32
        if (home == NULL || *home == '\0') home = getenv("USERPROFILE");
#endif
        if (home == NULL || *home == '\0') home = ".";
        base = home;
        status = kc_buffer_open(&buffer);
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_format(buffer, "%s/.local/state/kaiman", base);
        }
    } else {
        status = kc_buffer_open(&buffer);
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_format(buffer, "%s/kaiman", base);
        }
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(buffer, "/sessions/%s.session", name);
    }
    if (status != KC_KAIMAN_OK) {
        kc_buffer_close(buffer);
        return NULL;
    }
    home = kc_buffer_value(buffer);
    status = util_parent(home);
    if (status == 0) {
        kc_buffer_close(buffer);
        return NULL;
    }
    name = kc_util_strdup(home);
    kc_buffer_close(buffer);
    return (char *)name;
}

/**
 * Read one private file as bytes.
 * @param path File path.
 * @param size Receives the byte count.
 * @return Owned bytes with a NUL sentinel, or NULL when absent.
 */
char *kc_util_read_file(const char *path, size_t *size) {
    FILE *file;
    char *data;
    long length;

    if (path == NULL || size == NULL) return NULL;
    *size = 0U;
    file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (char *)malloc((size_t)length + 1U);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }
    if (length > 0 && fread(data, 1U, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    data[length] = '\0';
    fclose(file);
    *size = (size_t)length;
    return data;
}

/**
 * Replace one private file atomically.
 * @param path File path.
 * @param data Bytes to write.
 * @param size Byte count.
 * @return 1 on success, 0 on failure.
 */
int kc_util_write_private(const char *path, const void *data, size_t size) {
    char *temporary;
    size_t length;
    FILE *file;
    int ok = 0;

    if (path == NULL || (data == NULL && size != 0U)) return 0;
    if (!util_parent(path)) return 0;
    length = strlen(path) + 5U;
    temporary = (char *)malloc(length);
    if (temporary == NULL) return 0;
    (void)snprintf(temporary, length, "%s.tmp", path);
    file = fopen(temporary, "wb");
    if (file == NULL) {
        free(temporary);
        return 0;
    }
    if (size == 0U || fwrite(data, 1U, size, file) == size) {
#ifndef _WIN32
        ok = chmod(temporary, 0600) == 0;
#else
        ok = 1;
#endif
    }
    if (fclose(file) != 0) ok = 0;
    if (ok) {
#ifdef _WIN32
        remove(path);
#endif
        ok = rename(temporary, path) == 0;
    }
    if (!ok) remove(temporary);
    free(temporary);
    return ok;
}

/**
 * Quote a command argument for the platform shell.
 * @param text Argument text.
 * @return Owned quoted argument, or NULL on failure.
 */
char *kc_util_quote(const char *text) {
    kc_buffer_t *buffer = NULL;
#ifndef _WIN32
    const char *cursor;
#endif
    char *result;
    int status;

    if (text == NULL) return NULL;
    status = kc_buffer_open(&buffer);
#ifdef _WIN32
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "\"");
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, text);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "\"");
#else
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "'");
    for (cursor = text; status == KC_KAIMAN_OK && *cursor != '\0'; cursor++) {
        if (*cursor == '\'') {
            status = kc_buffer_text(buffer, "'\\''");
        } else {
            status = kc_buffer_append(buffer, cursor, 1U);
        }
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "'");
#endif
    if (status != KC_KAIMAN_OK) {
        kc_buffer_close(buffer);
        return NULL;
    }
    result = kc_util_strdup(kc_buffer_value(buffer));
    kc_buffer_close(buffer);
    return result;
}

/**
 * Resolve the absolute path of the running executable.
 * @return Owned path, or NULL when unavailable.
 */
char *kc_util_executable_path(void) {
#ifdef _WIN32
    char *buffer = NULL;
    DWORD length = 260U;
    DWORD result;

    for (;;) {
        char *larger = (char *)realloc(buffer, length);
        if (larger == NULL) {
            free(buffer);
            return NULL;
        }
        buffer = larger;
        result = GetModuleFileNameA(NULL, buffer, length);
        if (result == 0U) {
            free(buffer);
            return NULL;
        }
        if (result < length) break;
        if (length > 32768U) {
            free(buffer);
            return NULL;
        }
        length *= 2U;
    }
    return buffer;
#elif defined(__APPLE__)
    uint32_t length = 0U;
    char *buffer;

    _NSGetExecutablePath(NULL, &length);
    buffer = (char *)malloc(length + 1U);
    if (buffer == NULL) return NULL;
    if (_NSGetExecutablePath(buffer, &length) != 0) {
        free(buffer);
        return NULL;
    }
    buffer[length] = '\0';
    return buffer;
#else
    size_t length = 256U;
    char *buffer = NULL;
    ssize_t result;

    for (;;) {
        char *larger = (char *)realloc(buffer, length + 1U);
        if (larger == NULL) {
            free(buffer);
            return NULL;
        }
        buffer = larger;
        result = readlink("/proc/self/exe", buffer, length);
        if (result < 0) {
            free(buffer);
            return NULL;
        }
        if ((size_t)result < length) break;
        if (length > 65536U) {
            free(buffer);
            return NULL;
        }
        length *= 2U;
    }
    buffer[result] = '\0';
    return buffer;
#endif
}

/**
 * Read a monotonic time in seconds.
 * @return Seconds since an arbitrary epoch.
 */
double kc_util_seconds(void) {
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
#endif
}

/**
 * Sleep for a bounded number of milliseconds.
 * @param milliseconds Milliseconds to wait.
 * @return void
 */
void kc_util_sleep(int milliseconds) {
    if (milliseconds <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000);
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        continue;
    }
#endif
}

/**
 * Open one mutual exclusion primitive.
 * @param mutex Receives the mutex.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_mutex_open(kc_mutex_t *mutex) {
    if (mutex == NULL) return KC_KAIMAN_EINVAL;
#ifdef _WIN32
    InitializeCriticalSection(mutex);
    return KC_KAIMAN_OK;
#else
    return pthread_mutex_init(mutex, NULL) == 0
        ? KC_KAIMAN_OK : KC_KAIMAN_EIO;
#endif
}

/**
 * Close one mutual exclusion primitive.
 * @param mutex Mutex to close, or NULL.
 * @return void
 */
void kc_mutex_close(kc_mutex_t *mutex) {
    if (mutex == NULL) return;
#ifdef _WIN32
    DeleteCriticalSection(mutex);
#else
    pthread_mutex_destroy(mutex);
#endif
}

/**
 * Lock one mutual exclusion primitive.
 * @param mutex Mutex to lock.
 * @return void
 */
void kc_mutex_lock(kc_mutex_t *mutex) {
    if (mutex == NULL) return;
#ifdef _WIN32
    EnterCriticalSection(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}

/**
 * Unlock one mutual exclusion primitive.
 * @param mutex Mutex to unlock.
 * @return void
 */
void kc_mutex_unlock(kc_mutex_t *mutex) {
    if (mutex == NULL) return;
#ifdef _WIN32
    LeaveCriticalSection(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}

/**
 * Resolve the private process identifier path of the gateway.
 * @return Owned path, or NULL on failure.
 */
char *kc_util_pid_path(void) {
    const char *base = getenv("XDG_STATE_HOME");
    const char *home;
    kc_buffer_t *buffer = NULL;
    char *result;
    int status;

    if (base == NULL || *base == '\0') {
        home = getenv("HOME");
#ifdef _WIN32
        if (home == NULL || *home == '\0') home = getenv("USERPROFILE");
#endif
        if (home == NULL || *home == '\0') home = ".";
        base = home;
        status = kc_buffer_open(&buffer);
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_format(buffer, "%s/.local/state/kaiman", base);
        }
    } else {
        status = kc_buffer_open(&buffer);
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_format(buffer, "%s/kaiman", base);
        }
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(buffer, "/kaiman.pid");
    }
    if (status != KC_KAIMAN_OK) {
        kc_buffer_close(buffer);
        return NULL;
    }
    if (!util_parent(kc_buffer_value(buffer))) {
        kc_buffer_close(buffer);
        return NULL;
    }
    result = kc_util_strdup(kc_buffer_value(buffer));
    kc_buffer_close(buffer);
    return result;
}
