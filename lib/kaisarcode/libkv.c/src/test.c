/**
 * test.c - libkv public API contract tests.
 * Summary: Validates mutation, ownership, binary snapshots, and error handling.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "libkv.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define CHECK(condition) ((condition) ? 0 : 1)

typedef int (*case_fn)(void);

typedef struct {
    const char *name;
    const char *detail;
    case_fn run;
} case_item_t;

/**
 * Open an empty collection for a test.
 * @return Owned collection, or NULL on failure.
 */
static kc_kv_t *create(void)
{
    kc_kv_t *kv = NULL;

    if (kc_kv_open(&kv, NULL) != KC_KV_OK) return NULL;
    return kv;
}

/**
 * Check one value and distinguish empty data from an absent key.
 * @param kv Collection to inspect.
 * @param key Requested key.
 * @param expected Expected bytes, or NULL for an empty value.
 * @param length Expected byte length.
 * @return Zero when the value matches.
 */
static int value_equals(const kc_kv_t *kv, const char *key,
    const void *expected, size_t length)
{
    const void *actual = NULL;
    size_t size = 0;

    if (kc_kv_get(kv, key, &actual, &size) != KC_KV_OK) return 1;
    if (size != length) return 1;
    if (size == 0) return 0;
    if (actual == NULL || expected == NULL) return 1;
    return memcmp(actual, expected, size) != 0;
}

/**
 * Confirm a corrupt binary snapshot is rejected without returning a handle.
 * @param data Serialized bytes.
 * @param size Byte length.
 * @return Zero when import rejects the snapshot.
 */
static int rejects(const void *data, size_t size)
{
    kc_kv_t *kv = NULL;
    int status = kc_kv_open(&kv, &(kc_kv_options_t){.data = data, .size = size});

    if (kv != NULL) kc_kv_close(kv);
    return status != KC_KV_EFORMAT || kv != NULL;
}

/**
 * Validate collection creation, empty state, and NULL handling.
 * @return Zero when the contract holds.
 */
static int case_open(void)
{
    kc_kv_t *kv = create();
    const char *key = NULL;
    const void *value = NULL;
    size_t size = 0;
    int fail = CHECK(kc_kv_open(NULL, NULL) == KC_KV_EINVAL);

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_count(kv) == 0);
    fail |= CHECK(kc_kv_count(NULL) == 0);
    fail |= CHECK(kc_kv_at(kv, 0, &key, &value, &size) == KC_KV_NOT_FOUND);
    fail |= CHECK(key == NULL && value == NULL && size == 0);
    kc_kv_close(kv);
    kc_kv_close(NULL);
    return fail;
}

/**
 * Validate binary values and copying of caller-owned key and value bytes.
 * @return Zero when the contract holds.
 */
static int case_set(void)
{
    kc_kv_t *kv = create();
    char key[] = "binary";
    unsigned char bytes[] = {0, 255, 1, 128};
    const unsigned char expected[] = {0, 255, 1, 128};
    int fail = 0;

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_set(kv, key, bytes, sizeof(bytes)) == KC_KV_OK);
    key[0] = 'x';
    bytes[1] = 2;
    fail |= CHECK(value_equals(kv, "binary", expected, sizeof(expected)) == 0);
    fail |= CHECK(kc_kv_count(kv) == 1);
    fail |= CHECK(kc_kv_set(kv, "text", "hello", 5) == KC_KV_OK);
    fail |= CHECK(value_equals(kv, "text", "hello", 5) == 0);
    kc_kv_close(kv);
    return fail;
}

/**
 * Check replacement semantics and stable cardinality.
 * @return Zero when replacements retain one key.
 */
static int case_replace(void)
{
    kc_kv_t *kv = create();
    int fail = 0;

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_set(kv, "a", "first", 5) == KC_KV_OK);
    fail |= CHECK(kc_kv_set(kv, "a", "second", 6) == KC_KV_OK);
    fail |= CHECK(value_equals(kv, "a", "second", 6) == 0);
    fail |= CHECK(kc_kv_count(kv) == 1);
    fail |= CHECK(kc_kv_set(kv, "a", "x", 1) == KC_KV_OK);
    fail |= CHECK(value_equals(kv, "a", "x", 1) == 0);
    kc_kv_close(kv);
    return fail;
}

/**
 * Verify present empty values are distinct from missing keys.
 * @return Zero when empty and missing are distinct.
 */
static int case_empty(void)
{
    kc_kv_t *kv = create();
    const void *value = (const void *)1;
    size_t size = 77;
    int fail = 0;

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_set(kv, "empty", NULL, 0) == KC_KV_OK);
    fail |= CHECK(value_equals(kv, "empty", NULL, 0) == 0);
    fail |= CHECK(kc_kv_get(kv, "missing", &value, &size) == KC_KV_NOT_FOUND);
    fail |= CHECK(value == NULL && size == 0);
    fail |= CHECK(kc_kv_set(kv, "empty", "x", 1) == KC_KV_OK);
    fail |= CHECK(kc_kv_set(kv, "empty", NULL, 0) == KC_KV_OK);
    fail |= CHECK(value_equals(kv, "empty", NULL, 0) == 0);
    kc_kv_close(kv);
    return fail;
}

