/**
 * libkv.c - In-memory key-value collection.
 * Summary: Owns binary values addressed by unique string keys.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#include "libkv.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef KC_KV_VERSION
#error "KC_KV_VERSION must come from manifest.json via CMake"
#endif

#define KC_KV_HEADER_SIZE 20U
#define KC_KV_RECORD_HEADER_SIZE 12U
#define KC_KV_FORMAT_VERSION 1U

static const unsigned char kc_kv_magic[8] = {
    'K', 'C', 'K', 'V', 'B', 'I', 'N', '\n'
};

typedef struct {
    char *key;
    unsigned char *value;
    size_t size;
} kc_kv_entry_t;

struct kc_kv {
    kc_kv_entry_t *entries;
    size_t count;
    size_t capacity;
    unsigned char *snapshot;
};

/**
 * Find a key and its sorted insertion position.
 * @param kv Source collection.
 * @param key Key to locate.
 * @param found Receives whether the key exists.
 * @return Entry index or insertion position.
 */
static size_t kc_kv_locate(const kc_kv_t *kv, const char *key, int *found)
{
    size_t low = 0, high = kv->count;

    *found = 0;
    while (low < high) {
        size_t mid = low + (high - low) / 2U;
        int comparison = strcmp(key, kv->entries[mid].key);

        if (comparison == 0) {
            *found = 1;
            return mid;
        }
        if (comparison > 0) low = mid + 1U;
        else high = mid;
    }
    return low;
}

/**
 * Make a private copy of a possibly empty binary value.
 * @param data Source bytes.
 * @param size Byte count.
 * @return Owned bytes, or NULL for zero bytes or allocation failure.
 */
static unsigned char *kc_kv_copy(const void *data, size_t size)
{
    unsigned char *value;

    if (size == 0) return NULL;
    value = (unsigned char *)malloc(size);
    if (value != NULL) memcpy(value, data, size);
    return value;
}

/**
 * Expand the entry array without changing its logical contents.
 * @param kv Collection to expand.
 * @return KC_KV_OK or an allocation error.
 */
static int kc_kv_grow(kc_kv_t *kv)
{
    size_t capacity;
    kc_kv_entry_t *entries;

    if (kv->count < kv->capacity) return KC_KV_OK;
    capacity = kv->capacity == 0 ? 8U : kv->capacity;
    if (kv->capacity != 0) {
        if (capacity > SIZE_MAX / 2U) return KC_KV_ENOMEM;
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*entries)) return KC_KV_ENOMEM;
    entries = (kc_kv_entry_t *)realloc(kv->entries, capacity * sizeof(*entries));
    if (entries == NULL) return KC_KV_ENOMEM;
    kv->entries = entries;
    kv->capacity = capacity;
    return KC_KV_OK;
}

/**
 * Decode a little-endian 32-bit integer.
 * @param p Input bytes.
 * @return Decoded integer.
 */
static uint32_t kc_kv_read32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Decode a little-endian 64-bit integer.
 * @param p Input bytes.
 * @return Decoded integer.
 */
static uint64_t kc_kv_read64(const unsigned char *p)
{
    uint64_t value = 0;

    for (unsigned int i = 0; i < 8U; i++)
        value |= (uint64_t)p[i] << (8U * i);
    return value;
}

/**
 * Encode a little-endian 32-bit integer.
 * @param p Destination bytes.
 * @param value Value to encode.
 * @return None.
 */
static void kc_kv_write32(unsigned char *p, uint32_t value)
{
    for (unsigned int i = 0; i < 4U; i++)
        p[i] = (unsigned char)(value >> (8U * i));
}

/**
 * Encode a little-endian 64-bit integer.
 * @param p Destination bytes.
 * @param value Value to encode.
 * @return None.
 */
static void kc_kv_write64(unsigned char *p, uint64_t value)
{
    for (unsigned int i = 0; i < 8U; i++)
        p[i] = (unsigned char)(value >> (8U * i));
}

