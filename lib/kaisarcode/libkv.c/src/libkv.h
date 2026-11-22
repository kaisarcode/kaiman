/**
 * libkv.h - In-memory key-value collection.
 * Summary: Public API for binary values addressed by string keys.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef KC_KV_H
#define KC_KV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kc_kv kc_kv_t;

typedef struct {
    const void *data;
    size_t size;
} kc_kv_options_t;

#define KC_KV_OK 0
#define KC_KV_NOT_FOUND 1
#define KC_KV_EINVAL -1
#define KC_KV_ENOMEM -2
#define KC_KV_EFORMAT -3

/**
 * Open an empty collection or restore a portable binary snapshot.
 * NULL options create an empty collection. Provided bytes are copied.
 * @param out Receives the collection or NULL on failure.
 * @param options Optional serialized data for restoration.
 * @return KC_KV_OK or an error status.
 */
int kc_kv_open(kc_kv_t **out, const kc_kv_options_t *options);

/**
 * Insert or replace one binary value under a nonempty string key.
 * The collection copies the key and value. A NULL value is permitted only
 * when size is zero, to represent a present, empty value. Failure leaves
 * the previous entry unchanged. Key comparison is bytewise and case-sensitive.
 * @param kv Collection handle.
 * @param key Nonempty NUL-terminated key.
 * @param value Value bytes, or NULL for an empty value.
 * @param size Value byte length.
 * @return KC_KV_OK or an error status.
 */
int kc_kv_set(kc_kv_t *kv, const char *key, const void *value, size_t size);

/**
 * Retrieve a value without transferring ownership.
 * The borrowed bytes remain valid until that entry is replaced or deleted,
 * or the collection is closed. A present empty value has size zero and may
 * have a NULL data pointer. Missing keys return KC_KV_NOT_FOUND.
 * @param kv Collection handle.
 * @param key Nonempty NUL-terminated key.
 * @param out_value Receives borrowed bytes, or NULL when absent.
 * @param out_size Receives value length, or zero when absent.
 * @return KC_KV_OK, KC_KV_NOT_FOUND, or an error status.
 */
int kc_kv_get(const kc_kv_t *kv, const char *key,
    const void **out_value, size_t *out_size);

/**
 * Remove one key and its value.
 * @param kv Collection handle.
 * @param key Nonempty NUL-terminated key.
 * @return KC_KV_OK, KC_KV_NOT_FOUND, or an error status.
 */
int kc_kv_del(kc_kv_t *kv, const char *key);

/**
 * Return the number of distinct keys in the collection.
 * @param kv Collection handle, or NULL.
 * @return Number of entries, or zero for NULL.
 */
size_t kc_kv_count(const kc_kv_t *kv);

/**
 * Read one entry in ascending bytewise key order.
 * Key and value are borrowed until the collection is modified or closed.
 * @param kv Collection handle.
 * @param index Zero-based entry index.
 * @param out_key Receives borrowed NUL-terminated key.
 * @param out_value Receives borrowed value bytes.
 * @param out_size Receives value size.
 * @return KC_KV_OK, KC_KV_NOT_FOUND, or an error status.
 */
int kc_kv_at(const kc_kv_t *kv, size_t index,
    const char **out_key, const void **out_value, size_t *out_size);

/**
 * Export the collection to a portable, self-contained binary value.
 * The collection owns the bytes until its next export or close.
 * Export does not write to any storage medium.
 * @param kv Collection handle.
 * @param out_data Receives borrowed collection-owned bytes.
 * @param out_size Receives byte count.
 * @return KC_KV_OK or an error status.
 */
int kc_kv_export(kc_kv_t *kv, void **out_data, size_t *out_size);

/**
 * Close a collection and release its owned memory. NULL is safe.
 * @param kv Collection handle, or NULL.
 * @return None.
 */
void kc_kv_close(kc_kv_t *kv);

/**
 * Describe one status value.
 * @param status Status to describe.
 * @return Static status description.
 */
const char *kc_kv_strerror(int status);

/**
 * Return the library semantic version.
 * @return Semantic version string.
 */
const char *kc_kv_version(void);

#ifdef __cplusplus
}
#endif

#endif