/**
 * Verify deletion at every position and re-insertion.
 * @return Zero when deletion and re-insertion behave correctly.
 */
static int case_delete(void)
{
    kc_kv_t *kv = create();
    const char *names[] = {"a", "b", "c", "d", "e"};
    int fail = 0;

    if (kv == NULL) return 1;
    for (size_t i = 0; i < 5; i++)
        fail |= CHECK(kc_kv_set(kv, names[i], names[i], 1) == KC_KV_OK);
    fail |= CHECK(kc_kv_del(kv, "c") == KC_KV_OK);
    fail |= CHECK(kc_kv_del(kv, "a") == KC_KV_OK);
    fail |= CHECK(kc_kv_del(kv, "e") == KC_KV_OK);
    fail |= CHECK(kc_kv_del(kv, "c") == KC_KV_NOT_FOUND);
    fail |= CHECK(kc_kv_count(kv) == 2);
    fail |= CHECK(value_equals(kv, "b", "b", 1) == 0);
    fail |= CHECK(value_equals(kv, "d", "d", 1) == 0);
    fail |= CHECK(kc_kv_set(kv, "c", "new", 3) == KC_KV_OK);
    fail |= CHECK(value_equals(kv, "c", "new", 3) == 0);
    fail |= CHECK(kc_kv_count(kv) == 3);
    kc_kv_close(kv);
    return fail;
}

/**
 * Verify bytewise, case-sensitive key ordering.
 * @return Zero when enumeration is deterministic.
 */
static int case_ordering(void)
{
    kc_kv_t *kv = create();
    const char *input[] = {"z", "a", "B", "aa", "A", "b"};
    const char *sorted[] = {"A", "B", "a", "aa", "b", "z"};
    const char *key = NULL;
    const void *value = NULL;
    size_t size = 0;
    int fail = 0;

    if (kv == NULL) return 1;
    for (size_t i = 0; i < 6; i++)
        fail |= CHECK(kc_kv_set(kv, input[i], input[i], strlen(input[i])) == KC_KV_OK);
    for (size_t i = 0; i < 6; i++) {
        if (kc_kv_at(kv, i, &key, &value, &size) != KC_KV_OK) { fail = 1; break; }
        fail |= CHECK(strcmp(key, sorted[i]) == 0);
        fail |= CHECK(size == strlen(sorted[i]));
        fail |= CHECK(memcmp(value, sorted[i], size) == 0);
    }
    fail |= CHECK(kc_kv_count(kv) == 6);
    kc_kv_close(kv);
    return fail;
}

/**
 * Verify out-of-range indexes clear output arguments.
 * @return Zero when iteration errors are explicit and safe.
 */
static int case_iteration(void)
{
    kc_kv_t *kv = create();
    const char *key = "stale";
    const void *value = (const void *)1;
    size_t size = 4;
    int fail = 0;

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_set(kv, "one", "1", 1) == KC_KV_OK);
    fail |= CHECK(kc_kv_at(kv, 1, &key, &value, &size) == KC_KV_NOT_FOUND);
    fail |= CHECK(key == NULL && value == NULL && size == 0);
    fail |= CHECK(kc_kv_at(NULL, 0, &key, &value, &size) == KC_KV_EINVAL);
    fail |= CHECK(kc_kv_at(kv, 0, NULL, &value, &size) == KC_KV_EINVAL);
    fail |= CHECK(value == NULL && size == 0);
    fail |= CHECK(kc_kv_at(kv, 0, &key, NULL, &size) == KC_KV_EINVAL);
    fail |= CHECK(key == NULL && size == 0);
    fail |= CHECK(kc_kv_at(kv, 0, &key, &value, NULL) == KC_KV_EINVAL);
    fail |= CHECK(key == NULL && value == NULL);
    kc_kv_close(kv);
    return fail;
}

/**
 * Verify lookup errors, missing keys, and cleared outputs.
 * @return Zero when lookups report errors without stale data.
 */
static int case_lookup(void)
{
    kc_kv_t *kv = create();
    const void *value = (const void *)1;
    size_t size = 44;
    int fail = 0;

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_get(kv, "missing", &value, &size) == KC_KV_NOT_FOUND);
    fail |= CHECK(value == NULL && size == 0);
    fail |= CHECK(kc_kv_get(kv, "", &value, &size) == KC_KV_EINVAL);
    fail |= CHECK(kc_kv_get(NULL, "key", &value, &size) == KC_KV_EINVAL);
    fail |= CHECK(kc_kv_get(kv, NULL, &value, &size) == KC_KV_EINVAL);
    fail |= CHECK(kc_kv_get(kv, "key", NULL, &size) == KC_KV_EINVAL);
    fail |= CHECK(size == 0);
    fail |= CHECK(kc_kv_get(kv, "key", &value, NULL) == KC_KV_EINVAL);
    fail |= CHECK(value == NULL);
    kc_kv_close(kv);
    return fail;
}

/**
 * Verify invalid mutations leave the collection unchanged.
 * @return Zero when rejected operations preserve content.
 */