/**
 * Check that a byte range is within a bounded buffer.
 * @param size Input buffer size.
 * @param offset Current offset.
 * @param length Required byte count.
 * @return Nonzero when the range fits.
 */
static int kc_kv_fits(size_t size, size_t offset, size_t length)
{
    return offset <= size && length <= size - offset;
}

/**
 * Restore a collection from serialized data.
 * @param out Receives the restored collection.
 * @param source Source binary snapshot.
 * @param size Exact snapshot size.
 * @return Result status.
 */
static int kc_kv_restore(kc_kv_t **out, const void *source, size_t size);

/**
 * Open an empty collection or restore serialized data.
 * @param out Receives the opened collection.
 * @param options Optional binary snapshot.
 * @return Result status.
 */
int kc_kv_open(kc_kv_t **out, const kc_kv_options_t *options)
{
    if (out == NULL) return KC_KV_EINVAL;
    *out = NULL;
    if (options != NULL) {
        if (options->data == NULL || options->size == 0)
            return KC_KV_EFORMAT;
        return kc_kv_restore(out, options->data, options->size);
    }
    *out = (kc_kv_t *)calloc(1, sizeof(kc_kv_t));
    return *out == NULL ? KC_KV_ENOMEM : KC_KV_OK;
}

/**
 * Insert or replace a collection value.
 * @param kv Collection handle.
 * @param key Entry key.
 * @param value Entry bytes.
 * @param size Number of entry bytes.
 * @return Result status.
 */
int kc_kv_set(kc_kv_t *kv, const char *key, const void *value, size_t size)
{
    size_t position, key_size;
    unsigned char *copy;
    char *name;
    int found, status;

    if (kv == NULL || key == NULL || !*key || (value == NULL && size != 0))
        return KC_KV_EINVAL;
    position = kc_kv_locate(kv, key, &found);
    copy = kc_kv_copy(value, size);
    if (size != 0 && copy == NULL) return KC_KV_ENOMEM;
    if (found) {
        free(kv->entries[position].value);
        kv->entries[position].value = copy;
        kv->entries[position].size = size;
        return KC_KV_OK;
    }
    key_size = strlen(key);
    if (key_size == SIZE_MAX) {
        free(copy);
        return KC_KV_ENOMEM;
    }
    name = (char *)malloc(key_size + 1U);
    if (name == NULL) {
        free(copy);
        return KC_KV_ENOMEM;
    }
    memcpy(name, key, key_size + 1U);
    status = kc_kv_grow(kv);
    if (status != KC_KV_OK) {
        free(name);
        free(copy);
        return status;
    }
    memmove(kv->entries + position + 1U, kv->entries + position,
        (kv->count - position) * sizeof(*kv->entries));
    kv->entries[position].key = name;
    kv->entries[position].value = copy;
    kv->entries[position].size = size;
    kv->count++;
    return KC_KV_OK;
}

/**
 * Get a collection value by key.
 * @param kv Collection handle.
 * @param key Entry key.
 * @param out_value Receives the entry bytes.
 * @param out_size Receives the entry byte count.
 * @return Result status.
 */
int kc_kv_get(const kc_kv_t *kv, const char *key,
    const void **out_value, size_t *out_size)
{
    size_t position;
    int found;

    if (out_value != NULL) *out_value = NULL;
    if (out_size != NULL) *out_size = 0;
    if (kv == NULL || key == NULL || !*key || out_value == NULL ||
        out_size == NULL) return KC_KV_EINVAL;
    position = kc_kv_locate(kv, key, &found);
    if (!found) return KC_KV_NOT_FOUND;
    *out_value = kv->entries[position].value;
    *out_size = kv->entries[position].size;
    return KC_KV_OK;
}

/**
 * Delete a collection entry.
 * @param kv Collection handle.
 * @param key Entry key.
 * @return Result status.
 */
