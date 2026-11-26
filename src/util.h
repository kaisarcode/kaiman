/**
 * util.h - Small shared helpers for kaiman.
 * Summary: Internal buffers, encoding, timing and private file helpers.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef KC_KAIMAN_UTIL_H
#define KC_KAIMAN_UTIL_H

#include <stddef.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
typedef CRITICAL_SECTION kc_mutex_t;
#else
typedef pthread_mutex_t kc_mutex_t;
#endif

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} kc_buffer_t;

/**
 * Open an empty growable byte buffer.
 * @param out Receives the buffer, or NULL on failure.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_buffer_open(kc_buffer_t **out);

/**
 * Append bytes to one growable buffer.
 * @param buffer Open buffer.
 * @param data Bytes to append.
 * @param size Byte count.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_buffer_append(kc_buffer_t *buffer, const void *data, size_t size);

/**
 * Append a NUL terminated string without its terminator.
 * @param buffer Open buffer.
 * @param text Text to append.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_buffer_text(kc_buffer_t *buffer, const char *text);

/**
 * Append formatted text to one buffer.
 * @param buffer Open buffer.
 * @param format Printf style format.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_buffer_format(kc_buffer_t *buffer, const char *format, ...);

/**
 * Borrow the NUL terminated contents of one buffer.
 * @param buffer Open buffer.
 * @return NUL terminated bytes, or NULL for an invalid buffer.
 */
const char *kc_buffer_value(const kc_buffer_t *buffer);

/**
 * Release one growable buffer.
 * @param buffer Buffer to close, or NULL.
 * @return void
 */
void kc_buffer_close(kc_buffer_t *buffer);

/**
 * Copy a NUL terminated string.
 * @param text Text to copy, or NULL.
 * @return Owned copy, or NULL on failure.
 */
char *kc_util_strdup(const char *text);

/**
 * Encode bytes as standard base64.
 * @param data Bytes to encode.
 * @param size Byte count.
 * @return Owned NUL terminated base64, or NULL on failure.
 */
char *kc_util_base64(const void *data, size_t size);

/**
 * Resolve the private session path of one service.
 * @param name Service name.
 * @return Owned path, or NULL on failure.
 */
char *kc_util_session_path(const char *name);

/**
 * Read one private file as bytes.
 * @param path File path.
 * @param size Receives the byte count.
 * @return Owned bytes with a NUL sentinel, or NULL when absent.
 */
char *kc_util_read_file(const char *path, size_t *size);

/**
 * Replace one private file atomically.
 * @param path File path.
 * @param data Bytes to write.
 * @param size Byte count.
 * @return 1 on success, 0 on failure.
 */
int kc_util_write_private(const char *path, const void *data, size_t size);

/**
 * Quote a command argument for the platform shell.
 * @param text Argument text.
 * @return Owned quoted argument, or NULL on failure.
 */
char *kc_util_quote(const char *text);

/**
 * Resolve the absolute path of the running executable.
 * @return Owned path, or NULL when unavailable.
 */
char *kc_util_executable_path(void);

/**
 * Read a monotonic time in seconds.
 * @return Seconds since an arbitrary epoch.
 */
double kc_util_seconds(void);

/**
 * Sleep for a bounded number of milliseconds.
 * @param milliseconds Milliseconds to wait.
 * @return void
 */
void kc_util_sleep(int milliseconds);

/**
 * Open one mutual exclusion primitive.
 * @param mutex Receives the mutex.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_mutex_open(kc_mutex_t *mutex);

/**
 * Close one mutual exclusion primitive.
 * @param mutex Mutex to close, or NULL.
 * @return void
 */
void kc_mutex_close(kc_mutex_t *mutex);

/**
 * Lock one mutual exclusion primitive.
 * @param mutex Mutex to lock.
 * @return void
 */
void kc_mutex_lock(kc_mutex_t *mutex);

/**
 * Unlock one mutual exclusion primitive.
 * @param mutex Mutex to unlock.
 * @return void
 */
void kc_mutex_unlock(kc_mutex_t *mutex);

/**
 * Resolve the private process identifier path of the gateway.
 * @return Owned path, or NULL on failure.
 */
char *kc_util_pid_path(void);

#ifdef __cplusplus
}
#endif

#endif