static int case_mutation_errors(void)
{
    kc_kv_t *kv = create();
    int fail = 0;

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_set(kv, "keep", "data", 4) == KC_KV_OK);
    fail |= CHECK(kc_kv_set(NULL, "key", "x", 1) == KC_KV_EINVAL);
    fail |= CHECK(kc_kv_set(kv, NULL, "x", 1) == KC_KV_EINVAL);
    fail |= CHECK(kc_kv_set(kv, "", "x", 1) == KC_KV_EINVAL);
    fail |= CHECK(kc_kv_set(kv, "keep", NULL, 1) == KC_KV_EINVAL);
    fail |= CHECK(kc_kv_del(NULL, "key") == KC_KV_EINVAL);
    fail |= CHECK(kc_kv_del(kv, NULL) == KC_KV_EINVAL);
    fail |= CHECK(kc_kv_del(kv, "") == KC_KV_EINVAL);
    fail |= CHECK(kc_kv_count(kv) == 1);
    fail |= CHECK(value_equals(kv, "keep", "data", 4) == 0);
    kc_kv_close(kv);
    return fail;
}

/**
 * Verify the portable empty snapshot and its imported lifetime.
 * @return Zero when the empty snapshot is portable.
 */
static int case_empty_snapshot(void)
{
    kc_kv_t *kv = create(), *copy = NULL;
    void *bytes = NULL;
    size_t length = 0;
    int fail = 0;
    const unsigned char magic[8] = {'K', 'C', 'K', 'V', 'B', 'I', 'N', '\n'};

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_export(kv, &bytes, &length) == KC_KV_OK);
    if (bytes == NULL) fail = 1;
    else {
        fail |= CHECK(length == 20);
        fail |= CHECK(memcmp(bytes, magic, sizeof(magic)) == 0);
        fail |= CHECK(((unsigned char *)bytes)[8] == 1);
        fail |= CHECK(kc_kv_open(&copy, &(kc_kv_options_t){.data = bytes, .size = length}) == KC_KV_OK);
        if (copy != NULL) fail |= CHECK(kc_kv_count(copy) == 0);
        else fail = 1;
    }
    kc_kv_close(copy);
    kc_kv_close(kv);

    return fail;
}

/**
 * Verify snapshots preserve binary content and outlive their sources.
 * @return Zero when data is independent and round-trips.
 */
static int case_snapshot(void)
{
    kc_kv_t *original = create(), *copy = NULL;
    void *bytes = NULL, *again = NULL;
    size_t length = 0, again_size = 0;
    const unsigned char data[] = {0, 255, 3, 0, 17};
    int fail = 0;

    if (original == NULL) return 1;
    fail |= CHECK(kc_kv_set(original, "bin", data, sizeof(data)) == KC_KV_OK);
    fail |= CHECK(kc_kv_set(original, "empty", NULL, 0) == KC_KV_OK);
    fail |= CHECK(kc_kv_export(original, &bytes, &length) == KC_KV_OK);
    if (bytes == NULL) fail = 1;
    else {
        fail |= CHECK(kc_kv_open(&copy, &(kc_kv_options_t){.data = bytes, .size = length}) == KC_KV_OK);
        if (copy == NULL) fail = 1;
        else {
            fail |= CHECK(value_equals(copy, "bin", data, sizeof(data)) == 0);
            fail |= CHECK(value_equals(copy, "empty", NULL, 0) == 0);
            fail |= CHECK(kc_kv_export(copy, &again, &again_size) == KC_KV_OK);
            fail |= CHECK(again != NULL && again_size == length);
            if (again != NULL && again_size == length)
                fail |= CHECK(memcmp(bytes, again, length) == 0);
            fail |= CHECK(kc_kv_set(original, "extra", "new", 3) == KC_KV_OK);
            fail |= CHECK(kc_kv_count(original) == 3);
            if (again != NULL && again_size == length)
                fail |= CHECK(memcmp(bytes, again, length) == 0);
            kc_kv_close(original);
            original = NULL;
            fail |= CHECK(value_equals(copy, "bin", data, sizeof(data)) == 0);
        }
    }
    kc_kv_close(original);
    kc_kv_close(copy);

    return fail;
}

/**
 * Verify exports are deterministic regardless of insertion order.
 * @return Zero when snapshots are canonically ordered.
 */
static int case_deterministic(void)
{
    kc_kv_t *a = create(), *b = create();
    void *bytes_a = NULL, *bytes_b = NULL;
    size_t size_a = 0, size_b = 0;
    int fail = 0;

    if (a == NULL || b == NULL) { kc_kv_close(a); kc_kv_close(b); return 1; }
    fail |= CHECK(kc_kv_set(a, "z", "last", 4) == KC_KV_OK);
    fail |= CHECK(kc_kv_set(a, "a", "first", 5) == KC_KV_OK);
    fail |= CHECK(kc_kv_set(b, "a", "first", 5) == KC_KV_OK);
    fail |= CHECK(kc_kv_set(b, "z", "last", 4) == KC_KV_OK);
    fail |= CHECK(kc_kv_export(a, &bytes_a, &size_a) == KC_KV_OK);
    fail |= CHECK(kc_kv_export(b, &bytes_b, &size_b) == KC_KV_OK);
    fail |= CHECK(bytes_a != NULL && bytes_b != NULL && size_a == size_b);
    if (bytes_a != NULL && bytes_b != NULL && size_a == size_b)
        fail |= CHECK(memcmp(bytes_a, bytes_b, size_a) == 0);
    kc_kv_close(a);
    kc_kv_close(b);
    return fail;
}