int kc_kv_del(kc_kv_t *kv, const char *key)
{
    size_t position;
    int found;

    if (kv == NULL || key == NULL || !*key) return KC_KV_EINVAL;
    position = kc_kv_locate(kv, key, &found);
    if (!found) return KC_KV_NOT_FOUND;
    free(kv->entries[position].key);
    free(kv->entries[position].value);
    kv->count--;
    memmove(kv->entries + position, kv->entries + position + 1U,
        (kv->count - position) * sizeof(*kv->entries));
    return KC_KV_OK;
}

/**
 * Count collection entries.
 * @param kv Collection handle.
 * @return Entry count.
 */
size_t kc_kv_count(const kc_kv_t *kv)
{
    return kv == NULL ? 0U : kv->count;
}

/**
 * Get an entry by sorted index.
 * @param kv Collection handle.
 * @param index Entry index.
 * @param out_key Receives the entry key.
 * @param out_value Receives the entry bytes.
 * @param out_size Receives the entry byte count.
 * @return Result status.
 */
int kc_kv_at(const kc_kv_t *kv, size_t index,
    const char **out_key, const void **out_value, size_t *out_size)
{
    if (out_key != NULL) *out_key = NULL;
    if (out_value != NULL) *out_value = NULL;
    if (out_size != NULL) *out_size = 0;
    if (kv == NULL || out_key == NULL || out_value == NULL ||
        out_size == NULL) return KC_KV_EINVAL;
    if (index >= kv->count) return KC_KV_NOT_FOUND;
    *out_key = kv->entries[index].key;
    *out_value = kv->entries[index].value;
    *out_size = kv->entries[index].size;
    return KC_KV_OK;
}

/**
 * Export collection data as a portable snapshot.
 * @param kv Collection handle.
 * @param out_data Receives allocated snapshot data.
 * @param out_size Receives the snapshot byte count.
 * @return Result status.
 */
int kc_kv_export(kc_kv_t *kv, void **out_data, size_t *out_size)
{
    size_t size = KC_KV_HEADER_SIZE, offset;
    unsigned char *data;

    if (out_data != NULL) *out_data = NULL;
    if (out_size != NULL) *out_size = 0;
    if (kv == NULL || out_data == NULL || out_size == NULL)
        return KC_KV_EINVAL;
    for (size_t i = 0; i < kv->count; i++) {
        size_t key_size = strlen(kv->entries[i].key);
        size_t val_size = kv->entries[i].size;

        if (key_size == 0 || key_size > UINT32_MAX ||
            size > SIZE_MAX - KC_KV_RECORD_HEADER_SIZE ||
            key_size > SIZE_MAX - size - KC_KV_RECORD_HEADER_SIZE ||
            val_size > SIZE_MAX - size - KC_KV_RECORD_HEADER_SIZE - key_size)
            return KC_KV_ENOMEM;
        size += KC_KV_RECORD_HEADER_SIZE + key_size + val_size;
    }
    data = (unsigned char *)malloc(size);
    if (data == NULL) return KC_KV_ENOMEM;
    memcpy(data, kc_kv_magic, sizeof(kc_kv_magic));
    kc_kv_write32(data + 8U, KC_KV_FORMAT_VERSION);
    kc_kv_write64(data + 12U, (uint64_t)kv->count);
    offset = KC_KV_HEADER_SIZE;
    for (size_t i = 0; i < kv->count; i++) {
        size_t key_size = strlen(kv->entries[i].key);
        size_t val_size = kv->entries[i].size;

        kc_kv_write32(data + offset, (uint32_t)key_size);
        kc_kv_write64(data + offset + 4U, (uint64_t)val_size);
        offset += KC_KV_RECORD_HEADER_SIZE;
        memcpy(data + offset, kv->entries[i].key, key_size);
        offset += key_size;
        if (val_size != 0) memcpy(data + offset, kv->entries[i].value, val_size);
        offset += val_size;
    }
    free(kv->snapshot);
    kv->snapshot = data;
    *out_data = data;
    *out_size = size;
    return KC_KV_OK;
}

