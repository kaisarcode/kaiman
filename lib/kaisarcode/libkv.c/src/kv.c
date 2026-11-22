/**
 * kv.c - In-memory key-value command-line interface.
 * Summary: Transforms and queries portable binary collections via standard streams.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#include "libkv.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/**
 * Print command help.
 * @param program Executable name.
 * @return None.
 */
static void kv_usage(const char *program)
{
    printf("Usage: %s <command> [arguments] < collection.kv\n", program);
    printf("\n");
    printf("Commands:\n");
    printf("  new                         Write an empty binary collection\n");
    printf("  set <key> <text>            Insert/replace a UTF-8 text value\n");
    printf("  set <key> --value-file <path>  Insert/replace bytes from a file\n");
    printf("  get <key>                   Write value bytes to stdout\n");
    printf("  del <key>                   Delete a key and write updated collection\n");
    printf("  count                       Print the number of keys\n");
    printf("  list                        Print keys, one escaped key per line\n");
    printf("  at <index>                  Write value bytes at a sorted index\n");
    printf("\n");
    printf("new requires no input. Other commands read a binary collection from stdin.\n");
    printf("set and del write the updated binary collection to stdout.\n");
    printf("--value-file reads the value only; stdin always holds the collection.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help                  Show this help\n");
    printf("  -v, --version               Show version\n");
}

/**
 * Read a byte stream into caller-owned memory.
 * @param stream Input stream.
 * @param out Receives allocated bytes.
 * @param out_size Receives byte count.
 * @return Zero on success, nonzero otherwise.
 */
static int kv_read(FILE *stream, unsigned char **out, size_t *out_size)
{
    unsigned char *data = NULL;
    size_t size = 0, capacity = 0, count;
    unsigned char chunk[8192];

    *out = NULL;
    *out_size = 0;
    while ((count = fread(chunk, 1, sizeof(chunk), stream)) != 0) {
        unsigned char *next;
        size_t needed;
        size_t expanded;

        if (size > SIZE_MAX - count) {
            free(data);
            return 1;
        }
        needed = size + count;
        if (needed > capacity) {
            expanded = capacity == 0 ? sizeof(chunk) : capacity;
            while (expanded < needed) {
                if (expanded > SIZE_MAX / 2U) {
                    expanded = needed;
                    break;
                }
                expanded *= 2U;
            }
            next = (unsigned char *)realloc(data, expanded);
            if (next == NULL) {
                free(data);
                return 1;
            }
            data = next;
            capacity = expanded;
        }
        memcpy(data + size, chunk, count);
        size = needed;
    }
    if (ferror(stream)) {
        free(data);
        return 1;
    }
    *out = data;
    *out_size = size;
    return 0;
}

/**
 * Read a binary value from a file without interpreting its contents.
 * @param path File path.
 * @param out Receives allocated bytes.
 * @param out_size Receives byte count.
 * @return Zero on success, nonzero otherwise.
 */
static int kv_read_file(const char *path, unsigned char **out, size_t *out_size)
{
    FILE *file;
    int status;

    if (path == NULL || !*path || strcmp(path, "-") == 0) return 1;
    file = fopen(path, "rb");
    if (file == NULL) return 1;
    status = kv_read(file, out, out_size);
    if (fclose(file) != 0) status = 1;
    return status;
}

/**
 * Write bytes to standard output, preserving binary content.
 * @param data Source bytes, or NULL when size is zero.
 * @param size Byte count.
 * @return Zero on success, nonzero otherwise.
 */
static int kv_write(const void *data, size_t size)
{
    if (size != 0 && fwrite(data, 1, size, stdout) != size) return 1;
    return fflush(stdout) != 0;
}

/**
 * Write a canonical binary snapshot of the collection.
 * @param kv Collection to export.
 * @return Process status.
 */
static int kv_export(kc_kv_t *kv)
{
    void *bytes = NULL;
    size_t size = 0;
    int status = kc_kv_export(kv, &bytes, &size);

    if (status != KC_KV_OK) {
        fprintf(stderr, "kv: %s\n", kc_kv_strerror(status));
        return 1;
    }
    status = kv_write(bytes, size);
    
    if (status != 0) fprintf(stderr, "kv: cannot write stdout\n");
    return status;
}

/**
 * Load the supplied serialized collection from standard input.
 * @param out Receives the collection handle.
 * @return Zero on success, nonzero otherwise.
 */
static int kv_input(kc_kv_t **out)
{
    unsigned char *bytes = NULL;
    size_t size = 0;
    int status;

    if (kv_read(stdin, &bytes, &size) != 0) {
        fprintf(stderr, "kv: cannot read stdin\n");
        return 1;
    }
    status = kc_kv_open(out, &(kc_kv_options_t){.data = bytes, .size = size});
    free(bytes);
    if (status != KC_KV_OK) {
        fprintf(stderr, "kv: %s\n", kc_kv_strerror(status));
        return 1;
    }
    return 0;
}

/**
 * Parse a nonnegative zero-based key index.
 * @param text Decimal integer text.
 * @param out Parsed index.
 * @return Zero on success, nonzero otherwise.
 */
static int kv_index(const char *text, size_t *out)
{
    unsigned long long number;
    char *end;

    if (text == NULL || !*text || *text == '-') return 1;
    errno = 0;
    number = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || number > SIZE_MAX) return 1;
    *out = (size_t)number;
    return 0;
}