/**
 * Reject invalid snapshot headers, versions, counts and lengths.
 * @return Zero when malformed headers are safely rejected.
 */
static int case_snapshot_header(void)
{
    kc_kv_t *kv = create();
    void *bytes = NULL;
    size_t size = 0;
    int fail = 0;

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_export(kv, &bytes, &size) == KC_KV_OK);
    if (bytes != NULL && size == 20) {
        unsigned char copy[21];

        fail |= rejects(NULL, 0);
        for (size_t i = 0; i < size; i++) fail |= rejects(bytes, i);
        memcpy(copy, bytes, size);
        copy[0] = 'X';
        fail |= rejects(copy, size);
        memcpy(copy, bytes, size);
        copy[8] = 2;
        fail |= rejects(copy, size);
        memcpy(copy, bytes, size);
        copy[12] = 1;
        fail |= rejects(copy, size);
        memcpy(copy, bytes, size);
        copy[19] = 255;
        fail |= rejects(copy, size);
        memcpy(copy, bytes, size);
        copy[20] = 7;
        fail |= rejects(copy, size + 1);
    } else fail = 1;
    kc_kv_close(kv);

    return fail;
}

/**
 * Reject malformed record boundaries, key order and binary lengths.
 * @return Zero when malformed record data is rejected.
 */
static int case_snapshot_records(void)
{
    kc_kv_t *kv = create();
    void *bytes = NULL;
    size_t size = 0, second;
    int fail = 0;

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_set(kv, "aa", "1", 1) == KC_KV_OK);
    fail |= CHECK(kc_kv_set(kv, "bb", "2", 1) == KC_KV_OK);
    fail |= CHECK(kc_kv_export(kv, &bytes, &size) == KC_KV_OK);
    second = 20U + 12U + 2U + 1U;
    if (bytes != NULL && size == 20U + 2U * 15U) {
        unsigned char *copy = (unsigned char *)malloc(size);

        if (copy == NULL) fail = 1;
        else {
            memcpy(copy, bytes, size);
            copy[20] = 0;
            fail |= rejects(copy, size);
            memcpy(copy, bytes, size);
            copy[24] = 255;
            copy[25] = 255;
            copy[26] = 255;
            copy[27] = 255;
            copy[28] = 255;
            copy[29] = 255;
            copy[30] = 255;
            copy[31] = 255;
            fail |= rejects(copy, size);
            memcpy(copy, bytes, size);
            copy[32 + 1] = 0;
            fail |= rejects(copy, size);
            memcpy(copy, bytes, size);
            copy[second + 12] = 'a';
            copy[second + 13] = 'a';
            fail |= rejects(copy, size);
            memcpy(copy, bytes, size);
            copy[second + 12] = 'A';
            fail |= rejects(copy, size);
            memcpy(copy, bytes, size);
            copy[second] = 255;
            fail |= rejects(copy, size);
            fail |= rejects(bytes, size - 1);
            free(copy);
        }
    } else fail = 1;
    kc_kv_close(kv);

    return fail;
}

/**
 * Verify large collections support interleaved edits and sorted traversal.
 * @return Zero when growth and mutation preserve every surviving record.
 */
static int case_growth(void)
{
    kc_kv_t *kv = create(), *copy = NULL;
    void *bytes = NULL;
    size_t length = 0;
    int fail = 0;

    if (kv == NULL) return 1;
    for (int i = 511; i >= 0; i--) {
        char key[16], value[16];

        snprintf(key, sizeof(key), "k%04d", i);
        snprintf(value, sizeof(value), "v%04d", i);
        fail |= CHECK(kc_kv_set(kv, key, value, 5) == KC_KV_OK);
    }
    fail |= CHECK(kc_kv_count(kv) == 512);
    for (int i = 0; i < 512; i += 2) {
        char key[16];

        snprintf(key, sizeof(key), "k%04d", i);
        fail |= CHECK(kc_kv_del(kv, key) == KC_KV_OK);
    }
    fail |= CHECK(kc_kv_count(kv) == 256);
    for (int i = 1; i < 512; i += 2) {
        char key[16], value[16];

        snprintf(key, sizeof(key), "k%04d", i);
        snprintf(value, sizeof(value), "v%04d", i);
        fail |= CHECK(value_equals(kv, key, value, 5) == 0);
    }
    fail |= CHECK(kc_kv_export(kv, &bytes, &length) == KC_KV_OK);
    if (bytes == NULL) fail = 1;
    else {
        fail |= CHECK(kc_kv_open(&copy, &(kc_kv_options_t){.data = bytes, .size = length}) == KC_KV_OK);
        if (copy != NULL) fail |= CHECK(kc_kv_count(copy) == 256);
        else fail = 1;
    }
    kc_kv_close(copy);
    kc_kv_close(kv);

    return fail;
}