/**
 * Restore a collection from a portable snapshot.
 * @param out Receives the imported collection.
 * @param source Snapshot bytes.
 * @param size Snapshot byte count.
 * @return Result status.
 */
static int kc_kv_restore(kc_kv_t **out, const void *source, size_t size)
{
    const unsigned char *data = (const unsigned char *)source;
    uint64_t count;
    size_t offset = KC_KV_HEADER_SIZE;
    kc_kv_t *kv = NULL;
    int status = KC_KV_EFORMAT;

    if (out == NULL) return KC_KV_EINVAL;
    *out = NULL;
    if (data == NULL || size < KC_KV_HEADER_SIZE ||
        memcmp(data, kc_kv_magic, sizeof(kc_kv_magic)) != 0 ||
        kc_kv_read32(data + 8U) != KC_KV_FORMAT_VERSION) return KC_KV_EFORMAT;
    count = kc_kv_read64(data + 12U);
    if (count > (size - KC_KV_HEADER_SIZE) / (KC_KV_RECORD_HEADER_SIZE + 1U))
        return KC_KV_EFORMAT;
    status = kc_kv_open(&kv, NULL);
    if (status != KC_KV_OK) return status;
    for (uint64_t i = 0; i < count; i++) {
        size_t key_size, value_size;
        char *key;

        status = KC_KV_EFORMAT;
        if (!kc_kv_fits(size, offset, KC_KV_RECORD_HEADER_SIZE)) break;
        key_size = (size_t)kc_kv_read32(data + offset);
        if (kc_kv_read64(data + offset + 4U) > SIZE_MAX) break;
        value_size = (size_t)kc_kv_read64(data + offset + 4U);
        offset += KC_KV_RECORD_HEADER_SIZE;
        if (key_size == 0 || !kc_kv_fits(size, offset, key_size) ||
            !kc_kv_fits(size, offset + key_size, value_size)) break;
        if (memchr(data + offset, 0, key_size) != NULL) break;
        key = (char *)malloc(key_size + 1U);
        if (key == NULL) {
            status = KC_KV_ENOMEM;
            break;
        }
        memcpy(key, data + offset, key_size);
        key[key_size] = '\0';
        if (i != 0 && strcmp(key, kv->entries[kv->count - 1U].key) <= 0) {
            free(key);
            break;
        }
        offset += key_size;
        status = kc_kv_set(kv, key, data + offset, value_size);
        free(key);
        if (status != KC_KV_OK) break;
        offset += value_size;
    }
    if (status == KC_KV_OK && kv->count == count && offset == size) {
        *out = kv;
        return KC_KV_OK;
    }
    kc_kv_close(kv);
    return status == KC_KV_OK ? KC_KV_EFORMAT : status;
}

/**
 * Close a collection and release its entries.
 * @param kv Collection handle.
 * @return void
 */
void kc_kv_close(kc_kv_t *kv)
{
    if (kv == NULL) return;
    for (size_t i = 0; i < kv->count; i++) {
        free(kv->entries[i].key);
        free(kv->entries[i].value);
    }
    free(kv->entries);
    free(kv->snapshot);
    free(kv);
}

/**
 * Describe a result status.
 * @param status Result status.
 * @return Status description.
 */
const char *kc_kv_strerror(int status)
{
    switch (status) {
        case KC_KV_OK: return "Success";
        case KC_KV_NOT_FOUND: return "Key not found";
        case KC_KV_EINVAL: return "Invalid argument";
        case KC_KV_ENOMEM: return "Out of memory";
        case KC_KV_EFORMAT: return "Invalid binary value";
        default: return "Unknown error";
    }
}

/**
 * Return the library semantic version.
 * @return Semantic version string.
 */
const char *kc_kv_version(void)
{
    return KC_KV_VERSION;
}