/**
 * Render a possibly control-containing key as an unambiguous single line.
 * @param key Key to render.
 * @return Zero on success, nonzero otherwise.
 */
static int kv_print_key(const char *key)
{
    const unsigned char *p = (const unsigned char *)key;

    for (; *p != 0; p++) {
        if (*p == '\\') {
            if (fputs("\\\\", stdout) == EOF) return 1;
        } else if (*p == '\n') {
            if (fputs("\\n", stdout) == EOF) return 1;
        } else if (*p == '\r') {
            if (fputs("\\r", stdout) == EOF) return 1;
        } else if (*p == '\t') {
            if (fputs("\\t", stdout) == EOF) return 1;
        } else if (*p < 32U || *p == 127U) {
            if (fprintf(stdout, "\\x%02X", (unsigned int)*p) < 0) return 1;
        } else if (fputc(*p, stdout) == EOF) return 1;
    }
    return fputc('\n', stdout) == EOF;
}

/**
 * Execute a key-value operation on an imported collection.
 * @param kv Imported collection.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Process status.
 */
static int kv_command(kc_kv_t *kv, int argc, char **argv)
{
    const char *command = argv[1];
    const void *value = NULL;
    const char *key = NULL;
    size_t size = 0;
    int status;

    if (strcmp(command, "set") == 0) {
        unsigned char *file = NULL;

        if ((argc != 4 && argc != 5) || !argv[2][0] ||
            (argc == 5 && strcmp(argv[3], "--value-file") != 0)) return 1;
        if (argc == 5) {
            if (kv_read_file(argv[4], &file, &size) != 0) {
                fprintf(stderr, "kv: cannot read value file '%s'\n", argv[4]);
                return 1;
            }
            value = file;
        } else {
            value = argv[3];
            size = strlen(argv[3]);
        }
        status = kc_kv_set(kv, argv[2], value, size);
        free(file);
        if (status == KC_KV_OK) return kv_export(kv);
    } else if (strcmp(command, "del") == 0) {
        if (argc != 3 || !argv[2][0]) return 1;
        status = kc_kv_del(kv, argv[2]);
        if (status == KC_KV_OK) return kv_export(kv);
    } else if (strcmp(command, "get") == 0) {
        if (argc != 3 || !argv[2][0]) return 1;
        status = kc_kv_get(kv, argv[2], &value, &size);
        if (status == KC_KV_OK) {
            status = kv_write(value, size) == 0 ? KC_KV_OK : KC_KV_EINVAL;
            if (status != KC_KV_OK) fprintf(stderr, "kv: cannot write stdout\n");
            return status == KC_KV_OK ? 0 : 1;
        }
    } else if (strcmp(command, "count") == 0) {
        if (argc != 2) return 1;
        if (printf("%zu\n", kc_kv_count(kv)) < 0 || fflush(stdout) != 0) {
            fprintf(stderr, "kv: cannot write stdout\n");
            return 1;
        }
        return 0;
    } else if (strcmp(command, "list") == 0) {
        if (argc != 2) return 1;
        for (size_t i = 0; i < kc_kv_count(kv); i++) {
            if (kc_kv_at(kv, i, &key, &value, &size) != KC_KV_OK ||
                kv_print_key(key) != 0) {
                fprintf(stderr, "kv: cannot list keys\n");
                return 1;
            }
        }
        return fflush(stdout) != 0;
    } else if (strcmp(command, "at") == 0) {
        size_t index;

        if (argc != 3 || kv_index(argv[2], &index) != 0) return 1;
        status = kc_kv_at(kv, index, &key, &value, &size);
        if (status == KC_KV_OK) {
            if (kv_write(value, size) == 0) return 0;
            fprintf(stderr, "kv: cannot write stdout\n");
            return 1;
        }
    } else {
        fprintf(stderr, "kv: unknown command '%s'\n", command);
        return 1;
    }
    fprintf(stderr, "kv: %s\n", kc_kv_strerror(status));
    return 1;
}

/**
 * Run the key-value CLI.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Zero on success, nonzero otherwise.
 */
int main(int argc, char **argv)
{
    kc_kv_t *kv = NULL;
    int status;

#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1) {
        fprintf(stderr, "kv: cannot configure binary I/O\n");
        return 1;
    }
#endif
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "--help") == 0)) {
        kv_usage(argv[0]);
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "-v") == 0 ||
        strcmp(argv[1], "--version") == 0)) {
        printf("%s\n", kc_kv_version());
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "new") == 0) {
        status = kc_kv_open(&kv, NULL);
        if (status != KC_KV_OK) {
            fprintf(stderr, "kv: %s\n", kc_kv_strerror(status));
            return 1;
        }
        status = kv_export(kv);
    } else {
        if (argc < 2 || (strcmp(argv[1], "set") != 0 &&
            strcmp(argv[1], "get") != 0 && strcmp(argv[1], "del") != 0 &&
            strcmp(argv[1], "count") != 0 && strcmp(argv[1], "list") != 0 &&
            strcmp(argv[1], "at") != 0)) {
            kv_usage(argv[0]);
            return 1;
        }
        status = kv_input(&kv);
        if (status == 0) status = kv_command(kv, argc, argv);
    }
    kc_kv_close(kv);
    return status;
}