/**
 * Verify output pointer contracts and public status descriptions.
 * @return Zero when metadata and ownership APIs behave correctly.
 */
static int case_contract(void)
{
    kc_kv_t *kv = create(), *loaded = NULL;
    void *data = (void *)1;
    size_t size = 99;
    int fail = 0;

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_export(NULL, &data, &size) == KC_KV_EINVAL);
    fail |= CHECK(data == NULL && size == 0);
    fail |= CHECK(kc_kv_export(kv, NULL, &size) == KC_KV_EINVAL);
    fail |= CHECK(size == 0);
    fail |= CHECK(kc_kv_export(kv, &data, NULL) == KC_KV_EINVAL);
    fail |= CHECK(data == NULL);
    fail |= CHECK(kc_kv_open(NULL, &(kc_kv_options_t){.data = NULL, .size = 0}) == KC_KV_EINVAL);
    fail |= CHECK(kc_kv_open(&loaded, &(kc_kv_options_t){.data = NULL, .size = 0}) == KC_KV_EFORMAT);
    fail |= CHECK(loaded == NULL);
    fail |= CHECK((strcmp(kc_kv_version(), KC_MANIFEST_VERSION) == 0));
    fail |= CHECK(strcmp(kc_kv_strerror(KC_KV_OK), "Success") == 0);
    fail |= CHECK(strcmp(kc_kv_strerror(KC_KV_NOT_FOUND), "Key not found") == 0);
    fail |= CHECK(strcmp(kc_kv_strerror(KC_KV_EINVAL), "Invalid argument") == 0);
    fail |= CHECK(strcmp(kc_kv_strerror(KC_KV_ENOMEM), "Out of memory") == 0);
    fail |= CHECK(strcmp(kc_kv_strerror(KC_KV_EFORMAT), "Invalid binary value") == 0);
    fail |= CHECK(strcmp(kc_kv_strerror(999), "Unknown error") == 0);
    kc_kv_close(NULL);
    kc_kv_close(kv);
    return fail;
}

#ifndef __EMSCRIPTEN__

#ifndef KC_CLI_PATH
#define KC_CLI_PATH "kv"
#endif

/**
 * Write a binary fixture file.
 * @param path Destination path.
 * @param bytes Content bytes.
 * @param size Byte count.
 * @return Zero on success, nonzero otherwise.
 */
static int cli_write(const char *path, const void *bytes, size_t size)
{
    FILE *file = fopen(path, "wb");
    int failed;

    if (file == NULL) return 1;
    failed = size != 0 && fwrite(bytes, 1, size, file) != size;
    if (fclose(file) != 0) failed = 1;
    return failed;
}

/**
 * Execute the command-line frontend with optional binary stdin and stdout.
 * @param args Command arguments.
 * @param input Optional input file.
 * @param output Output file.
 * @return Zero when command exits successfully.
 */
static int cli_run(const char *args, const char *input, const char *output)
{
    char command[4096], redirect[256];

    if (input != NULL) snprintf(redirect, sizeof(redirect), " < %s", input);
    else redirect[0] = '\0';
#ifdef _WIN32
    snprintf(command, sizeof(command), "\"\"%s\" %s%s > %s 2>NUL\"",
        KC_CLI_PATH, args, redirect, output);
#else
    snprintf(command, sizeof(command), "\"%s\" %s%s > %s 2>/dev/null",
        KC_CLI_PATH, args, redirect, output);
#endif
    return system(command) == 0 ? 0 : 1;
}

/**
 * Verify a captured output file contains exactly the expected bytes.
 * @param path Captured file.
 * @param expected Expected bytes.
 * @param size Exact length.
 * @return Zero on match, one otherwise.
 */
static int cli_matches(const char *path, const void *expected, size_t size)
{
    FILE *file = fopen(path, "rb");
    unsigned char chunk[4096];
    size_t offset = 0, read;
    int fail = 0;

    if (file == NULL) return 1;
    while ((read = fread(chunk, 1, sizeof(chunk), file)) != 0) {
        if (read > size - offset ||
            (read != 0 && memcmp(chunk, (const unsigned char *)expected + offset, read) != 0)) {
            fail = 1;
            break;
        }
        offset += read;
    }
    fail |= ferror(file) != 0 || offset != size;
    if (fclose(file) != 0) fail = 1;
    return fail;
}

/**
 * Prepare a complete binary collection without invoking CLI commands.
 * @return Zero on success, one otherwise.
 */
static int cli_fixture(void)
{
    kc_kv_t *kv = create();
    void *bytes = NULL;
    size_t size = 0;
    int fail = 0;

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_set(kv, "theme", "dark", 4) == KC_KV_OK);
    fail |= CHECK(kc_kv_set(kv, "lang", "es", 2) == KC_KV_OK);
    fail |= CHECK(kc_kv_export(kv, &bytes, &size) == KC_KV_OK);
    if (bytes == NULL) fail = 1;
    else fail |= cli_write("kv-cli-in.bin", bytes, size);

    kc_kv_close(kv);
    return fail;
}

/**
 * Read a CLI output snapshot and check its contents through public C API.
 * @param key Expected key.
 * @param expected Expected value bytes.
 * @param size Expected length.
 * @param expected_count Expected collection count.
 * @return Zero when valid.
 */
static int cli_collection(const char *key, const void *expected,
    size_t size, size_t expected_count)
{
    FILE *file = fopen("kv-cli-out.bin", "rb");
    unsigned char *bytes = NULL;
    size_t length = 0, capacity = 0, n;
    unsigned char chunk[4096];
    kc_kv_t *kv = NULL;
    int fail = 0;

    if (file == NULL) return 1;
    while ((n = fread(chunk, 1, sizeof(chunk), file)) != 0) {
        unsigned char *next;
        size_t needed;

        if (length > SIZE_MAX - n) { fail = 1; break; }
        needed = length + n;
        if (needed > capacity) {
            next = (unsigned char *)realloc(bytes, needed);
            if (next == NULL) { fail = 1; break; }
            bytes = next;
            capacity = needed;
        }
        memcpy(bytes + length, chunk, n);
        length = needed;
    }
    fail |= ferror(file) != 0;
    if (fclose(file) != 0) fail = 1;
    if (!fail) fail |= CHECK(kc_kv_open(&kv, &(kc_kv_options_t){.data = bytes, .size = length}) == KC_KV_OK);
    if (kv != NULL) {
        fail |= CHECK(kc_kv_count(kv) == expected_count);
        if (key != NULL) fail |= CHECK(value_equals(kv, key, expected, size) == 0);
    } else fail = 1;
    kc_kv_close(kv);
    free(bytes);
    return fail;
}

/**
 * Verify canonical command help and version.
 * @return Zero on success.
 */
static int case_cli_help(void)
{
    int fail = cli_run("--help", NULL, "kv-cli-out.bin");

    return fail || cli_run("-h", NULL, "kv-cli-out.bin");
}

/**
 * Verify CLI semantic version matches native library.
 * @return Zero on success.
 */
static int case_cli_version(void)
{
    char version[64];
    int size = snprintf(version, sizeof(version), "%s\n", kc_kv_version());

    return cli_run("--version", NULL, "kv-cli-out.bin") ||
        size < 0 || cli_matches("kv-cli-out.bin", version, (size_t)size);
}

/**
 * Verify new creates a portable empty collection.
 * @return Zero on success.
 */
static int case_cli_new(void)
{
    return cli_run("new", NULL, "kv-cli-out.bin") ||
        cli_collection(NULL, NULL, 0, 0);
}

/**
 * Verify CLI writes text without interpreting the collection storage.
 * @return Zero on success.
 */
static int case_cli_set(void)
{
    return cli_fixture() ||
        cli_run("set theme light", "kv-cli-in.bin", "kv-cli-out.bin") ||
        cli_collection("theme", "light", 5, 2);
}

/**
 * Verify direct raw-value retrieval.
 * @return Zero on success.
 */
static int case_cli_get(void)
{
    return cli_fixture() ||
        cli_run("get theme", "kv-cli-in.bin", "kv-cli-out.bin") ||
        cli_matches("kv-cli-out.bin", "dark", 4);
}

/**
 * Verify CLI deletion produces the modified collection.
 * @return Zero on success.
 */
static int case_cli_del(void)
{
    return cli_fixture() ||
        cli_run("del theme", "kv-cli-in.bin", "kv-cli-out.bin") ||
        cli_collection("lang", "es", 2, 1);
}

/**
 * Verify counting includes distinct keys.
 * @return Zero on success.
 */
static int case_cli_count(void)
{
    return cli_fixture() ||
        cli_run("count", "kv-cli-in.bin", "kv-cli-out.bin") ||
        cli_matches("kv-cli-out.bin", "2\n", 2);
}

/**
 * Verify CLI lists keys in deterministic order.
 * @return Zero on success.
 */
static int case_cli_list(void)
{
    return cli_fixture() ||
        cli_run("list", "kv-cli-in.bin", "kv-cli-out.bin") ||
        cli_matches("kv-cli-out.bin", "lang\ntheme\n", 11);
}

/**
 * Verify zero-based indexed value reads.
 * @return Zero on success.
 */
static int case_cli_at(void)
{
    return cli_fixture() ||
        cli_run("at 0", "kv-cli-in.bin", "kv-cli-out.bin") ||
        cli_matches("kv-cli-out.bin", "es", 2);
}

/**
 * Verify binary values containing NUL are preserved through CLI.
 * @return Zero on success.
 */
static int case_cli_binary(void)
{
    static const unsigned char value[] = {0, 255, 9, 0, 10};
    int fail = cli_fixture();

    fail |= cli_write("kv-cli-value.bin", value, sizeof(value));
    if (fail) return 1;
    fail |= cli_run("set pixels --value-file kv-cli-value.bin",
        "kv-cli-in.bin", "kv-cli-out.bin");
    if (!fail) fail |= cli_collection("pixels", value, sizeof(value), 3);
    return fail;
}

/**
 * Verify an explicitly empty value file is not a deletion.
 * @return Zero on success.
 */
static int case_cli_empty(void)
{
    return cli_fixture() || cli_write("kv-cli-value.bin", NULL, 0) ||
        cli_run("set empty --value-file kv-cli-value.bin",
            "kv-cli-in.bin", "kv-cli-out.bin") ||
        cli_collection("empty", NULL, 0, 3);
}

/**
 * Verify absent keys and invalid indexed lookups fail explicitly.
 * @return Zero on success.
 */
static int case_cli_missing(void)
{
    return cli_fixture() ||
        !cli_run("get missing", "kv-cli-in.bin", "kv-cli-out.bin") ||
        !cli_run("del missing", "kv-cli-in.bin", "kv-cli-out.bin") ||
        !cli_run("at 2", "kv-cli-in.bin", "kv-cli-out.bin") ||
        !cli_run("at -1", "kv-cli-in.bin", "kv-cli-out.bin");
}

/**
 * Verify malformed snapshots and CLI arguments are rejected.
 * @return Zero on success.
 */
static int case_cli_errors(void)
{
    int fail = 0;

    fail |= cli_fixture();
    fail |= !cli_run("new unexpected", NULL, "kv-cli-out.bin");
    fail |= !cli_run("unknown", "kv-cli-in.bin", "kv-cli-out.bin");
    fail |= !cli_run("set theme", "kv-cli-in.bin", "kv-cli-out.bin");
    fail |= !cli_run("set theme --value-file missing-kv-file.bin",
        "kv-cli-in.bin", "kv-cli-out.bin");
    fail |= !cli_run("count extra", "kv-cli-in.bin", "kv-cli-out.bin");
    fail |= !cli_run("at nope", "kv-cli-in.bin", "kv-cli-out.bin");
    fail |= cli_write("kv-cli-invalid.bin", "bad", 3);
    if (!fail) fail |= !cli_run("list", "kv-cli-invalid.bin", "kv-cli-out.bin");
    return fail;
}

/**
 * Verify special characters in keys are escaped on a single output line.
 * @return Zero on success.
 */
static int case_cli_escaping(void)
{
    kc_kv_t *kv = create();
    void *bytes = NULL;
    size_t size = 0;
    int fail = 0;

    if (kv == NULL) return 1;
    fail |= CHECK(kc_kv_set(kv, "line\nbreak", "a", 1) == KC_KV_OK);
    fail |= CHECK(kc_kv_set(kv, "tab\tkey", "b", 1) == KC_KV_OK);
    fail |= CHECK(kc_kv_set(kv, "slash\\key", "c", 1) == KC_KV_OK);
    fail |= CHECK(kc_kv_export(kv, &bytes, &size) == KC_KV_OK);
    if (bytes == NULL) fail = 1;
    else fail |= cli_write("kv-cli-in.bin", bytes, size);

    kc_kv_close(kv);
    if (!fail) fail |= cli_run("list", "kv-cli-in.bin", "kv-cli-out.bin");
    if (!fail) {
        const char *expected = "line\\nbreak\nslash\\\\key\ntab\\tkey\n";
        fail |= cli_matches("kv-cli-out.bin", expected, strlen(expected));
    }
    return fail;
}

/**
 * Verify collection transformations work through a shell pipeline.
 * @return Zero on success.
 */
static int case_cli_pipeline(void)
{
    char command[4096];

#ifdef _WIN32
    snprintf(command, sizeof(command),
        "\"\"%s\" new | \"%s\" set language es | \"%s\" set theme dark > kv-cli-out.bin 2>NUL\"",
        KC_CLI_PATH, KC_CLI_PATH, KC_CLI_PATH);
#else
    snprintf(command, sizeof(command),
        "\"%s\" new | \"%s\" set language es | \"%s\" set theme dark > kv-cli-out.bin 2>/dev/null",
        KC_CLI_PATH, KC_CLI_PATH, KC_CLI_PATH);
#endif
    if (system(command) != 0) return 1;
    return cli_collection("theme", "dark", 4, 2);
}

#endif

static const case_item_t cases[] = {
    {"open", "creates an empty collection and handles NULL cleanup", case_open},
    {"set", "copies arbitrary binary values and caller-owned keys", case_set},
    {"replace", "replaces existing values without adding duplicate keys", case_replace},
    {"empty", "distinguishes an empty value from a missing key", case_empty},
    {"delete", "removes first, middle, and last entries and allows reinsert", case_delete},
    {"ordering", "enumerates bytewise case-sensitive key order", case_ordering},
    {"iteration", "checks bounds and clears invalid iteration outputs", case_iteration},
    {"lookup", "rejects invalid lookups and clears stale outputs", case_lookup},
    {"mutation_errors", "rejects invalid mutations without changing content", case_mutation_errors},
    {"empty_snapshot", "round-trips the portable empty binary representation", case_empty_snapshot},
    {"snapshot", "round-trips bytes and owns imported content independently", case_snapshot},
    {"deterministic", "exports identical bytes regardless of insertion order", case_deterministic},
    {"snapshot_header", "rejects truncated and malformed snapshot headers", case_snapshot_header},
    {"snapshot_records", "rejects corrupt boundaries, keys, and value lengths", case_snapshot_records},
    {"growth", "handles hundreds of insertions, removals, and imports", case_growth},
    {"contract", "validates output arguments, cleanup, version and errors", case_contract}
#ifndef __EMSCRIPTEN__
    ,{"cli_help", "help and short help options succeed", case_cli_help}
    ,{"cli_version", "prints the library semantic version", case_cli_version}
    ,{"cli_new", "creates a portable empty snapshot", case_cli_new}
    ,{"cli_set", "updates text values in a binary collection", case_cli_set}
    ,{"cli_get", "returns an exact raw value", case_cli_get}
    ,{"cli_del", "deletes a key and returns the collection", case_cli_del}
    ,{"cli_count", "counts distinct keys", case_cli_count}
    ,{"cli_list", "lists keys in sorted order", case_cli_list}
    ,{"cli_at", "retrieves an indexed binary value", case_cli_at}
    ,{"cli_binary", "preserves a binary value read from a file", case_cli_binary}
    ,{"cli_empty", "keeps a present empty binary value", case_cli_empty}
    ,{"cli_missing", "rejects missing keys and invalid indexes", case_cli_missing}
    ,{"cli_errors", "rejects malformed snapshots and arguments", case_cli_errors}
    ,{"cli_escaping", "escapes control characters in listed keys", case_cli_escaping}
    ,{"cli_pipeline", "composes binary transformations through pipes", case_cli_pipeline}
#endif
};

/**
 * Execute a selected test or the complete suite.
 * @param argc Command argument count.
 * @param argv Command arguments.
 * @return Zero on success, nonzero on failed contract.
 */
static int test_dispatch(int argc, char **argv)
{
    const size_t count = sizeof(cases) / sizeof(cases[0]);
    size_t selected = 0, total = count;
    int failures = 0;

    if (argc != 2) {
        fprintf(stderr, "test case: expected one argument\n");
        return 2;
    }
    if (strcmp(argv[1], "all") != 0) {
        for (selected = 0; selected < count; selected++)
            if (strcmp(argv[1], cases[selected].name) == 0) break;
        if (selected == count) {
            fprintf(stderr, "unknown test case: %s\n", argv[1]);
            return 2;
        }
        total = 1;
    }
    for (size_t i = 0; i < total; i++) {
        const case_item_t *item = &cases[total == 1 ? selected : i];
        int fail = item->run() != 0;

        failures += fail;
        printf("[%zu/%zu] [%s] %s: %s\n", i + 1, total,
            fail ? "FAIL" : "PASS", item->name, item->detail);
    }
    printf("%zu passed, %d failed\n", total - (size_t)failures, failures);
    return failures ? 1 : 0;
}

#ifndef __EMSCRIPTEN__
/**
 * Run contract fixtures in a temporary workspace and clean up afterwards.
 * @param argc Command argument count.
 * @param argv Command arguments.
 * @return Contract runner status, or two on workspace failure.
 */
int main(int argc, char **argv)
{
    static const char *const files[] = {
        "kv-cli-in.bin", "kv-cli-out.bin", "kv-cli-value.bin",
        "kv-cli-invalid.bin"
    };
    char previous[4096];
    char workspace[4096];
    int status;

#ifdef _WIN32
    char temporary[MAX_PATH];
    DWORD length = GetCurrentDirectoryA((DWORD)sizeof(previous), previous);
    DWORD prefix = GetTempPathA((DWORD)sizeof(temporary), temporary);

    if (length == 0 || length >= sizeof(previous) ||
        prefix == 0 || prefix >= sizeof(temporary) ||
        !GetTempFileNameA(temporary, "kv-", 0, workspace) ||
        !DeleteFileA(workspace) || !CreateDirectoryA(workspace, NULL) ||
        !SetCurrentDirectoryA(workspace)) {
        fprintf(stderr, "Cannot create KV test workspace\n");
        return 2;
    }
#else
    const char *root = getenv("TMPDIR");
    int length;

    if (getcwd(previous, sizeof(previous)) == NULL) {
        fprintf(stderr, "Cannot determine KV test working directory\n");
        return 2;
    }
    if (root == NULL || !*root) root = "/tmp";
    length = snprintf(workspace, sizeof(workspace), "%s/kv-test-XXXXXX",
        root);
    if (length < 0 || (size_t)length >= sizeof(workspace) ||
        mkdtemp(workspace) == NULL || chdir(workspace) != 0) {
        fprintf(stderr, "Cannot create KV test workspace\n");
        return 2;
    }
#endif

    status = test_dispatch(argc, argv);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        remove(files[i]);
#ifdef _WIN32
    if (!SetCurrentDirectoryA(previous) || !RemoveDirectoryA(workspace)) {
#else
    if (chdir(previous) != 0 || rmdir(workspace) != 0) {
#endif
        fprintf(stderr, "Cannot remove KV test workspace\n");
        return 2;
    }
    return status;
}
#else
/**
 * Run WebAssembly contracts without native filesystem fixtures.
 * @param argc Command argument count.
 * @param argv Command arguments.
 * @return Contract runner exit status.
 */
int main(int argc, char **argv)
{
    return test_dispatch(argc, argv);
}
#endif
