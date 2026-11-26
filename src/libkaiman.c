/**
 * libkaiman.c - Unified AI service management for kaiman.
 * Summary: Owns managed services, gateway settings and their persistence.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#include "libkaiman.h"

#include "json.h"
#include "keyring.h"
#include "libhttp.h"
#include "libnetl.h"
#include "upstream.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#define KAIMAN_MKDIR(path) _mkdir(path)
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define KAIMAN_MKDIR(path) mkdir((path), 0700)
#endif

#ifndef KC_KAIMAN_VERSION
#error "KC_KAIMAN_VERSION must come from manifest.json via CMake"
#endif

#define DEFAULT_LISTEN "127.0.0.1:49380"
#define DEFAULT_TIMEOUT 60

struct kc_kaiman_service {
    kc_kaiman_t *owner;
    kc_kaiman_service_t *next;
    int alive;
    char *name;
    char *adapter;
    char *base_url;
    char *model;
    char *api_key_env;
    char *token_command;
    char *auth_scheme;
    char *auth_user;
    char *keyring;
    int timeout;
};

struct kc_kaiman {
    char *path;
    char *listen;
    char *error;
    kc_kaiman_service_t *all;
    kc_kaiman_service_t **services;
    size_t count;
};

typedef struct {
    kc_netl_peer_t *peer;
    kc_http_parser_t *parser;
    kc_buffer_t *body;
    kc_kaiman_gateway_t *gateway;
    int failed;
} kc_kaiman_connection_t;

typedef struct {
    kc_netl_peer_t *peer;
    kc_kaiman_gateway_t *gateway;
    kc_buffer_t *sse;
    char *model;
    int headers_sent;
    int done;
    int status;
} kc_kaiman_stream_t;

struct kc_kaiman_gateway {
    kc_kaiman_t *manager;
    kc_netl_t *listener;
    kc_kaiman_connection_t *connections;
    size_t connection_count;
    kc_mutex_t mutex;
    volatile int stop;
    int mutex_ready;
    char *address;
    char *error;
};

static kc_kaiman_gateway_t *kc_kaiman_signal_gateway;

/**
 * Copy a NUL terminated string.
 * @param text Text to copy, or NULL.
 * @return Owned copy, or NULL for NULL input or allocation failure.
 */
static char *copy_text(const char *text) {
    size_t length;
    char *result;

    if (text == NULL) return NULL;
    length = strlen(text) + 1U;
    result = (char *)malloc(length);
    if (result != NULL) memcpy(result, text, length);
    return result;
}

/**
 * Store a detailed error message on a manager.
 * @param kaiman Destination manager, or NULL.
 * @param format Printf style format.
 * @return void
 */
static void set_error(kc_kaiman_t *kaiman, const char *format, ...) {
    char message[512];
    va_list args;

    if (kaiman == NULL) return;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    free(kaiman->error);
    kaiman->error = copy_text(message);
}

/**
 * Remove leading and trailing whitespace in place.
 * @param text Text to trim.
 * @return Pointer into text at the first non-whitespace byte.
 */
static char *trim(char *text) {
    char *end;

    while (*text != '\0' && isspace((unsigned char)*text)) text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return text;
}

/**
 * Validate a service name.
 * @param name Candidate name.
 * @return 1 when valid, 0 otherwise.
 */
static int valid_name(const char *name) {
    const char *cursor;

    if (name == NULL || *name == '\0') return 0;
    for (cursor = name; *cursor != '\0'; cursor++) {
        unsigned char value = (unsigned char)*cursor;
        if (!isalnum(value) && *cursor != '_' && *cursor != '-' &&
            *cursor != '.') {
            return 0;
        }
    }
    return 1;
}

/**
 * Validate that a value fits one configuration line.
 * @param value Candidate value.
 * @return 1 when valid, 0 otherwise.
 */
static int valid_value(const char *value) {
    const char *cursor;

    if (value == NULL) return 0;
    for (cursor = value; *cursor != '\0'; cursor++) {
        if (*cursor == '\n' || *cursor == '\r') return 0;
    }
    return 1;
}

/**
 * Validate an HTTP or HTTPS URL.
 * @param value Candidate URL.
 * @return 1 when valid, 0 otherwise.
 */
static int http_url(const char *value) {
    return strncmp(value, "http://", 7) == 0 ||
        strncmp(value, "https://", 8) == 0;
}

/**
 * Validate a host and port listen address.
 * @param value Candidate address.
 * @return 1 when valid, 0 otherwise.
 */
static int valid_listen(const char *value) {
    const char *separator;
    const char *cursor;
    long port;

    if (value == NULL || *value == '\0') return 0;
    separator = strrchr(value, ':');
    if (separator == NULL || separator == value || separator[1] == '\0') {
        return 0;
    }
    for (cursor = separator + 1; *cursor != '\0'; cursor++) {
        if (!isdigit((unsigned char)*cursor)) return 0;
    }
    port = strtol(separator + 1, NULL, 10);
    if (port < 1 || port > 65535) return 0;
    return 1;
}

/**
 * Resolve the default configuration path.
 * @return Owned path, or NULL on allocation failure.
 */
static char *default_path(void) {
    const char *base = getenv("XDG_CONFIG_HOME");
    const char *suffix = "/kaiman/services.ini";
    const char *home;
    size_t length;
    char *path;

    if (base == NULL || *base == '\0') {
        home = getenv("HOME");
#ifdef _WIN32
        if (home == NULL || *home == '\0') home = getenv("USERPROFILE");
#endif
        if (home == NULL || *home == '\0') home = ".";
        suffix = "/.config/kaiman/services.ini";
        base = home;
    }
    length = strlen(base) + strlen(suffix) + 1U;
    path = (char *)malloc(length);
    if (path == NULL) return NULL;
    snprintf(path, length, "%s%s", base, suffix);
    return path;
}

/**
 * Create each parent directory of a file path.
 * @param path File path.
 * @return 1 on success, 0 on failure.
 */
static int ensure_parent(const char *path) {
    char *copy = copy_text(path);
    char *cursor;

    if (copy == NULL) return 0;
    for (cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/' && *cursor != '\\') continue;
        *cursor = '\0';
        if (KAIMAN_MKDIR(copy) != 0 && errno != EEXIST) {
            free(copy);
            return 0;
        }
        *cursor = '/';
    }
    free(copy);
    return 1;
}

/**
 * Read one line from a file without its line ending.
 * @param file Open file.
 * @return Owned line without newlines, or NULL at end of file.
 */
static char *read_line(FILE *file) {
    size_t capacity = 128U;
    size_t length = 0U;
    char *line = (char *)malloc(capacity);
    int value;

    if (line == NULL) return NULL;
    value = fgetc(file);
    while (value != EOF && value != '\n') {
        char *larger;
        if (length + 2U > capacity) {
            capacity *= 2U;
            larger = (char *)realloc(line, capacity);
            if (larger == NULL) {
                free(line);
                return NULL;
            }
            line = larger;
        }
        line[length++] = (char)value;
        value = fgetc(file);
    }
    if (value == EOF && length == 0U) {
        free(line);
        return NULL;
    }
    while (length > 0U && (line[length - 1U] == '\r' ||
        line[length - 1U] == '\n')) {
        length--;
    }
    line[length] = '\0';
    return line;
}

/**
 * Release all owned field strings of a service.
 * @param service Service to clear.
 * @return void
 */
static void service_fields_clear(kc_kaiman_service_t *service) {
    free(service->name);
    free(service->adapter);
    free(service->base_url);
    free(service->model);
    free(service->api_key_env);
    free(service->token_command);
    free(service->auth_scheme);
    free(service->auth_user);
    free(service->keyring);
    service->name = NULL;
    service->adapter = NULL;
    service->base_url = NULL;
    service->model = NULL;
    service->api_key_env = NULL;
    service->token_command = NULL;
    service->auth_scheme = NULL;
    service->auth_user = NULL;
    service->keyring = NULL;
}

/**
 * Check whether a service handle is still managed.
 * @param service Service handle, or NULL.
 * @return 1 when usable, 0 otherwise.
 */
static int service_valid(const kc_kaiman_service_t *service) {
    return service != NULL && service->alive;
}

/**
 * Find a managed service by name.
 * @param kaiman Open manager.
 * @param name Service name.
 * @return Borrowed service, or NULL when absent.
 */
static kc_kaiman_service_t *find_service(const kc_kaiman_t *kaiman,
    const char *name) {
    size_t index;

    if (kaiman == NULL || name == NULL) return NULL;
    for (index = 0U; index < kaiman->count; index++) {
        if (strcmp(kaiman->services[index]->name, name) == 0) {
            return kaiman->services[index];
        }
    }
    return NULL;
}

/**
 * Allocate a managed service with defaults.
 * @param kaiman Owning manager.
 * @param name Service name.
 * @return Owned service, or NULL on invalid name or allocation failure.
 */
static kc_kaiman_service_t *service_new(kc_kaiman_t *kaiman,
    const char *name) {
    kc_kaiman_service_t *service;

    if (!valid_name(name)) return NULL;
    if (find_service(kaiman, name) != NULL) return NULL;
    service = (kc_kaiman_service_t *)calloc(1, sizeof(*service));
    if (service == NULL) return NULL;
    service->owner = kaiman;
    service->alive = 1;
    service->name = copy_text(name);
    service->auth_scheme = copy_text("bearer");
    service->timeout = DEFAULT_TIMEOUT;
    if (service->name == NULL || service->auth_scheme == NULL) {
        service_fields_clear(service);
        free(service);
        return NULL;
    }
    return service;
}

/**
 * Remove a service from the active set without persisting.
 * @param kaiman Owning manager.
 * @param service Service to retire.
 * @return void
 */
static void service_retire(kc_kaiman_t *kaiman,
    kc_kaiman_service_t *service) {
    size_t index;
    size_t next;

    for (index = 0U; index < kaiman->count; index++) {
        if (kaiman->services[index] != service) continue;
        for (next = index + 1U; next < kaiman->count; next++) {
            kaiman->services[next - 1U] = kaiman->services[next];
        }
        kaiman->count--;
        break;
    }
    service->alive = 0;
    service_fields_clear(service);
}

/**
 * Write the current configuration to disk.
 * @param kaiman Open manager.
 * @return KC_KAIMAN_OK or an error status.
 */
static int persist(kc_kaiman_t *kaiman) {
    char *temporary;
    FILE *file;
    size_t index;
    size_t length;

    if (!ensure_parent(kaiman->path)) {
        set_error(kaiman, "could not create configuration directory");
        return KC_KAIMAN_EIO;
    }
    length = strlen(kaiman->path) + 5U;
    temporary = (char *)malloc(length);
    if (temporary == NULL) {
        set_error(kaiman, "out of memory");
        return KC_KAIMAN_ENOMEM;
    }
    snprintf(temporary, length, "%s.tmp", kaiman->path);
    file = fopen(temporary, "w");
    if (file == NULL) {
        free(temporary);
        set_error(kaiman, "could not write configuration");
        return KC_KAIMAN_EIO;
    }
    fprintf(file, "[gateway]\nlisten=%s\n\n", kaiman->listen);
    for (index = 0U; index < kaiman->count; index++) {
        const kc_kaiman_service_t *service = kaiman->services[index];
        fprintf(file, "[service.%s]\n", service->name);
        fprintf(file, "adapter=%s\n",
            service->adapter != NULL ? service->adapter : "");
        fprintf(file, "url=%s\n",
            service->base_url != NULL ? service->base_url : "");
        fprintf(file, "model=%s\n",
            service->model != NULL ? service->model : "");
        fprintf(file, "env=%s\n",
            service->api_key_env != NULL ? service->api_key_env : "");
        fprintf(file, "token-command=%s\n",
            service->token_command != NULL ? service->token_command : "");
        fprintf(file, "scheme=%s\n",
            service->auth_scheme != NULL ? service->auth_scheme : "bearer");
        fprintf(file, "user=%s\n",
            service->auth_user != NULL ? service->auth_user : "");
        if (service->keyring != NULL && *service->keyring != '\0') {
            fprintf(file, "keyring=%s\n", service->keyring);
        }
        fprintf(file, "timeout=%d\n\n", service->timeout);
    }
    if (fclose(file) != 0) {
        remove(temporary);
        free(temporary);
        set_error(kaiman, "could not write configuration");
        return KC_KAIMAN_EIO;
    }
#ifndef _WIN32
    if (chmod(temporary, 0600) != 0) {
        remove(temporary);
        free(temporary);
        set_error(kaiman, "could not protect configuration");
        return KC_KAIMAN_EIO;
    }
#else
    remove(kaiman->path);
#endif
    if (rename(temporary, kaiman->path) != 0) {
        remove(temporary);
        free(temporary);
        set_error(kaiman, "could not replace configuration");
        return KC_KAIMAN_EIO;
    }
    free(temporary);
    return KC_KAIMAN_OK;
}

/**
 * Set one field of a service without persisting.
 * @param service Service to update.
 * @param key Field name.
 * @param value New value.
 * @return KC_KAIMAN_OK or an error status.
 */
static int apply_field(kc_kaiman_service_t *service, const char *key,
    const char *value) {
    kc_kaiman_t *kaiman = service->owner;
    char **target = NULL;
    char *replacement;
    char *end;
    long parsed;

    if (key == NULL || value == NULL) {
        set_error(kaiman, "invalid setting");
        return KC_KAIMAN_EINVAL;
    }
    if (!valid_value(value)) {
        set_error(kaiman, "%s cannot contain a newline", key);
        return KC_KAIMAN_EINVAL;
    }
    if (strcmp(key, "name") == 0) {
        if (!valid_name(value)) {
            set_error(kaiman, "invalid service name: %s", value);
            return KC_KAIMAN_EINVAL;
        }
        if (find_service(kaiman, value) != NULL &&
            strcmp(value, service->name) != 0) {
            set_error(kaiman, "service already exists: %s", value);
            return KC_KAIMAN_EINVAL;
        }
        target = &service->name;
    } else if (strcmp(key, "adapter") == 0) {
        if (strcmp(value, "openai") != 0 && strcmp(value, "opencode") != 0 &&
            strcmp(value, "anthropic") != 0 && strcmp(value, "gemini") != 0 &&
            strcmp(value, "responses") != 0 && strcmp(value, "ollama") != 0 &&
            strcmp(value, "bedrock") != 0) {
            set_error(kaiman, "unknown adapter: %s", value);
            return KC_KAIMAN_EINVAL;
        }
        target = &service->adapter;
    } else if (strcmp(key, "url") == 0 || strcmp(key, "base_url") == 0) {
        if (!http_url(value)) {
            set_error(kaiman, "url must start with http:// or https://");
            return KC_KAIMAN_EINVAL;
        }
        target = &service->base_url;
    } else if (strcmp(key, "model") == 0) {
        target = &service->model;
    } else if (strcmp(key, "env") == 0 || strcmp(key, "api_key_env") == 0) {
        target = &service->api_key_env;
    } else if (strcmp(key, "token-command") == 0 ||
        strcmp(key, "token_command") == 0) {
        target = &service->token_command;
    } else if (strcmp(key, "scheme") == 0 || strcmp(key, "auth_scheme") == 0) {
        if (strcmp(value, "none") != 0 && strcmp(value, "bearer") != 0 &&
            strcmp(value, "basic") != 0) {
            set_error(kaiman, "scheme must be none, bearer or basic");
            return KC_KAIMAN_EINVAL;
        }
        target = &service->auth_scheme;
    } else if (strcmp(key, "user") == 0 || strcmp(key, "auth_user") == 0) {
        target = &service->auth_user;
    } else if (strcmp(key, "keyring") == 0) {
        if (*value != '\0' && (strlen(value) != 32U ||
            strspn(value, "0123456789abcdef") != 32U)) {
            set_error(kaiman, "invalid credential reference");
            return KC_KAIMAN_EINVAL;
        }
        target = &service->keyring;
    } else if (strcmp(key, "timeout") == 0) {
        parsed = strtol(value, &end, 10);
        if (end == value || *end != '\0' || parsed < 1 || parsed > 600) {
            set_error(kaiman, "timeout must be between 1 and 600");
            return KC_KAIMAN_EINVAL;
        }
        service->timeout = (int)parsed;
        return KC_KAIMAN_OK;
    } else {
        set_error(kaiman, "unknown service setting: %s", key);
        return KC_KAIMAN_EINVAL;
    }
    replacement = copy_text(value);
    if (replacement == NULL) {
        set_error(kaiman, "out of memory");
        return KC_KAIMAN_ENOMEM;
    }
    free(*target);
    *target = replacement;
    return KC_KAIMAN_OK;
}

/**
 * Open a service manager and load its saved configuration.
 * @param out Receives the manager, or NULL on failure.
 * @param options Optional path and listen override. Values are copied.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_open(kc_kaiman_t **out, const kc_kaiman_options_t *options) {
    kc_kaiman_t *kaiman;
    int status;

    if (out == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    kaiman = (kc_kaiman_t *)calloc(1, sizeof(*kaiman));
    if (kaiman == NULL) return KC_KAIMAN_ENOMEM;
    if (options != NULL && options->path != NULL) {
        kaiman->path = copy_text(options->path);
    } else {
        kaiman->path = default_path();
    }
    if (options != NULL && options->listen != NULL) {
        kaiman->listen = copy_text(options->listen);
    } else {
        kaiman->listen = copy_text(DEFAULT_LISTEN);
    }
    if (kaiman->path == NULL || kaiman->listen == NULL) {
        kc_kaiman_close(kaiman);
        return KC_KAIMAN_ENOMEM;
    }
    status = kc_kaiman_load(kaiman);
    if (status != KC_KAIMAN_OK) {
        kc_kaiman_close(kaiman);
        return status;
    }
    *out = kaiman;
    return KC_KAIMAN_OK;
}

/**
 * Close a manager and release every service it owns.
 * @param kaiman Manager to close, or NULL.
 * @return void
 */
void kc_kaiman_close(kc_kaiman_t *kaiman) {
    kc_kaiman_service_t *service;
    kc_kaiman_service_t *next;

    if (kaiman == NULL) return;
    service = kaiman->all;
    while (service != NULL) {
        next = service->next;
        service_fields_clear(service);
        free(service);
        service = next;
    }
    free(kaiman->services);
    free(kaiman->path);
    free(kaiman->listen);
    free(kaiman->error);
    free(kaiman);
}

/**
 * Reload services and gateway settings from the configured path.
 * @param kaiman Open manager.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_load(kc_kaiman_t *kaiman) {
    FILE *file;
    char *line;
    kc_kaiman_service_t *current = NULL;
    int gateway = 0;
    int number = 0;
    int status = KC_KAIMAN_OK;

    if (kaiman == NULL) return KC_KAIMAN_EINVAL;
    while (kaiman->count > 0U) {
        service_retire(kaiman, kaiman->services[kaiman->count - 1U]);
    }
    file = fopen(kaiman->path, "r");
    if (file == NULL) {
        if (errno == ENOENT) return KC_KAIMAN_OK;
        set_error(kaiman, "could not read configuration");
        return KC_KAIMAN_EIO;
    }

    while ((line = read_line(file)) != NULL) {
        char *text = trim(line);
        number++;
        if (*text == '\0' || *text == '#' || *text == ';') {
            free(line);
            continue;
        }
        if (*text == '[') {
            size_t group_length = strlen(text);
            char *name;
            if (group_length < 3U || text[group_length - 1U] != ']') {
                set_error(kaiman, "line %d: invalid group", number);
                status = KC_KAIMAN_EFORMAT;
                free(line);
                break;
            }
            text[group_length - 1U] = '\0';
            name = trim(text + 1);
            current = NULL;
            gateway = 0;
            if (strcmp(name, "gateway") == 0) {
                gateway = 1;
            } else if (strncmp(name, "service.", 8) == 0) {
                kc_kaiman_service_t *created = service_new(kaiman, name + 8);
                kc_kaiman_service_t **items;
                if (created == NULL) {
                    set_error(kaiman, "line %d: invalid or duplicate service",
                        number);
                    status = KC_KAIMAN_EFORMAT;
                    free(line);
                    break;
                }
                items = (kc_kaiman_service_t **)realloc(kaiman->services,
                    (kaiman->count + 1U) * sizeof(*items));
                if (items == NULL) {
                    service_fields_clear(created);
                    free(created);
                    set_error(kaiman, "out of memory");
                    status = KC_KAIMAN_ENOMEM;
                    free(line);
                    break;
                }
                kaiman->services = items;
                kaiman->services[kaiman->count++] = created;
                created->next = kaiman->all;
                kaiman->all = created;
                current = created;
            } else {
                set_error(kaiman, "line %d: unknown group: %s", number, name);
                status = KC_KAIMAN_EFORMAT;
                free(line);
                break;
            }
            free(line);
            continue;
        }

        char *separator = strchr(text, '=');
        char *key;
        char *value;
        if (separator == NULL) {
            set_error(kaiman, "line %d: expected key=value", number);
            status = KC_KAIMAN_EFORMAT;
            free(line);
            break;
        }
        *separator = '\0';
        key = trim(text);
        value = trim(separator + 1);
        if (gateway) {
            char *replacement;
            if (strcmp(key, "listen") != 0) {
                set_error(kaiman, "line %d: unknown gateway setting: %s",
                    number, key);
                status = KC_KAIMAN_EFORMAT;
                free(line);
                break;
            }
            if (!valid_listen(value)) {
                set_error(kaiman, "line %d: invalid listen address", number);
                status = KC_KAIMAN_EFORMAT;
                free(line);
                break;
            }
            replacement = copy_text(value);
            if (replacement == NULL) {
                set_error(kaiman, "out of memory");
                status = KC_KAIMAN_ENOMEM;
                free(line);
                break;
            }
            free(kaiman->listen);
            kaiman->listen = replacement;
        } else if (current != NULL) {
            status = apply_field(current, key, value);
            if (status != KC_KAIMAN_OK) {
                set_error(kaiman, "line %d: %s", number,
                    kc_kaiman_error(kaiman));
                free(line);
                break;
            }
        } else {
            set_error(kaiman, "line %d: setting outside a group", number);
            status = KC_KAIMAN_EFORMAT;
            free(line);
            break;
        }
        free(line);
    }
    fclose(file);
    return status;
}

/**
 * Borrow all managed services in configuration order.
 * @param kaiman Open manager.
 * @param out Receives a borrowed array of service handles.
 * @param count Receives the number of services.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_get_services(const kc_kaiman_t *kaiman,
    kc_kaiman_service_t ***out, size_t *count) {
    if (kaiman == NULL || out == NULL || count == NULL) {
        return KC_KAIMAN_EINVAL;
    }
    *out = kaiman->services;
    *count = kaiman->count;
    return KC_KAIMAN_OK;
}

/**
 * Find one managed service by name.
 * @param kaiman Open manager.
 * @param name Service name.
 * @param out Receives a borrowed service handle when found.
 * @return KC_KAIMAN_OK, KC_KAIMAN_NOT_FOUND, or an error status.
 */
int kc_kaiman_get_service(const kc_kaiman_t *kaiman, const char *name,
    kc_kaiman_service_t **out) {
    kc_kaiman_service_t *service;

    if (kaiman == NULL || out == NULL || name == NULL) {
        return KC_KAIMAN_EINVAL;
    }
    service = find_service(kaiman, name);
    if (service == NULL) return KC_KAIMAN_NOT_FOUND;
    *out = service;
    return KC_KAIMAN_OK;
}

/**
 * Create a service from a descriptor and persist the configuration.
 * @param kaiman Open manager.
 * @param descriptor Service fields to copy.
 * @param out Receives a borrowed service handle.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_add_service(kc_kaiman_t *kaiman,
    const kc_kaiman_descriptor_t *descriptor, kc_kaiman_service_t **out) {
    static const char *keys[] = {"adapter", "url", "model", "env",
        "token-command", "scheme", "user"};
    const char *values[7];
    kc_kaiman_service_t *service;
    kc_kaiman_service_t **items;
    char timeout[32];
    size_t index;
    int status;

    if (kaiman == NULL || descriptor == NULL || out == NULL) {
        set_error(kaiman, "invalid service descriptor");
        return KC_KAIMAN_EINVAL;
    }
    service = service_new(kaiman, descriptor->name);
    if (service == NULL) {
        set_error(kaiman, "invalid or duplicate service: %s",
            descriptor->name != NULL ? descriptor->name : "");
        return KC_KAIMAN_EINVAL;
    }
    values[0] = descriptor->adapter;
    values[1] = descriptor->base_url;
    values[2] = descriptor->model;
    values[3] = descriptor->api_key_env;
    values[4] = descriptor->token_command;
    values[5] = descriptor->auth_scheme;
    values[6] = descriptor->auth_user;
    for (index = 0U; index < sizeof(keys) / sizeof(keys[0]); index++) {
        if (values[index] == NULL) continue;
        status = apply_field(service, keys[index], values[index]);
        if (status == KC_KAIMAN_OK) continue;
        service_fields_clear(service);
        free(service);
        return status;
    }
    if (descriptor->timeout != 0) {
        snprintf(timeout, sizeof(timeout), "%d", descriptor->timeout);
        status = apply_field(service, "timeout", timeout);
        if (status != KC_KAIMAN_OK) {
            service_fields_clear(service);
            free(service);
            return status;
        }
    }
    items = (kc_kaiman_service_t **)realloc(kaiman->services,
        (kaiman->count + 1U) * sizeof(*items));
    if (items == NULL) {
        service_fields_clear(service);
        free(service);
        set_error(kaiman, "out of memory");
        return KC_KAIMAN_ENOMEM;
    }
    kaiman->services = items;
    kaiman->services[kaiman->count++] = service;
    service->next = kaiman->all;
    kaiman->all = service;
    status = persist(kaiman);
    if (status != KC_KAIMAN_OK) return status;
    *out = service;
    return KC_KAIMAN_OK;
}

/**
 * Set the gateway listen address and persist the configuration.
 * @param kaiman Open manager.
 * @param listen Host and port address.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_listen(kc_kaiman_t *kaiman, const char *listen) {
    char *address;
    int status;

    if (kaiman == NULL || !valid_listen(listen)) {
        set_error(kaiman, "invalid listen address");
        return KC_KAIMAN_EINVAL;
    }
    address = copy_text(listen);
    if (address == NULL) {
        set_error(kaiman, "out of memory");
        return KC_KAIMAN_ENOMEM;
    }
    free(kaiman->listen);
    kaiman->listen = address;
    status = persist(kaiman);
    return status;
}

/**
 * Borrow the current gateway listen address.
 * @param kaiman Open manager.
 * @return NUL terminated address, or NULL for an invalid manager.
 */
const char *kc_kaiman_address(const kc_kaiman_t *kaiman) {
    return kaiman != NULL ? kaiman->listen : NULL;
}

/**
 * Borrow the resolved configuration path.
 * @param kaiman Open manager.
 * @return NUL terminated path, or NULL for an invalid manager.
 */
const char *kc_kaiman_path(const kc_kaiman_t *kaiman) {
    return kaiman != NULL ? kaiman->path : NULL;
}

/**
 * Borrow the most recent detailed error message.
 * @param kaiman Open manager, or NULL.
 * @return NUL terminated message, or NULL when none is set.
 */
const char *kc_kaiman_error(const kc_kaiman_t *kaiman) {
    return kaiman != NULL ? kaiman->error : NULL;
}

/**
 * Return a stable description for a status code.
 * @param status Status to describe.
 * @return NUL terminated description.
 */
const char *kc_kaiman_strerror(int status) {
    switch (status) {
        case KC_KAIMAN_OK: return "success";
        case KC_KAIMAN_NOT_FOUND: return "not found";
        case KC_KAIMAN_EINVAL: return "invalid argument";
        case KC_KAIMAN_ENOMEM: return "out of memory";
        case KC_KAIMAN_EFORMAT: return "invalid configuration format";
        case KC_KAIMAN_EIO: return "input/output error";
        case KC_KAIMAN_EBUSY: return "gateway already running";
        case KC_KAIMAN_EPROTO: return "unsupported protocol capability";
        case KC_KAIMAN_EHTTP: return "upstream HTTP error";
        case KC_KAIMAN_ETIMEOUT: return "operation timed out";
        default: return "unknown error";
    }
}

/**
 * Return the manifest version of the library.
 * @return NUL terminated semantic version.
 */
const char *kc_kaiman_version(void) {
    return KC_KAIMAN_VERSION;
}

/**
 * Set one field of a service and persist the configuration.
 * @param service Managed service.
 * @param key Field name.
 * @param value New value.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_service_set(kc_kaiman_service_t *service, const char *key,
    const char *value) {
    int status;

    if (!service_valid(service)) return KC_KAIMAN_EINVAL;
    if (key != NULL && strcmp(key, "keyring") == 0) return KC_KAIMAN_EINVAL;
    status = apply_field(service, key, value);
    if (status != KC_KAIMAN_OK) return status;
    return persist(service->owner);
}

/**
 * Set authentication and optionally store a new secret in the system keyring.
 * A NULL secret preserves the existing source for an unchanged scheme.
 * The none scheme removes the existing credential and references.
 * @param service Managed service.
 * @param scheme Authentication scheme: none, bearer or basic.
 * @param user Username for basic authentication, otherwise NULL or empty.
 * @param secret New credential, or NULL to keep the current credential.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_service_authenticate(kc_kaiman_service_t *service,
    const char *scheme, const char *user, const char *secret) {
    char id[33];
    char *next_scheme = NULL;
    char *next_user = NULL;
    char *next_keyring = NULL;
    char *old_scheme;
    char *old_user;
    char *old_keyring;
    char *old_env;
    char *old_command;
    int none;
    int replace_source;
    int status;

    if (!service_valid(service) || scheme == NULL) return KC_KAIMAN_EINVAL;
    none = strcmp(scheme, "none") == 0;
    if ((!none && strcmp(scheme, "bearer") != 0 &&
        strcmp(scheme, "basic") != 0) ||
        (strcmp(scheme, "basic") == 0 &&
            (user == NULL || *user == '\0' || strchr(user, ':') != NULL ||
                !valid_value(user))) ||
        (secret != NULL && (*secret == '\0' || !valid_value(secret))) ||
        (none && secret != NULL)) {
        set_error(service->owner, "invalid authentication");
        return KC_KAIMAN_EINVAL;
    }
    if (!none && secret == NULL &&
        (service->auth_scheme == NULL ||
            strcmp(service->auth_scheme, scheme) != 0 ||
            ((service->keyring == NULL || *service->keyring == '\0') &&
                (service->api_key_env == NULL || *service->api_key_env == '\0') &&
                (service->token_command == NULL || *service->token_command == '\0')))) {
        set_error(service->owner, "a credential is required");
        return KC_KAIMAN_EINVAL;
    }
    replace_source = none || secret != NULL;
    next_scheme = copy_text(scheme);
    next_user = copy_text(strcmp(scheme, "basic") == 0 ? user : "");
    if (next_scheme == NULL || next_user == NULL) {
        status = KC_KAIMAN_ENOMEM;
        goto fail;
    }
    if (secret != NULL) {
        status = kc_keyring_id(id);
        if (status != KC_KAIMAN_OK) goto fail;
        next_keyring = copy_text(id);
        if (next_keyring == NULL) {
            status = KC_KAIMAN_ENOMEM;
            goto fail;
        }
        status = kc_keyring_store(next_keyring, secret);
        if (status != KC_KAIMAN_OK) {
            set_error(service->owner, "system credential storage is unavailable");
            goto fail;
        }
    }
    old_scheme = service->auth_scheme;
    old_user = service->auth_user;
    old_keyring = service->keyring;
    old_env = service->api_key_env;
    old_command = service->token_command;
    service->auth_scheme = next_scheme;
    service->auth_user = next_user;
    if (replace_source) {
        service->keyring = next_keyring;
        service->api_key_env = NULL;
        service->token_command = NULL;
    }
    status = persist(service->owner);
    if (status != KC_KAIMAN_OK) {
        service->auth_scheme = old_scheme;
        service->auth_user = old_user;
        service->keyring = old_keyring;
        service->api_key_env = old_env;
        service->token_command = old_command;
        if (next_keyring != NULL) (void)kc_keyring_remove(next_keyring);
        goto fail;
    }
    free(old_scheme);
    free(old_user);
    if (replace_source) {
        if (old_keyring != NULL && *old_keyring != '\0') {
            status = kc_keyring_remove(old_keyring);
            if (status != KC_KAIMAN_OK) {
                set_error(service->owner,
                    "authentication saved; previous credential could not be removed");
            }
        }
        free(old_keyring);
        free(old_env);
        free(old_command);
    }
    return status;

fail:
    free(next_scheme);
    free(next_user);
    free(next_keyring);
    return status;
}

/**
 * Borrow one field of a service.
 * @param service Managed service.
 * @param key Field name.
 * @return Borrowed value, or NULL when unset or unknown.
 */
const char *kc_kaiman_service_get(const kc_kaiman_service_t *service,
    const char *key) {
    if (!service_valid(service) || key == NULL) return NULL;
    if (strcmp(key, "name") == 0) return service->name;
    if (strcmp(key, "adapter") == 0) return service->adapter;
    if (strcmp(key, "url") == 0 || strcmp(key, "base_url") == 0) {
        return service->base_url;
    }
    if (strcmp(key, "model") == 0) return service->model;
    if (strcmp(key, "keyring") == 0) return service->keyring;
    if (strcmp(key, "env") == 0 || strcmp(key, "api_key_env") == 0) {
        return service->api_key_env;
    }
    if (strcmp(key, "token-command") == 0 ||
        strcmp(key, "token_command") == 0) {
        return service->token_command;
    }
    if (strcmp(key, "scheme") == 0 || strcmp(key, "auth_scheme") == 0) {
        return service->auth_scheme;
    }
    if (strcmp(key, "user") == 0 || strcmp(key, "auth_user") == 0) {
        return service->auth_user;
    }
    return NULL;
}

/**
 * Borrow the configured request timeout of a service.
 * @param service Managed service.
 * @return Timeout in seconds, or zero for an invalid handle.
 */
int kc_kaiman_service_timeout(const kc_kaiman_service_t *service) {
    return service_valid(service) ? service->timeout : 0;
}

/**
 * Remove a service and persist the configuration.
 * The handle remains allocated but becomes invalid.
 * @param service Managed service.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_service_remove(kc_kaiman_service_t *service) {
    int status;

    if (!service_valid(service)) return KC_KAIMAN_EINVAL;
    if (service->keyring != NULL && *service->keyring != '\0') {
        status = kc_keyring_remove(service->keyring);
        if (status != KC_KAIMAN_OK) {
            set_error(service->owner, "could not remove the stored credential");
            return status;
        }
    }
    service_retire(service->owner, service);
    status = persist(service->owner);
    return status;
}

/**
 * Store a detailed error message on one gateway.
 * @param gateway Destination gateway, or NULL.
 * @param format Printf style format.
 * @return void
 */
static void gateway_error(kc_kaiman_gateway_t *gateway, const char *format, ...) {
    char message[512];
    va_list args;

    if (gateway == NULL) return;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    free(gateway->error);
    gateway->error = kc_util_strdup(message);
}

/**
 * Find the connection state associated with one peer.
 * @param gateway Open gateway.
 * @param peer Borrowed peer.
 * @return Borrowed connection, or NULL when absent.
 */
static kc_kaiman_connection_t *gateway_connection(kc_kaiman_gateway_t *gateway,
    kc_netl_peer_t *peer) {
    size_t index;

    if (gateway == NULL || peer == NULL) return NULL;
    for (index = 0U; index < gateway->connection_count; index++) {
        if (gateway->connections[index].peer == peer) {
            return &gateway->connections[index];
        }
    }
    return NULL;
}

/**
 * Forget one peer and release its parser.
 * @param gateway Open gateway.
 * @param peer Borrowed peer.
 * @return void
 */
static void gateway_forget(kc_kaiman_gateway_t *gateway,
    kc_netl_peer_t *peer) {
    kc_kaiman_connection_t *connection;
    size_t index;

    if (gateway == NULL || peer == NULL) return;
    connection = gateway_connection(gateway, peer);
    if (connection == NULL) return;
    kc_http_parser_close(connection->parser);
    kc_buffer_close(connection->body);
    index = (size_t)(connection - gateway->connections);
    if (index + 1U < gateway->connection_count) {
        memmove(&gateway->connections[index], &gateway->connections[index + 1U],
            (gateway->connection_count - index - 1U) *
            sizeof(*gateway->connections));
    }
    gateway->connection_count--;
}

/**
 * Send one complete HTTP response and close its peer.
 * @param gateway Open gateway.
 * @param peer Borrowed peer.
 * @param status HTTP status.
 * @param content_type Response content type.
 * @param body Response bytes.
 * @param body_size Byte count.
 * @return KC_KAIMAN_OK or an error status.
 */
static int gateway_respond(kc_kaiman_gateway_t *gateway,
    kc_netl_peer_t *peer, int status, const char *content_type,
    const void *body, size_t body_size) {
    kc_http_field_t fields[2];
    kc_http_response_build_t built;
    void *wire = NULL;
    size_t wire_size = 0U;
    int result;
    int rc;

    if (gateway == NULL || peer == NULL) return KC_KAIMAN_EINVAL;
    fields[0].name = "Content-Type";
    fields[0].value = content_type != NULL ? content_type : "text/plain";
    fields[1].name = "Connection";
    fields[1].value = "close";
    memset(&built, 0, sizeof(built));
    built.status = &status;
    built.headers = fields;
    built.header_count = 2U;
    built.body = body;
    built.body_size = body_size;
    rc = kc_http_response(&built, &wire, &wire_size);
    if (rc != KC_HTTP_OK) {
        gateway_error(gateway, "could not build an HTTP response");
        return KC_KAIMAN_EFORMAT;
    }
    rc = kc_netl_peer_send(peer, wire, wire_size);
    kc_http_free(wire);
    if (rc != KC_NETL_OK) {
        gateway_error(gateway, "could not send an HTTP response");
        return KC_KAIMAN_EIO;
    }
    result = KC_KAIMAN_OK;
    kc_netl_peer_close(peer);
    return result;
}

/**
 * Send one JSON HTTP response.
 * @param gateway Open gateway.
 * @param peer Borrowed peer.
 * @param status HTTP status.
 * @param text JSON text.
 * @return KC_KAIMAN_OK or an error status.
 */
static int gateway_json(kc_kaiman_gateway_t *gateway,
    kc_netl_peer_t *peer, int status, const char *text) {
    return gateway_respond(gateway, peer, status, "application/json",
        text, strlen(text));
}

/**
 * Send one OpenAI compatible error response.
 * @param gateway Open gateway.
 * @param peer Borrowed peer.
 * @param status HTTP status.
 * @param message Error message.
 * @return KC_KAIMAN_OK or an error status.
 */
static int gateway_failure(kc_kaiman_gateway_t *gateway,
    kc_netl_peer_t *peer, int status, const char *message) {
    kc_buffer_t *buffer = NULL;
    char *escaped = NULL;
    int result;

    if (kc_buffer_open(&buffer) != KC_KAIMAN_OK) {
        return gateway_json(gateway, peer, status,
            "{\"error\":{\"message\":\"request failed\"}}");
    }
    if (kc_json_escape(message != NULL ? message : "request failed",
        &escaped) == KC_KAIMAN_OK) {
        (void)kc_buffer_format(buffer,
            "{\"error\":{\"message\":\"%s\",\"type\":\"invalid_request_error\",\"code\":%d}}",
            escaped, status);
    } else {
        (void)kc_buffer_format(buffer,
            "{\"error\":{\"message\":\"request failed\",\"code\":%d}}", status);
    }
    kc_kaiman_free(escaped);
    result = gateway_json(gateway, peer, status, kc_buffer_value(buffer));
    kc_buffer_close(buffer);
    return result;
}

/**
 * Append one escaped JSON string, including its quotes.
 * @param buffer Destination buffer.
 * @param text Text to append.
 * @return KC_KAIMAN_OK or an error status.
 */
static int json_write_text(kc_buffer_t *buffer, const char *text) {
    char *escaped = NULL;
    int status;

    status = kc_json_escape(text != NULL ? text : "", &escaped);
    if (status != KC_KAIMAN_OK) return status;
    status = kc_buffer_text(buffer, "\"");
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, escaped);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "\"");
    kc_kaiman_free(escaped);
    return status;
}

/**
 * Serialize one JSON object with selected fields replaced.
 * @param value Borrowed JSON object.
 * @param model Replacement model, or NULL to keep the original.
 * @param object Replacement object type, or NULL to keep the original.
 * @param stream Requested stream flag, or negative to keep the original.
 * @param out Receives owned JSON text.
 * @return KC_KAIMAN_OK or an error status.
 */
static int json_rewrite(const kc_json_value_t *value, const char *model,
    const char *object, int stream, char **out) {
    kc_buffer_t *buffer = NULL;
    char *piece = NULL;
    size_t index;
    int has_stream = 0;
    int status = KC_KAIMAN_OK;

    if (out == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    if (value == NULL || kc_json_type(value) != KC_JSON_OBJECT) {
        return KC_KAIMAN_EINVAL;
    }
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) return status;
    status = kc_buffer_text(buffer, "{");
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < kc_json_length(value); index++) {
        const char *key = kc_json_key(value, index);
        kc_json_value_t *item = kc_json_at(value, index);
        if (index != 0U) status = kc_buffer_text(buffer, ",");
        if (status != KC_KAIMAN_OK) break;
        status = json_write_text(buffer, key);
        if (status != KC_KAIMAN_OK) break;
        status = kc_buffer_text(buffer, ":");
        if (status != KC_KAIMAN_OK) break;
        if (strcmp(key, "model") == 0 && model != NULL) {
            status = json_write_text(buffer, model);
        } else if (strcmp(key, "object") == 0 && object != NULL) {
            status = json_write_text(buffer, object);
        } else if (strcmp(key, "stream") == 0 && stream >= 0) {
            has_stream = 1;
            status = kc_buffer_text(buffer, stream ? "true" : "false");
        } else {
            status = kc_json_stringify(item, &piece);
            if (status == KC_KAIMAN_OK) {
                status = kc_buffer_text(buffer, piece != NULL
                    ? piece : "null");
            }
            kc_kaiman_free(piece);
            piece = NULL;
        }
    }
    if (status == KC_KAIMAN_OK && stream >= 0 && !has_stream) {
        status = kc_buffer_format(buffer, ",\"stream\":%s",
            stream ? "true" : "false");
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "}");
    if (status != KC_KAIMAN_OK) {
        kc_buffer_close(buffer);
        return status;
    }
    *out = kc_util_strdup(kc_buffer_value(buffer));
    kc_buffer_close(buffer);
    return *out != NULL ? KC_KAIMAN_OK : KC_KAIMAN_ENOMEM;
}

/**
 * Append one base URL and API path.
 * @param base Base URL.
 * @param path API path beginning with a slash.
 * @return Owned URL, or NULL on failure.
 */
static char *api_url(const char *base, const char *path) {
    kc_buffer_t *buffer = NULL;
    size_t length;
    char *result;

    if (base == NULL || path == NULL) return NULL;
    if (kc_buffer_open(&buffer) != KC_KAIMAN_OK) return NULL;
    length = strlen(base);
    if (kc_buffer_text(buffer, base) != KC_KAIMAN_OK) {
        kc_buffer_close(buffer);
        return NULL;
    }
    if (length > 0U && base[length - 1U] == '/') {
        (void)kc_buffer_append(buffer, path + 1U, strlen(path + 1U));
    } else {
        (void)kc_buffer_text(buffer, path);
    }
    result = kc_util_strdup(kc_buffer_value(buffer));
    kc_buffer_close(buffer);
    return result;
}

/**
 * Resolve the authorization value of one service without storing it.
 * @param service Managed service.
 * @param out Receives owned header value, or NULL when authentication is off.
 * @return KC_KAIMAN_OK or an error status.
 */
static int service_authorization(const kc_kaiman_service_t *service,
    char **out) {
    const char *scheme;
    char *token = NULL;
    char *combined = NULL;
    char *encoded = NULL;
    int status = KC_KAIMAN_OK;

    if (out == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    if (!service_valid(service)) return KC_KAIMAN_EINVAL;
    scheme = service->auth_scheme != NULL ? service->auth_scheme : "none";
    if (strcmp(scheme, "none") == 0) return KC_KAIMAN_OK;

    if (service->token_command != NULL && *service->token_command != '\0') {
        char line[4096];
#ifdef _WIN32
        FILE *pipe = _popen(service->token_command, "r");
#else
        FILE *pipe = popen(service->token_command, "r");
#endif
        if (pipe == NULL) return KC_KAIMAN_EIO;
        if (fgets(line, (int)sizeof(line), pipe) == NULL) status = KC_KAIMAN_EIO;
#ifdef _WIN32
        if (_pclose(pipe) != 0 && status == KC_KAIMAN_OK) {
            status = KC_KAIMAN_EIO;
        }
#else
        if (pclose(pipe) != 0 && status == KC_KAIMAN_OK) {
            status = KC_KAIMAN_EIO;
        }
#endif
        if (status == KC_KAIMAN_OK) {
            size_t length = strlen(line);
            while (length > 0U && (line[length - 1U] == '\n' ||
                line[length - 1U] == '\r')) {
                line[--length] = '\0';
            }
            if (length == 0U) status = KC_KAIMAN_EINVAL;
            else token = kc_util_strdup(line);
        }
    } else if (service->api_key_env != NULL && *service->api_key_env != '\0') {
        const char *value = getenv(service->api_key_env);
        if (value == NULL || *value == '\0') status = KC_KAIMAN_EINVAL;
        else token = kc_util_strdup(value);
    } else if (service->keyring != NULL && *service->keyring != '\0') {
        status = kc_keyring_read(service->keyring, &token);
    } else {
        status = KC_KAIMAN_EINVAL;
    }
    if (status != KC_KAIMAN_OK) return status;
    if (token == NULL) return KC_KAIMAN_ENOMEM;
    if (*token == '\0' || !valid_value(token)) {
        free(token);
        return KC_KAIMAN_EINVAL;
    }

    if (strcmp(scheme, "bearer") == 0) {
        kc_buffer_t *buffer = NULL;
        status = kc_buffer_open(&buffer);
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_text(buffer, "Bearer ");
        }
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, token);
        if (status == KC_KAIMAN_OK) *out = kc_util_strdup(
            kc_buffer_value(buffer));
        kc_buffer_close(buffer);
        if (*out == NULL) status = KC_KAIMAN_ENOMEM;
    } else {
        combined = (char *)malloc(strlen(service->auth_user != NULL
            ? service->auth_user : "") + strlen(token) + 2U);
        if (combined == NULL) status = KC_KAIMAN_ENOMEM;
        else {
            (void)snprintf(combined,
                strlen(service->auth_user != NULL ? service->auth_user : "") +
                strlen(token) + 2U, "%s:%s",
                service->auth_user != NULL ? service->auth_user : "", token);
            encoded = kc_util_base64(combined, strlen(combined));
            if (encoded == NULL) status = KC_KAIMAN_ENOMEM;
            else {
                kc_buffer_t *buffer = NULL;
                status = kc_buffer_open(&buffer);
                if (status == KC_KAIMAN_OK) {
                    status = kc_buffer_text(buffer, "Basic ");
                }
                if (status == KC_KAIMAN_OK) {
                    status = kc_buffer_text(buffer, encoded);
                }
                if (status == KC_KAIMAN_OK) {
                    *out = kc_util_strdup(kc_buffer_value(buffer));
                }
                kc_buffer_close(buffer);
                if (*out == NULL) status = KC_KAIMAN_ENOMEM;
            }
        }
    }
    free(encoded);
    free(combined);
    free(token);
    return status;
}

/**
 * Remove a prefix from one growable buffer.
 * @param buffer Buffer to modify.
 * @param count Number of bytes to remove.
 * @return void
 */
static void gateway_consume(kc_buffer_t *buffer, size_t count) {
    if (buffer == NULL || count == 0U || buffer->data == NULL) return;
    if (count >= buffer->size) {
        buffer->size = 0U;
        buffer->data[0] = '\0';
        return;
    }
    memmove(buffer->data, buffer->data + count, buffer->size - count);
    buffer->size -= count;
    buffer->data[buffer->size] = '\0';
}

/**
 * Begin one streaming HTTP response.
 * @param gateway Open gateway.
 * @param stream Streaming context.
 * @return KC_KAIMAN_OK or an error status.
 */
static int gateway_sse_start(kc_kaiman_gateway_t *gateway,
    kc_kaiman_stream_t *stream) {
    static const char header[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";

    if (stream == NULL || stream->headers_sent) return KC_KAIMAN_OK;
    if (kc_netl_peer_send(stream->peer, header, sizeof(header) - 1U) !=
        KC_NETL_OK) {
        gateway_error(gateway, "could not start an event stream");
        return KC_KAIMAN_EIO;
    }
    stream->headers_sent = 1;
    return KC_KAIMAN_OK;
}

/**
 * Send one server sent event data line.
 * @param gateway Open gateway.
 * @param stream Streaming context.
 * @param payload Event payload.
 * @return KC_KAIMAN_OK or an error status.
 */
static int gateway_sse_data(kc_kaiman_gateway_t *gateway,
    kc_kaiman_stream_t *stream, const char *payload) {
    kc_buffer_t *frame = NULL;
    int status;

    status = gateway_sse_start(gateway, stream);
    if (status != KC_KAIMAN_OK) return status;
    status = kc_buffer_open(&frame);
    if (status != KC_KAIMAN_OK) return status;
    status = kc_buffer_text(frame, "data: ");
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(frame, payload);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(frame, "\n\n");
    if (status == KC_KAIMAN_OK) {
        char size[32];
        (void)snprintf(size, sizeof(size), "%zx\r\n", frame->size);
        if (kc_netl_peer_send(stream->peer, size, strlen(size)) != KC_NETL_OK ||
            kc_netl_peer_send(stream->peer, frame->data,
                frame->size) != KC_NETL_OK ||
            kc_netl_peer_send(stream->peer, "\r\n", 2U) != KC_NETL_OK) {
            status = KC_KAIMAN_EIO;
            gateway_error(gateway, "could not send an event stream frame");
        }
    }
    kc_buffer_close(frame);
    return status;
}

/**
 * Send a rewritten upstream SSE payload.
 * @param gateway Open gateway.
 * @param stream Streaming context.
 * @param payload Borrowed upstream data payload.
 * @return void
 */
static void gateway_sse_payload(kc_kaiman_gateway_t *gateway,
    kc_kaiman_stream_t *stream, const char *payload) {
    kc_json_value_t *value = NULL;
    char *text = NULL;
    size_t length = strlen(payload);

    if (strcmp(payload, "[DONE]") == 0) {
        if (!stream->done) {
            (void)gateway_sse_data(gateway, stream, "[DONE]");
            stream->done = 1;
        }
        return;
    }
    if (kc_json_parse(payload, length, &value) == KC_KAIMAN_OK) {
        if (json_rewrite(value, stream->model,
            "chat.completion.chunk", -1, &text) == KC_KAIMAN_OK) {
            (void)gateway_sse_data(gateway, stream, text);
        }
        kc_kaiman_free(text);
        kc_json_free(value);
        return;
    }
    kc_json_free(value);
    (void)gateway_sse_data(gateway, stream, payload);
}

/**
 * Receive the upstream response status for one stream.
 * @param status HTTP status.
 * @param content_type Borrowed content type.
 * @param userdata Streaming context.
 * @return void
 */
static void gateway_sse_status(int status, const char *content_type,
    void *userdata) {
    kc_kaiman_stream_t *stream = (kc_kaiman_stream_t *)userdata;
    (void)content_type;
    if (stream != NULL) stream->status = status;
}

/**
 * Convert one parsed upstream chunk into client SSE frames.
 * @param data Borrowed response bytes.
 * @param size Byte count.
 * @param userdata Streaming context.
 * @return 0 to continue, or a negative value to stop.
 */
static int gateway_sse_chunk(const void *data, size_t size, void *userdata) {
    kc_kaiman_stream_t *stream = (kc_kaiman_stream_t *)userdata;
    kc_kaiman_gateway_t *gateway;
    char line[65536];

    if (stream == NULL) return -1;
    gateway = stream->gateway;
    if (stream->status >= 400) return 0;
    if (kc_buffer_append(stream->sse, data, size) != KC_KAIMAN_OK) return -1;
    if (stream->sse->size > sizeof(line)) {
        kc_buffer_close(stream->sse);
        stream->sse = NULL;
        return -1;
    }
    for (;;) {
        char *position = strchr(kc_buffer_value(stream->sse), '\n');
        size_t length;
        if (position == NULL) break;
        length = (size_t)(position - kc_buffer_value(stream->sse));
        if (length >= sizeof(line)) {
            kc_buffer_close(stream->sse);
            stream->sse = NULL;
            return -1;
        }
        memcpy(line, kc_buffer_value(stream->sse), length);
        line[length] = '\0';
        gateway_consume(stream->sse, length + 1U);
        while (length > 0U && (line[length - 1U] == '\r' ||
            line[length - 1U] == '\n')) {
            line[--length] = '\0';
        }
        if (strncmp(line, "data:", 5U) == 0) {
            const char *payload = line + 5U;
            while (*payload == ' ') payload++;
            if (gateway != NULL) {
                gateway_sse_payload(gateway, stream, payload);
            }
        }
    }
    return 0;
}

/**
 * Select one configured service for a client model.
 * @param kaiman Open manager.
 * @param requested Requested model, or NULL.
 * @param out Receives borrowed service.
 * @return KC_KAIMAN_OK, KC_KAIMAN_NOT_FOUND, or an error status.
 */
static int gateway_select(kc_kaiman_t *kaiman, const char *requested,
    kc_kaiman_service_t **out) {
    size_t index;

    if (kaiman == NULL || out == NULL) return KC_KAIMAN_EINVAL;
    if (requested != NULL && *requested != '\0') {
        for (index = 0U; index < kaiman->count; index++) {
            kc_kaiman_service_t *service = kaiman->services[index];
            if (strcmp(service->name, requested) == 0 ||
                (service->model != NULL &&
                    strcmp(service->model, requested) == 0)) {
                *out = service;
                return KC_KAIMAN_OK;
            }
        }
    }
    if (kaiman->count == 1U) {
        *out = kaiman->services[0U];
        return KC_KAIMAN_OK;
    }
    return KC_KAIMAN_NOT_FOUND;
}

/**
 * Append the text of one JSON message content value.
 * @param buffer Destination buffer.
 * @param value Borrowed content value.
 * @return KC_KAIMAN_OK or an error status.
 */
static int json_append_content(kc_buffer_t *buffer,
    const kc_json_value_t *value) {
    size_t index;
    int status = KC_KAIMAN_OK;

    if (buffer == NULL || value == NULL) return KC_KAIMAN_EINVAL;
    if (kc_json_type(value) == KC_JSON_STRING) {
        return kc_buffer_text(buffer, kc_json_text(value));
    }
    if (kc_json_type(value) == KC_JSON_ARRAY) {
        for (index = 0U; status == KC_KAIMAN_OK &&
            index < kc_json_length(value); index++) {
            kc_json_value_t *item = kc_json_item(value, index);
            kc_json_value_t *text;
            if (kc_json_type(item) == KC_JSON_STRING) {
                status = kc_buffer_text(buffer, kc_json_text(item));
                continue;
            }
            text = kc_json_member(item, "text");
            if (kc_json_type(text) == KC_JSON_STRING) {
                status = kc_buffer_text(buffer, kc_json_text(text));
            } else {
                text = kc_json_member(item, "content");
                if (kc_json_type(text) == KC_JSON_STRING) {
                    status = kc_buffer_text(buffer, kc_json_text(text));
                }
            }
        }
        return status;
    }
    return KC_KAIMAN_OK;
}

/**
 * Extract one plain-text prompt from an OpenAI messages array.
 * @param root Borrowed chat request object.
 * @param out Receives owned prompt text.
 * @return KC_KAIMAN_OK or an error status.
 */
static int request_prompt(const kc_json_value_t *root, char **out) {
    kc_json_value_t *messages;
    kc_buffer_t *buffer = NULL;
    size_t index;
    int status;

    if (out == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    if (root == NULL) return KC_KAIMAN_EINVAL;
    messages = kc_json_member(root, "messages");
    if (kc_json_type(messages) != KC_JSON_ARRAY) return KC_KAIMAN_EINVAL;
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) return status;
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < kc_json_length(messages); index++) {
        kc_json_value_t *message = kc_json_item(messages, index);
        const char *role = kc_json_text(kc_json_member(message, "role"));
        kc_json_value_t *content = kc_json_member(message, "content");
        if (buffer->size > 32768U) break;
        if (role == NULL) role = "user";
        status = kc_buffer_format(buffer, "%s: ", role);
        if (status != KC_KAIMAN_OK) break;
        status = json_append_content(buffer, content);
        if (status != KC_KAIMAN_OK) break;
        status = kc_buffer_text(buffer, "\n\n");
    }
    if (status == KC_KAIMAN_OK) {
        *out = kc_util_strdup(kc_buffer_value(buffer));
        if (*out == NULL) status = KC_KAIMAN_ENOMEM;
    }
    kc_buffer_close(buffer);
    return status;
}

/**
 * Forward one chat request to an OpenAI-compatible provider.
 * @param gateway Open gateway.
 * @param peer Borrowed client peer.
 * @param root Borrowed chat request object.
 * @return KC_KAIMAN_OK or an error status.
 */
static int openai_chat(kc_kaiman_gateway_t *gateway, kc_netl_peer_t *peer,
    const kc_json_value_t *root) {
    const char *requested = kc_json_text(kc_json_member(root, "model"));
    kc_kaiman_service_t *service = NULL;
    kc_upstream_field_t fields[1];
    kc_upstream_request_t request;
    kc_upstream_response_t response;
    kc_kaiman_stream_t stream;
    kc_json_value_t *reply = NULL;
    char *authorization = NULL;
    char *body = NULL;
    char *url = NULL;
    char *text = NULL;
    const char *content_type;
    int stream_mode;
    int status;
    int rc;

    status = gateway_select(gateway->manager, requested, &service);
    if (status != KC_KAIMAN_OK) {
        return gateway_failure(gateway, peer, 404,
            "no configured service matches the requested model");
    }
    stream_mode = kc_json_boolean(kc_json_member(root, "stream"));
    status = service_authorization(service, &authorization);
    if (status != KC_KAIMAN_OK) {
        return gateway_failure(gateway, peer, 400,
            "the service credential is unavailable");
    }
    status = json_rewrite(root, service->model, NULL, stream_mode, &body);
    if (status != KC_KAIMAN_OK) {
        free(authorization);
        return gateway_failure(gateway, peer, 400, "invalid chat request");
    }
    url = api_url(service->base_url, "/chat/completions");
    if (url == NULL) {
        free(authorization);
        free(body);
        return gateway_failure(gateway, peer, 400, "invalid service URL");
    }
    memset(fields, 0, sizeof(fields));
    fields[0].name = "Authorization";
    fields[0].value = authorization;
    memset(&request, 0, sizeof(request));
    request.method = "POST";
    request.url = url;
    request.headers = authorization != NULL ? fields : NULL;
    request.header_count = authorization != NULL ? 1U : 0U;
    request.body = body;
    request.body_size = strlen(body);
    request.timeout = kc_kaiman_service_timeout(service);
    memset(&response, 0, sizeof(response));
    memset(&stream, 0, sizeof(stream));

    if (stream_mode) {
        status = kc_buffer_open(&stream.sse);
        if (status == KC_KAIMAN_OK) {
            stream.peer = peer;
            stream.gateway = gateway;
            stream.model = kc_util_strdup(requested != NULL && *requested != '\0'
                ? requested : service->name);
            if (stream.model == NULL) status = KC_KAIMAN_ENOMEM;
        }
        if (status == KC_KAIMAN_OK) {
            request.chunk = gateway_sse_chunk;
            request.status = gateway_sse_status;
            request.userdata = &stream;
            rc = kc_upstream_send(&request, &response);
            if (rc != KC_KAIMAN_OK) {
                gateway_failure(gateway, peer, 502,
                    kc_upstream_strerror(rc));
                status = KC_KAIMAN_EHTTP;
            } else if (response.status >= 400) {
                content_type = response.content_type != NULL &&
                    *response.content_type != '\0'
                    ? response.content_type : "application/json";
                status = gateway_respond(gateway, peer, response.status,
                    content_type, response.body, response.body_size);
            } else {
                if (!stream.done) {
                    (void)gateway_sse_data(gateway, &stream, "[DONE]");
                }
                if (stream.headers_sent) {
                    (void)kc_netl_peer_send(peer, "0\r\n\r\n", 5U);
                    kc_netl_peer_close(peer);
                } else {
                    status = gateway_failure(gateway, peer, 502,
                        "the service returned no event stream");
                }
            }
        }
        kc_buffer_close(stream.sse);
        free(stream.model);
    } else {
        rc = kc_upstream_send(&request, &response);
        if (rc != KC_KAIMAN_OK) {
            status = gateway_failure(gateway, peer, 502,
                kc_upstream_strerror(rc));
        } else if (response.status >= 400) {
            content_type = response.content_type != NULL &&
                *response.content_type != '\0'
                ? response.content_type : "application/json";
            status = gateway_respond(gateway, peer, response.status,
                content_type, response.body, response.body_size);
        } else {
            status = kc_json_parse(response.body, response.body_size, &reply);
            if (status == KC_KAIMAN_OK) {
                status = json_rewrite(reply,
                    requested != NULL && *requested != '\0'
                        ? requested : service->name,
                    "chat.completion", -1, &text);
            }
            if (status == KC_KAIMAN_OK) {
                status = gateway_json(gateway, peer, 200, text);
            } else {
                content_type = response.content_type != NULL &&
                    *response.content_type != '\0'
                    ? response.content_type : "application/json";
                status = gateway_respond(gateway, peer, 200, content_type,
                    response.body, response.body_size);
            }
        }
    }
    kc_json_free(reply);
    kc_kaiman_free(text);
    kc_upstream_response_free(&response);
    free(url);
    free(body);
    free(authorization);
    return status;
}

#define KAIMAN_MAX_HISTORY 8U

typedef struct {
    char *role;
    char *content;
} kc_kaiman_message_t;

/**
 * Resolve the persistent session path of one upstream server.
 * @param service Managed service.
 * @return Owned path, or NULL on failure.
 */
static char *opencode_session_path(const kc_kaiman_service_t *service) {
    const char *cursor;
    unsigned long long hash = 1469598103934665603ULL;
    char key[32];

    if (!service_valid(service) || service->base_url == NULL) return NULL;
    for (cursor = service->base_url; *cursor != '\0'; cursor++) {
        hash ^= (unsigned char)*cursor;
        hash *= 1099511628211ULL;
    }
    (void)snprintf(key, sizeof(key), "url-%016llx", hash);
    return kc_util_session_path(key);
}

/**
 * Load one persistent OpenCode state.
 * @param path State path.
 * @param session Receives owned session identifier, or NULL.
 * @param messages Receives up to KAIMAN_MAX_HISTORY owned messages.
 * @param count Receives the message count.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_state_load(const char *path, char **session,
    kc_kaiman_message_t *messages, size_t *count) {
    kc_json_value_t *root = NULL;
    kc_json_value_t *items;
    char *data;
    size_t size;
    size_t index;
    size_t stored;
    size_t start;
    int status;

    if (session == NULL || messages == NULL || count == NULL) {
        return KC_KAIMAN_EINVAL;
    }
    *session = NULL;
    *count = 0U;
    data = kc_util_read_file(path, &size);
    if (data == NULL) return KC_KAIMAN_OK;
    status = kc_json_parse(data, size, &root);
    if (status != KC_KAIMAN_OK) {
        free(data);
        return KC_KAIMAN_OK;
    }
    {
        const char *id = kc_json_text(kc_json_member(root, "id"));
        if (id != NULL && *id != '\0') *session = kc_util_strdup(id);
    }
    items = kc_json_member(root, "messages");
    if (kc_json_type(items) == KC_JSON_ARRAY) {
        stored = kc_json_length(items);
        start = stored > KAIMAN_MAX_HISTORY ? stored - KAIMAN_MAX_HISTORY : 0U;
        for (index = start; index < stored; index++) {
            kc_json_value_t *item = kc_json_item(items, index);
            const char *role = kc_json_text(kc_json_member(item, "role"));
            const char *content = kc_json_text(kc_json_member(item, "content"));
            if (role == NULL || content == NULL) continue;
            messages[*count].role = kc_util_strdup(role);
            messages[*count].content = kc_util_strdup(content);
            if (messages[*count].role == NULL ||
                messages[*count].content == NULL) {
                while (*count > 0U) {
                    (*count)--;
                    free(messages[*count].role);
                    free(messages[*count].content);
                }
                status = KC_KAIMAN_ENOMEM;
                break;
            }
            (*count)++;
        }
    }
    kc_json_free(root);
    free(data);
    return status;
}

/**
 * Store one persistent OpenCode state.
 * @param path State path.
 * @param session Session identifier.
 * @param messages Borrowed bounded history.
 * @param count Message count.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_state_save(const char *path, const char *session,
    const kc_kaiman_message_t *messages, size_t count) {
    kc_buffer_t *buffer = NULL;
    size_t index;
    size_t start;
    int status;

    if (path == NULL || session == NULL || messages == NULL) {
        return KC_KAIMAN_EINVAL;
    }
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) return status;
    status = kc_buffer_text(buffer, "{\"id\":");
    if (status == KC_KAIMAN_OK) status = json_write_text(buffer, session);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer,
        ",\"messages\":[");
    start = count > KAIMAN_MAX_HISTORY ? count - KAIMAN_MAX_HISTORY : 0U;
    for (index = start; status == KC_KAIMAN_OK && index < count; index++) {
        if (index != start) status = kc_buffer_text(buffer, ",");
        if (status != KC_KAIMAN_OK) break;
        status = kc_buffer_text(buffer, "{\"role\":");
        if (status != KC_KAIMAN_OK) break;
        status = json_write_text(buffer, messages[index].role);
        if (status != KC_KAIMAN_OK) break;
        status = kc_buffer_text(buffer, ",\"content\":");
        if (status != KC_KAIMAN_OK) break;
        status = json_write_text(buffer, messages[index].content);
        if (status != KC_KAIMAN_OK) break;
        status = kc_buffer_text(buffer, "}");
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "]}");
    if (status == KC_KAIMAN_OK && !kc_util_write_private(path,
        kc_buffer_value(buffer), strlen(kc_buffer_value(buffer)))) {
        status = KC_KAIMAN_EIO;
    }
    kc_buffer_close(buffer);
    return status;
}

/**
 * Split an OpenCode model into provider and model identifiers.
 * @param model Configured model.
 * @param provider Receives owned provider identifier.
 * @param identifier Receives owned model identifier.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_split_model(const char *model, char **provider,
    char **identifier) {
    const char *slash;
    size_t length;

    if (model == NULL || provider == NULL || identifier == NULL) {
        return KC_KAIMAN_EINVAL;
    }
    slash = strchr(model, '/');
    if (slash == NULL || slash == model || slash[1U] == '\0') {
        return KC_KAIMAN_EINVAL;
    }
    length = (size_t)(slash - model);
    *provider = (char *)malloc(length + 1U);
    if (*provider == NULL) return KC_KAIMAN_ENOMEM;
    memcpy(*provider, model, length);
    (*provider)[length] = '\0';
    *identifier = kc_util_strdup(slash + 1U);
    if (*identifier == NULL) {
        free(*provider);
        *provider = NULL;
        return KC_KAIMAN_ENOMEM;
    }
    return KC_KAIMAN_OK;
}

/**
 * Perform one authenticated OpenCode API request.
 * @param service Managed service.
 * @param method HTTP method.
 * @param path API path.
 * @param body Request body, or NULL.
 * @param response Receives the owned response.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_request(const kc_kaiman_service_t *service,
    const char *method, const char *path, const char *body,
    kc_upstream_response_t *response) {
    kc_upstream_field_t fields[1];
    kc_upstream_request_t request;
    char *authorization = NULL;
    char *url;
    int status;

    if (response == NULL) return KC_KAIMAN_EINVAL;
    memset(response, 0, sizeof(*response));
    status = service_authorization(service, &authorization);
    if (status != KC_KAIMAN_OK) return status;
    url = api_url(service->base_url, path);
    if (url == NULL) {
        free(authorization);
        return KC_KAIMAN_ENOMEM;
    }
    memset(fields, 0, sizeof(fields));
    fields[0].name = "Authorization";
    fields[0].value = authorization;
    memset(&request, 0, sizeof(request));
    request.method = method;
    request.url = url;
    request.headers = authorization != NULL ? fields : NULL;
    request.header_count = authorization != NULL ? 1U : 0U;
    request.body = body;
    request.body_size = body != NULL ? strlen(body) : 0U;
    request.timeout = kc_kaiman_service_timeout(service);
    status = kc_upstream_send(&request, response);
    free(url);
    free(authorization);
    return status;
}

/**
 * Read the session identifier from an OpenCode response.
 * @param response Borrowed upstream response.
 * @return Owned identifier, or NULL.
 */
static char *opencode_session_id(const kc_upstream_response_t *response) {
    kc_json_value_t *root = NULL;
    kc_json_value_t *data;
    const char *id;
    char *result = NULL;

    if (kc_json_parse(response->body, response->body_size, &root) !=
        KC_KAIMAN_OK) {
        return NULL;
    }
    data = kc_json_member(root, "data");
    id = kc_json_text(kc_json_member(data, "id"));
    if (id != NULL && strncmp(id, "ses_", 4U) == 0 && id[4U] != '\0') {
        result = kc_util_strdup(id);
    }
    kc_json_free(root);
    return result;
}

/**
 * Ensure one persistent OpenCode session exists for a service.
 * @param service Managed service.
 * @param state_path Persistent state path.
 * @param session Receives owned session identifier.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_ensure_session(const kc_kaiman_service_t *service,
    const char *state_path, char **session) {
    kc_kaiman_message_t messages[KAIMAN_MAX_HISTORY];
    kc_upstream_response_t response;
    kc_buffer_t *buffer = NULL;
    char *provider = NULL;
    char *identifier = NULL;
    char *path = NULL;
    char *body = NULL;
    char *id = NULL;
    size_t count = 0U;
    size_t index;
    int status;

    if (session == NULL) return KC_KAIMAN_EINVAL;
    *session = NULL;
    memset(messages, 0, sizeof(messages));
    status = opencode_state_load(state_path, &id, messages, &count);
    if (status != KC_KAIMAN_OK) return status;

    if (id != NULL) {
        path = (char *)malloc(strlen(id) + 24U);
        if (path == NULL) status = KC_KAIMAN_ENOMEM;
        else {
            (void)snprintf(path, strlen(id) + 24U, "/api/session/%s", id);
            status = opencode_request(service, "GET", path, NULL, &response);
            if (status == KC_KAIMAN_OK && response.status >= 400) {
                status = KC_KAIMAN_EHTTP;
            }
            kc_upstream_response_free(&response);
        }
        free(path);
        path = NULL;
        if (status == KC_KAIMAN_OK) {
            *session = id;
            id = NULL;
        } else {
            free(id);
            id = NULL;
            status = KC_KAIMAN_OK;
        }
    }
    if (*session != NULL) goto done;

    status = opencode_split_model(service->model, &provider, &identifier);
    if (status != KC_KAIMAN_OK) goto done;
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) goto done;
    status = kc_buffer_text(buffer, "{\"title\":\"kaiman\",\"model\":{");
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer,
        "\"providerID\":");
    if (status == KC_KAIMAN_OK) status = json_write_text(buffer, provider);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer,
        ",\"id\":");
    if (status == KC_KAIMAN_OK) status = json_write_text(buffer, identifier);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "}}");
    if (status == KC_KAIMAN_OK) body = kc_util_strdup(kc_buffer_value(buffer));
    if (body == NULL) status = KC_KAIMAN_ENOMEM;
    if (status != KC_KAIMAN_OK) goto done;

    status = opencode_request(service, "POST", "/api/session", body, &response);
    if (status == KC_KAIMAN_OK && response.status >= 400) {
        status = KC_KAIMAN_EHTTP;
    }
    if (status == KC_KAIMAN_OK) {
        *session = opencode_session_id(&response);
        if (*session == NULL) status = KC_KAIMAN_EHTTP;
    }
    kc_upstream_response_free(&response);
    if (status == KC_KAIMAN_OK) {
        status = opencode_state_save(state_path, *session, messages, count);
    }

done:
    for (index = 0U; index < count; index++) {
        free(messages[index].role);
        free(messages[index].content);
    }
    kc_buffer_close(buffer);
    free(body);
    free(path);
    free(provider);
    free(identifier);
    free(id);
    return status;
}

/**
 * Append text parts from one OpenCode message.
 * @param value Borrowed message object.
 * @param buffer Destination buffer.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_message_text(const kc_json_value_t *value,
    kc_buffer_t *buffer) {
    kc_json_value_t *content = kc_json_member(value, "content");
    size_t index;
    int status = KC_KAIMAN_OK;

    if (kc_json_type(content) != KC_JSON_ARRAY) {
        content = kc_json_member(value, "parts");
    }
    if (kc_json_type(content) != KC_JSON_ARRAY) return KC_KAIMAN_OK;
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < kc_json_length(content); index++) {
        kc_json_value_t *part = kc_json_item(content, index);
        const char *kind = kc_json_text(kc_json_member(part, "type"));
        const char *text = kc_json_text(kc_json_member(part, "text"));
        if ((kind == NULL || strcmp(kind, "text") == 0) && text != NULL) {
            status = kc_buffer_text(buffer, text);
        }
    }
    return status;
}

/**
 * Check whether one message identifier is already known.
 * @param known Delimited known identifiers.
 * @param id Identifier to test.
 * @return 1 when known, 0 otherwise.
 */
static int opencode_known(const char *known, const char *id) {
    char marker[512];
    size_t length;

    if (known == NULL || id == NULL) return 0;
    length = strlen(id);
    if (length + 2U >= sizeof(marker)) return 0;
    (void)snprintf(marker, sizeof(marker), "|%s|", id);
    return strstr(known, marker) != NULL;
}

/**
 * Collect fresh assistant text and completion state from OpenCode messages.
 * @param body Borrowed response body.
 * @param known Delimited known identifiers to update.
 * @param text Receives appended reply text.
 * @param done Receives completion state.
 * @param error Receives an owned error message.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_collect(const char *body, kc_buffer_t *known,
    kc_buffer_t *text, int *done, char **error) {
    kc_json_value_t *root = NULL;
    kc_json_value_t *items;
    size_t index;
    int status;

    if (body == NULL || known == NULL || text == NULL || done == NULL ||
        error == NULL) {
        return KC_KAIMAN_EINVAL;
    }
    status = kc_json_parse(body, strlen(body), &root);
    if (status != KC_KAIMAN_OK) return status;
    items = kc_json_member(root, "data");
    if (kc_json_type(items) != KC_JSON_ARRAY) {
        kc_json_free(root);
        return KC_KAIMAN_EHTTP;
    }
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < kc_json_length(items); index++) {
        kc_json_value_t *item = kc_json_item(items, index);
        const char *id = kc_json_text(kc_json_member(item, "id"));
        const char *kind = kc_json_text(kc_json_member(item, "type"));
        if (kind == NULL) kind = kc_json_text(kc_json_member(item, "role"));

        if (id != NULL && kind != NULL && strcmp(kind, "assistant") == 0 &&
            !opencode_known(kc_buffer_value(known), id)) {
            status = kc_buffer_text(known, "|");
            if (status == KC_KAIMAN_OK) {
                status = kc_buffer_text(known, id);
            }
            if (status == KC_KAIMAN_OK) {
                status = kc_buffer_text(known, "|");
            }
            if (status == KC_KAIMAN_OK) {
                status = opencode_message_text(item, text);
            }
        } else if (kind != NULL && strcmp(kind, "idle") == 0) {
            const char *outcome = kc_json_text(kc_json_member(item,
                "outcome"));
            if (outcome != NULL && strcmp(outcome, "succeeded") == 0) {
                *done = 1;
            } else {
                kc_json_value_t *failure = kc_json_member(item, "error");
                const char *message = kc_json_text(kc_json_member(failure,
                    "message"));
                if (message != NULL) *error = kc_util_strdup(message);
                else *error = kc_util_strdup("opencode did not complete");
                status = KC_KAIMAN_EHTTP;
            }
        }
    }
    kc_json_free(root);
    return status;
}

/**
 * Build an OpenAI chat completion response from plain text.
 * @param reply Assistant reply.
 * @param model Client-visible model.
 * @param chunk Build a streaming chunk instead of a complete response.
 * @return Owned JSON text, or NULL on failure.
 */
static char *completion_json(const char *reply, const char *model, int chunk) {
    kc_buffer_t *buffer = NULL;
    char *result = NULL;
    int status;

    if (reply == NULL || model == NULL) return NULL;
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) return NULL;
    status = kc_buffer_format(buffer,
        "{\"id\":\"chatcmpl-kaiman\",\"object\":\"%s\",\"created\":%lld,\"model\":",
        chunk ? "chat.completion.chunk" : "chat.completion",
        (long long)time(NULL));
    if (status == KC_KAIMAN_OK) status = json_write_text(buffer, model);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer,
        ",\"choices\":[{\"index\":0,\"");
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(buffer, chunk ? "delta\":{" : "message\":{");
    }
    if (status == KC_KAIMAN_OK) {
        if (!chunk) status = kc_buffer_text(buffer, "\"role\":\"assistant\",");
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer,
        "\"content\":");
    if (status == KC_KAIMAN_OK) status = json_write_text(buffer, reply);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer,
        "},\"finish_reason\":");
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(buffer, chunk ? "null" : "\"stop\"");
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "}]}");
    if (status == KC_KAIMAN_OK) result = kc_util_strdup(
        kc_buffer_value(buffer));
    kc_buffer_close(buffer);
    return result;
}

/**
 * Compare a JSON string with an expected literal.
 * @param value Borrowed value.
 * @param expected Expected text.
 * @return 1 on equality, otherwise 0.
 */
static int native_equals(const kc_json_value_t *value, const char *expected) {
    const char *text = kc_json_text(value);

    return text != NULL && strcmp(text, expected) == 0;
}

/**
 * Append a named JSON field with its serialized value.
 * @param buffer Destination buffer.
 * @param name Field name.
 * @param value Borrowed value.
 * @return KC_KAIMAN_OK or an error status.
 */
static int native_field(kc_buffer_t *buffer, const char *name,
    const kc_json_value_t *value) {
    char *text = NULL;
    int status;

    if (value == NULL) return KC_KAIMAN_OK;
    status = kc_json_stringify(value, &text);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(buffer, ",\"%s\":%s", name, text);
    }
    kc_kaiman_free(text);
    return status;
}

/**
 * Extract text without silently discarding non-text message parts.
 * @param content Borrowed message content.
 * @param buffer Destination buffer.
 * @return KC_KAIMAN_OK or an error status.
 */
static int native_content(const kc_json_value_t *content, kc_buffer_t *buffer) {
    size_t index;
    int status = KC_KAIMAN_OK;

    if (kc_json_type(content) == KC_JSON_STRING) {
        return kc_buffer_text(buffer, kc_json_text(content));
    }
    if (kc_json_type(content) != KC_JSON_ARRAY) return KC_KAIMAN_EINVAL;
    for (index = 0U; index < kc_json_length(content); index++) {
        const kc_json_value_t *part = kc_json_item(content, index);
        const char *text = kc_json_text(kc_json_member(part, "text"));

        if (!native_equals(kc_json_member(part, "type"), "text") ||
            text == NULL) return KC_KAIMAN_EINVAL;
        status = kc_buffer_text(buffer, text);
        if (status != KC_KAIMAN_OK) return status;
    }
    return status;
}

/**
 * Encode messages and separate system instructions when required.
 * @param root Borrowed Chat Completions request.
 * @param adapter Selected native adapter.
 * @param messages Destination message array.
 * @param system Destination system instructions.
 * @return KC_KAIMAN_OK or an error status.
 */
static int native_messages(const kc_json_value_t *root, const char *adapter,
    kc_buffer_t *messages, kc_buffer_t *system) {
    const kc_json_value_t *items = kc_json_member(root, "messages");
    int gemini = strcmp(adapter, "gemini") == 0;
    int bedrock = strcmp(adapter, "bedrock") == 0;
    int separate = gemini || bedrock || strcmp(adapter, "anthropic") == 0;
    size_t index;
    size_t count = 0U;
    int status;

    if (kc_json_type(items) != KC_JSON_ARRAY ||
        kc_json_length(items) == 0U) return KC_KAIMAN_EINVAL;
    status = kc_buffer_text(messages, "[");
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < kc_json_length(items); index++) {
        const kc_json_value_t *item = kc_json_item(items, index);
        const char *role = kc_json_text(kc_json_member(item, "role"));
        kc_buffer_t *content = NULL;
        int instruction;

        if (role == NULL || kc_json_member(item, "tool_calls") != NULL ||
            kc_json_member(item, "function_call") != NULL ||
            kc_json_member(item, "name") != NULL) return KC_KAIMAN_EINVAL;
        instruction = strcmp(role, "system") == 0 ||
            strcmp(role, "developer") == 0;
        if (!instruction && strcmp(role, "user") != 0 &&
            strcmp(role, "assistant") != 0) return KC_KAIMAN_EINVAL;
        status = kc_buffer_open(&content);
        if (status == KC_KAIMAN_OK) {
            status = native_content(kc_json_member(item, "content"), content);
        }
        if (status == KC_KAIMAN_OK && instruction && separate) {
            if (system->size != 0U) status = kc_buffer_text(system, "\n\n");
            if (status == KC_KAIMAN_OK) {
                status = kc_buffer_text(system, kc_buffer_value(content));
            }
        } else if (status == KC_KAIMAN_OK) {
            if (gemini && strcmp(role, "assistant") == 0) role = "model";
            if (strcmp(role, "developer") == 0 &&
                strcmp(adapter, "responses") != 0) role = "system";
            status = kc_buffer_format(messages, "%s{\"role\":",
                count++ == 0U ? "" : ",");
            if (status == KC_KAIMAN_OK) {
                status = json_write_text(messages, role);
            }
            if (status == KC_KAIMAN_OK) {
                status = kc_buffer_text(messages, gemini ? ",\"parts\":[{\"text\":" :
                    bedrock ? ",\"content\":[{\"text\":" : ",\"content\":");
            }
            if (status == KC_KAIMAN_OK) {
                status = json_write_text(messages, kc_buffer_value(content));
            }
            if (status == KC_KAIMAN_OK) {
                status = kc_buffer_text(messages, gemini || bedrock ? "}]}" : "}");
            }
        }
        kc_buffer_close(content);
    }
    if (status == KC_KAIMAN_OK && count == 0U) return KC_KAIMAN_EINVAL;
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(messages, "]");
    return status;
}

/**
 * Build a native text request from the common chat contract.
 * @param root Borrowed Chat Completions request.
 * @param service Selected service.
 * @param body Receives owned native JSON.
 * @return KC_KAIMAN_OK or an error status.
 */
static int native_body(const kc_json_value_t *root,
    const kc_kaiman_service_t *service, char **body) {
    const char *adapter = service->adapter;
    const kc_json_value_t *max = kc_json_member(root, "max_completion_tokens");
    const kc_json_value_t *stop = kc_json_member(root, "stop");
    const kc_json_value_t *temperature = kc_json_member(root, "temperature");
    const kc_json_value_t *top = kc_json_member(root, "top_p");
    const kc_json_value_t *n = kc_json_member(root, "n");
    kc_buffer_t *messages = NULL;
    kc_buffer_t *system = NULL;
    kc_buffer_t *buffer = NULL;
    int gemini = strcmp(adapter, "gemini") == 0;
    int bedrock = strcmp(adapter, "bedrock") == 0;
    int anthropic = strcmp(adapter, "anthropic") == 0;
    int responses = strcmp(adapter, "responses") == 0;
    int ollama = strcmp(adapter, "ollama") == 0;
    int status;

    *body = NULL;
    if (max == NULL) max = kc_json_member(root, "max_tokens");
    if (kc_json_member(root, "tools") != NULL ||
        kc_json_member(root, "functions") != NULL ||
        kc_json_member(root, "tool_choice") != NULL ||
        kc_json_member(root, "response_format") != NULL ||
        (n != NULL && kc_json_number(n) != 1.0) ||
        (max != NULL && (kc_json_type(max) != KC_JSON_NUMBER ||
            kc_json_number(max) < 1.0 || kc_json_number(max) > INT_MAX)) ||
        (responses && stop != NULL)) return KC_KAIMAN_EINVAL;
    if (service->model == NULL || *service->model == '\0') {
        return KC_KAIMAN_EINVAL;
    }
    status = kc_buffer_open(&messages);
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&system);
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&buffer);
    if (status == KC_KAIMAN_OK) {
        status = native_messages(root, adapter, messages, system);
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "{");
    if (status == KC_KAIMAN_OK && !gemini && !bedrock) {
        status = kc_buffer_text(buffer, "\"model\":");
        if (status == KC_KAIMAN_OK) {
            status = json_write_text(buffer, service->model);
        }
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, ",");
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(buffer, "\"%s\":%s",
            gemini ? "contents" : responses ? "input" : "messages",
            kc_buffer_value(messages));
    }
    if (status == KC_KAIMAN_OK && system->size != 0U) {
        status = kc_buffer_text(buffer, gemini ?
            ",\"systemInstruction\":{\"parts\":[{\"text\":" : bedrock ?
            ",\"system\":[{\"text\":" : ",\"system\":");
        if (status == KC_KAIMAN_OK) {
            status = json_write_text(buffer, kc_buffer_value(system));
        }
        if (status == KC_KAIMAN_OK && (gemini || bedrock)) {
            status = kc_buffer_text(buffer, gemini ? "}]}" : "}]");
        }
    }
    if (status == KC_KAIMAN_OK && !gemini && !bedrock) {
        status = kc_buffer_text(buffer, ",\"stream\":false");
    }
    if (status == KC_KAIMAN_OK && responses) {
        status = kc_buffer_text(buffer, ",\"store\":false");
    }
    if (status == KC_KAIMAN_OK && (gemini || bedrock || ollama)) {
        status = kc_buffer_format(buffer, ",\"%s\":{",
            gemini ? "generationConfig" : bedrock ? "inferenceConfig" : "options");
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_format(buffer, "\"%s\":%d",
                gemini ? "maxOutputTokens" : bedrock ? "maxTokens" : "num_predict",
                max != NULL ? (int)kc_json_number(max) : 4096);
        }
    } else if (status == KC_KAIMAN_OK && (max != NULL || anthropic)) {
        status = kc_buffer_format(buffer, ",\"%s\":%d",
            responses ? "max_output_tokens" : "max_tokens",
            max != NULL ? (int)kc_json_number(max) : 4096);
    }
    if (status == KC_KAIMAN_OK) {
        status = native_field(buffer, "temperature", temperature);
    }
    if (status == KC_KAIMAN_OK) {
        status = native_field(buffer, gemini || bedrock ? "topP" : "top_p", top);
    }
    if (status == KC_KAIMAN_OK && stop != NULL) {
        const char *key = gemini || bedrock ? "stopSequences" :
            anthropic ? "stop_sequences" : "stop";

        if (kc_json_type(stop) == KC_JSON_STRING) {
            status = kc_buffer_format(buffer, ",\"%s\":[", key);
            if (status == KC_KAIMAN_OK) {
                status = json_write_text(buffer, kc_json_text(stop));
            }
            if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "]");
        } else if (kc_json_type(stop) == KC_JSON_ARRAY) {
            status = native_field(buffer, key, stop);
        } else {
            status = KC_KAIMAN_EINVAL;
        }
    }
    if (status == KC_KAIMAN_OK && (gemini || bedrock || ollama)) {
        status = kc_buffer_text(buffer, "}");
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "}");
    if (status == KC_KAIMAN_OK) {
        *body = kc_util_strdup(kc_buffer_value(buffer));
        if (*body == NULL) status = KC_KAIMAN_ENOMEM;
    }
    kc_buffer_close(buffer);
    kc_buffer_close(system);
    kc_buffer_close(messages);
    return status;
}

/**
 * Resolve the native API URL using an encoded model path segment.
 * @param service Selected service.
 * @return Owned URL, or NULL on failure.
 */
static char *native_url(const kc_kaiman_service_t *service) {
    const char *adapter = service->adapter;
    const unsigned char *cursor;
    const char *model = service->model;
    kc_buffer_t *path = NULL;
    char *url = NULL;
    int gemini = strcmp(adapter, "gemini") == 0;
    int status;

    if (strcmp(adapter, "anthropic") == 0) {
        return api_url(service->base_url, "/messages");
    }
    if (strcmp(adapter, "responses") == 0) {
        return api_url(service->base_url, "/responses");
    }
    if (strcmp(adapter, "ollama") == 0) {
        return api_url(service->base_url, "/api/chat");
    }
    if (gemini && strncmp(model, "models/", 7U) == 0) model += 7U;
    status = kc_buffer_open(&path);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(path, gemini ? "/models/" : "/model/");
    }
    for (cursor = (const unsigned char *)model;
        status == KC_KAIMAN_OK && *cursor != '\0'; cursor++) {
        unsigned char byte = *cursor;

        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
            byte == '.' || byte == '~') {
            status = kc_buffer_append(path, &byte, 1U);
        } else {
            status = kc_buffer_format(path, "%%%02X", (unsigned int)byte);
        }
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(path, gemini ? ":generateContent" : "/converse");
    }
    if (status == KC_KAIMAN_OK) {
        url = api_url(service->base_url, kc_buffer_value(path));
    }
    kc_buffer_close(path);
    return url;
}

/**
 * Extract text blocks from a native assistant message.
 * @param items Borrowed array of text blocks.
 * @param reply Destination reply.
 * @return KC_KAIMAN_OK or an error status.
 */
static int native_blocks(const kc_json_value_t *items, kc_buffer_t *reply) {
    size_t index;
    int status = KC_KAIMAN_OK;

    if (kc_json_type(items) != KC_JSON_ARRAY) return KC_KAIMAN_EFORMAT;
    for (index = 0U; index < kc_json_length(items); index++) {
        const kc_json_value_t *item = kc_json_item(items, index);
        const kc_json_value_t *type = kc_json_member(item, "type");
        const char *text = kc_json_text(kc_json_member(item, "text"));

        if (native_equals(type, "thinking") ||
            native_equals(type, "redacted_thinking") ||
            kc_json_boolean(kc_json_member(item, "thought"))) continue;
        if (native_equals(type, "refusal")) {
            text = kc_json_text(kc_json_member(item, "refusal"));
        }
        if (text == NULL) return KC_KAIMAN_EFORMAT;
        status = kc_buffer_text(reply, text);
        if (status != KC_KAIMAN_OK) return status;
    }
    return status;
}

/**
 * Normalize a native reply to text and a Chat Completions finish reason.
 * @param adapter Selected adapter.
 * @param root Borrowed native response.
 * @param reply Destination reply.
 * @param finish Receives a stable finish reason.
 * @return KC_KAIMAN_OK or an error status.
 */
static int native_reply(const char *adapter, const kc_json_value_t *root,
    kc_buffer_t *reply, const char **finish) {
    const kc_json_value_t *item;
    const kc_json_value_t *items;
    const char *reason = NULL;
    const char *text;
    size_t index;
    int status = KC_KAIMAN_OK;

    *finish = "stop";
    if (strcmp(adapter, "anthropic") == 0) {
        reason = kc_json_text(kc_json_member(root, "stop_reason"));
        status = native_blocks(kc_json_member(root, "content"), reply);
    } else if (strcmp(adapter, "gemini") == 0) {
        item = kc_json_item(kc_json_member(root, "candidates"), 0U);
        reason = kc_json_text(kc_json_member(item, "finishReason"));
        items = kc_json_member(kc_json_member(item, "content"), "parts");
        status = native_blocks(items, reply);
    } else if (strcmp(adapter, "bedrock") == 0) {
        reason = kc_json_text(kc_json_member(root, "stopReason"));
        item = kc_json_member(kc_json_member(root, "output"), "message");
        status = native_blocks(kc_json_member(item, "content"), reply);
    } else if (strcmp(adapter, "ollama") == 0) {
        item = kc_json_member(root, "message");
        text = kc_json_text(kc_json_member(item, "content"));
        reason = kc_json_text(kc_json_member(root, "done_reason"));
        if (text == NULL || !kc_json_boolean(kc_json_member(root, "done")) ||
            kc_json_length(kc_json_member(item, "tool_calls")) != 0U) {
            return KC_KAIMAN_EFORMAT;
        }
        status = kc_buffer_text(reply, text);
    } else {
        items = kc_json_member(root, "output");
        if (kc_json_type(items) != KC_JSON_ARRAY) return KC_KAIMAN_EFORMAT;
        if (native_equals(kc_json_member(root, "status"), "incomplete")) {
            item = kc_json_member(root, "incomplete_details");
            reason = kc_json_text(kc_json_member(item, "reason"));
        } else if (!native_equals(kc_json_member(root, "status"), "completed")) {
            return KC_KAIMAN_EFORMAT;
        }
        for (index = 0U; status == KC_KAIMAN_OK &&
            index < kc_json_length(items); index++) {
            item = kc_json_item(items, index);
            if (native_equals(kc_json_member(item, "type"), "reasoning")) continue;
            if (!native_equals(kc_json_member(item, "type"), "message")) {
                return KC_KAIMAN_EFORMAT;
            }
            status = native_blocks(kc_json_member(item, "content"), reply);
        }
    }
    if (status != KC_KAIMAN_OK) return status;
    if (reason != NULL) {
        if (strcmp(reason, "max_tokens") == 0 ||
            strcmp(reason, "max_output_tokens") == 0 ||
            strcmp(reason, "MAX_TOKENS") == 0 || strcmp(reason, "length") == 0) {
            *finish = "length";
        } else if (strcmp(reason, "SAFETY") == 0 ||
            strcmp(reason, "content_filter") == 0 ||
            strcmp(reason, "guardrail_intervened") == 0 ||
            strcmp(reason, "refusal") == 0) {
            *finish = "content_filter";
        } else if (strcmp(reason, "tool_use") == 0) {
            return KC_KAIMAN_EFORMAT;
        }
    }
    return KC_KAIMAN_OK;
}

/**
 * Serialize a native text reply as a complete response or stream frame.
 * @param reply Assistant text.
 * @param model Client-visible model name.
 * @param finish Finish reason, or NULL for a non-final chunk.
 * @param chunk Whether to produce an SSE chunk.
 * @return Owned JSON text, or NULL on failure.
 */
static char *native_completion(const char *reply, const char *model,
    const char *finish, int chunk) {
    kc_buffer_t *buffer = NULL;
    char *result = NULL;
    int status = kc_buffer_open(&buffer);

    if (status != KC_KAIMAN_OK) return NULL;
    status = kc_buffer_format(buffer,
        "{\"id\":\"chatcmpl-kaiman\",\"object\":\"%s\",\"created\":%lld,\"model\":",
        chunk ? "chat.completion.chunk" : "chat.completion",
        (long long)time(NULL));
    if (status == KC_KAIMAN_OK) status = json_write_text(buffer, model);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(buffer, ",\"choices\":[{\"index\":0,\"%s\":{",
            chunk ? "delta" : "message");
    }
    if (status == KC_KAIMAN_OK && reply != NULL) {
        status = kc_buffer_text(buffer, "\"role\":\"assistant\",\"content\":");
        if (status == KC_KAIMAN_OK) status = json_write_text(buffer, reply);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(buffer, "},\"finish_reason\":");
    }
    if (status == KC_KAIMAN_OK) {
        status = finish != NULL ? json_write_text(buffer, finish) :
            kc_buffer_text(buffer, "null");
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "}]}");
    if (status == KC_KAIMAN_OK) {
        result = kc_util_strdup(kc_buffer_value(buffer));
    }
    kc_buffer_close(buffer);
    return result;
}

/**
 * Forward a text chat through a native provider API.
 * @param gateway Open gateway.
 * @param peer Borrowed client peer.
 * @param root Borrowed Chat Completions request.
 * @param service Selected service.
 * @return KC_KAIMAN_OK or an error status.
 */
static int native_chat(kc_kaiman_gateway_t *gateway, kc_netl_peer_t *peer,
    const kc_json_value_t *root, const kc_kaiman_service_t *service) {
    const char *model = kc_json_text(kc_json_member(root, "model"));
    const char *finish = NULL;
    const char *type;
    kc_upstream_field_t fields[2];
    kc_upstream_request_t request;
    kc_upstream_response_t response;
    kc_kaiman_stream_t stream;
    kc_json_value_t *value = NULL;
    kc_buffer_t *reply = NULL;
    char *authorization = NULL;
    char *body = NULL;
    char *url = NULL;
    char *text = NULL;
    char *final = NULL;
    size_t count = 0U;
    int streaming = kc_json_boolean(kc_json_member(root, "stream"));
    int status;

    if (model == NULL || *model == '\0') model = service->name;
    memset(&response, 0, sizeof(response));
    memset(&request, 0, sizeof(request));
    memset(&stream, 0, sizeof(stream));
    status = native_body(root, service, &body);
    if (status != KC_KAIMAN_OK) {
        status = gateway_failure(gateway, peer, 400,
            "unsupported or invalid native text chat request");
        goto done;
    }
    status = service_authorization(service, &authorization);
    if (status != KC_KAIMAN_OK) {
        status = gateway_failure(gateway, peer, 400,
            "the service credential is unavailable");
        goto done;
    }
    url = native_url(service);
    if (url == NULL) {
        status = gateway_failure(gateway, peer, 400, "invalid service URL");
        goto done;
    }
    if (authorization != NULL) {
        fields[count].name = "Authorization";
        fields[count].value = authorization;
        if (strncmp(authorization, "Bearer ", 7U) == 0) {
            if (strcmp(service->adapter, "anthropic") == 0) {
                fields[count].name = "x-api-key";
                fields[count].value = authorization + 7U;
            } else if (strcmp(service->adapter, "gemini") == 0) {
                fields[count].name = "x-goog-api-key";
                fields[count].value = authorization + 7U;
            }
        }
        count++;
    }
    if (strcmp(service->adapter, "anthropic") == 0) {
        fields[count].name = "anthropic-version";
        fields[count++].value = "2023-06-01";
    }
    request.method = "POST";
    request.url = url;
    request.headers = fields;
    request.header_count = count;
    request.body = body;
    request.body_size = strlen(body);
    request.timeout = kc_kaiman_service_timeout(service);
    status = kc_upstream_send(&request, &response);
    if (status != KC_KAIMAN_OK) {
        status = gateway_failure(gateway, peer, 502, kc_upstream_strerror(status));
        goto done;
    }
    if (response.status >= 400) {
        type = response.content_type != NULL && *response.content_type != '\0' ?
            response.content_type : "application/json";
        status = gateway_respond(gateway, peer, response.status, type,
            response.body, response.body_size);
        goto done;
    }
    status = kc_json_parse(response.body, response.body_size, &value);
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&reply);
    if (status == KC_KAIMAN_OK) {
        status = native_reply(service->adapter, value, reply, &finish);
    }
    if (status != KC_KAIMAN_OK) {
        status = gateway_failure(gateway, peer, 502, "invalid native response");
        goto done;
    }
    text = native_completion(kc_buffer_value(reply), model,
        streaming ? NULL : finish, streaming);
    if (streaming) final = native_completion(NULL, model, finish, 1);
    if (text == NULL || (streaming && final == NULL)) {
        status = gateway_failure(gateway, peer, 500, "could not build a response");
        goto done;
    }
    if (streaming) {
        stream.peer = peer;
        stream.gateway = gateway;
        status = gateway_sse_data(gateway, &stream, text);
        if (status == KC_KAIMAN_OK) {
            status = gateway_sse_data(gateway, &stream, final);
        }
        if (status == KC_KAIMAN_OK) {
            status = gateway_sse_data(gateway, &stream, "[DONE]");
        }
        if (stream.headers_sent) {
            (void)kc_netl_peer_send(peer, "0\r\n\r\n", 5U);
            kc_netl_peer_close(peer);
        }
    } else {
        status = gateway_json(gateway, peer, 200, text);
    }

done:
    kc_kaiman_free(final);
    kc_kaiman_free(text);
    kc_buffer_close(reply);
    kc_json_free(value);
    kc_upstream_response_free(&response);
    kc_kaiman_free(url);
    kc_kaiman_free(body);
    kc_kaiman_free(authorization);
    return status;
}

/**
 * Forward one chat request to an OpenCode server using one persistent session.
 * @param gateway Open gateway.
 * @param peer Borrowed client peer.
 * @param root Borrowed chat request object.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_chat(kc_kaiman_gateway_t *gateway, kc_netl_peer_t *peer,
    const kc_json_value_t *root) {
    const char *requested = kc_json_text(kc_json_member(root, "model"));
    kc_kaiman_service_t *service = NULL;
    kc_upstream_response_t response;
    kc_kaiman_message_t messages[KAIMAN_MAX_HISTORY];
    kc_kaiman_stream_t stream;
    kc_buffer_t *prompt = NULL;
    kc_buffer_t *known = NULL;
    kc_buffer_t *reply = NULL;
    kc_buffer_t *before_text = NULL;
    char *state_path = NULL;
    char *session = NULL;
    char *state_id = NULL;
    char *provider = NULL;
    char *identifier = NULL;
    char *path = NULL;
    char *body = NULL;
    char *text = NULL;
    char *failure = NULL;
    size_t count = 0U;
    size_t index;
    int stream_mode;
    int done = 0;
    int status;
    int rc;

    status = gateway_select(gateway->manager, requested, &service);
    if (status != KC_KAIMAN_OK) {
        return gateway_failure(gateway, peer, 404,
            "no configured service matches the requested model");
    }
    if (service->model == NULL || strchr(service->model, '/') == NULL) {
        return gateway_failure(gateway, peer, 400,
            "opencode model must be providerID/modelID");
    }
    stream_mode = kc_json_boolean(kc_json_member(root, "stream"));
    memset(messages, 0, sizeof(messages));
    memset(&stream, 0, sizeof(stream));
    kc_mutex_lock(&gateway->mutex);
    state_path = opencode_session_path(service);
    if (state_path == NULL) status = KC_KAIMAN_ENOMEM;
    if (status == KC_KAIMAN_OK) {
        status = opencode_ensure_session(service, state_path, &session);
    }
    if (status == KC_KAIMAN_OK) {
        status = request_prompt(root, &text);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_open(&prompt);
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(prompt, text);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_open(&known);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_open(&reply);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_open(&before_text);
    }
    if (status == KC_KAIMAN_OK) {
        path = (char *)malloc(strlen(session) + 24U);
        if (path == NULL) status = KC_KAIMAN_ENOMEM;
        else {
            (void)snprintf(path, strlen(session) + 24U,
                "/api/session/%s/message", session);
            rc = opencode_request(service, "GET", path, NULL, &response);
            if (rc == KC_KAIMAN_OK && response.status >= 400) {
                rc = KC_KAIMAN_EHTTP;
            }
            if (rc == KC_KAIMAN_OK) {
                (void)opencode_collect(response.body, known, before_text,
                    &done, &failure);
                kc_kaiman_free(failure);
                failure = NULL;
                done = 0;
                status = KC_KAIMAN_OK;
            } else {
                status = rc;
            }
            kc_upstream_response_free(&response);
        }
    }
    free(path);
    path = NULL;
    kc_buffer_close(before_text);
    before_text = NULL;
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_open(&stream.sse);
    }
    if (status == KC_KAIMAN_OK) {
        status = opencode_split_model(service->model, &provider, &identifier);
    }
    if (status == KC_KAIMAN_OK) {
        kc_buffer_t *buffer = NULL;
        status = kc_buffer_open(&buffer);
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_text(buffer, "{\"text\":");
        }
        if (status == KC_KAIMAN_OK) {
            status = json_write_text(buffer, kc_buffer_value(prompt));
        }
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_text(buffer,
                ",\"model\":{\"providerID\":");
        }
        if (status == KC_KAIMAN_OK) {
            status = json_write_text(buffer, provider);
        }
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_text(buffer, ",\"id\":");
        }
        if (status == KC_KAIMAN_OK) {
            status = json_write_text(buffer, identifier);
        }
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_text(buffer, "}}");
        }
        if (status == KC_KAIMAN_OK) body = kc_util_strdup(
            kc_buffer_value(buffer));
        if (body == NULL) status = KC_KAIMAN_ENOMEM;
        kc_buffer_close(buffer);
    }
    if (status == KC_KAIMAN_OK) {
        path = (char *)malloc(strlen(session) + 32U);
        if (path == NULL) status = KC_KAIMAN_ENOMEM;
        else {
            (void)snprintf(path, strlen(session) + 32U,
                "/api/session/%s/prompt", session);
            rc = opencode_request(service, "POST", path, body, &response);
            if (rc == KC_KAIMAN_OK && response.status >= 400) {
                status = KC_KAIMAN_EHTTP;
                failure = kc_util_strdup(response.body);
            } else if (rc != KC_KAIMAN_OK) {
                status = rc;
            }
            kc_upstream_response_free(&response);
        }
    }
    free(path);
    path = NULL;
    if (status == KC_KAIMAN_OK) {
        double deadline = kc_util_seconds() +
            (double)kc_kaiman_service_timeout(service) * 2.0;
        path = (char *)malloc(strlen(session) + 24U);
        if (path == NULL) status = KC_KAIMAN_ENOMEM;
        else {
            (void)snprintf(path, strlen(session) + 24U,
                "/api/session/%s/message", session);
            while (!done && kc_util_seconds() < deadline) {
                rc = opencode_request(service, "GET", path, NULL, &response);
                if (rc != KC_KAIMAN_OK) {
                    status = rc;
                    break;
                }
                if (response.status >= 400) {
                    status = KC_KAIMAN_EHTTP;
                    failure = kc_util_strdup(response.body);
                    kc_upstream_response_free(&response);
                    break;
                }
                status = opencode_collect(response.body, known, reply,
                    &done, &failure);
                kc_upstream_response_free(&response);
                if (status != KC_KAIMAN_OK || done) break;
                kc_util_sleep(200);
            }
            if (status == KC_KAIMAN_OK && !done) {
                status = KC_KAIMAN_ETIMEOUT;
            }
        }
    }
    free(path);
    path = NULL;
    if (status == KC_KAIMAN_OK) {
        status = opencode_state_load(state_path, &state_id, messages, &count);
    }
    if (status == KC_KAIMAN_OK) {
        if (count < KAIMAN_MAX_HISTORY) {
            messages[count].role = kc_util_strdup("user");
            messages[count].content = kc_util_strdup(kc_buffer_value(prompt));
            if (messages[count].role == NULL || messages[count].content == NULL) {
                status = KC_KAIMAN_ENOMEM;
            } else {
                count++;
            }
        }
    }
    if (status == KC_KAIMAN_OK) {
        if (count < KAIMAN_MAX_HISTORY) {
            messages[count].role = kc_util_strdup("assistant");
            messages[count].content = kc_util_strdup(kc_buffer_value(reply));
            if (messages[count].role == NULL || messages[count].content == NULL) {
                status = KC_KAIMAN_ENOMEM;
            } else {
                count++;
            }
        }
    }
    if (status == KC_KAIMAN_OK) {
        status = opencode_state_save(state_path, session,
            messages, count);
    }
    kc_mutex_unlock(&gateway->mutex);

    if (status != KC_KAIMAN_OK) {
        gateway_failure(gateway, peer, 502,
            failure != NULL ? failure : kc_kaiman_strerror(status));
    } else if (stream_mode) {
        char *chunk = completion_json("", "",
            1);
        stream.peer = peer;
        stream.gateway = gateway;
        stream.model = kc_util_strdup(requested != NULL && *requested != '\0'
            ? requested : service->name);
        if (chunk != NULL) {
            (void)gateway_sse_data(gateway, &stream, chunk);
            kc_kaiman_free(chunk);
        }
        chunk = completion_json(kc_buffer_value(reply),
            stream.model != NULL ? stream.model : "", 1);
        if (chunk != NULL) {
            (void)gateway_sse_data(gateway, &stream, chunk);
            kc_kaiman_free(chunk);
        }
        (void)gateway_sse_data(gateway, &stream, "[DONE]");
        if (stream.headers_sent) {
            (void)kc_netl_peer_send(peer, "0\r\n\r\n", 5U);
            kc_netl_peer_close(peer);
        }
        free(stream.model);
    } else {
        text = completion_json(kc_buffer_value(reply),
            requested != NULL && *requested != '\0' ? requested : service->name,
            0);
        if (text != NULL) {
            (void)gateway_json(gateway, peer, 200, text);
        } else {
            gateway_failure(gateway, peer, 500, "could not build a response");
        }
    }

    kc_kaiman_free(text);
    kc_kaiman_free(failure);
    kc_buffer_close(stream.sse);
    kc_buffer_close(before_text);
    kc_buffer_close(reply);
    kc_buffer_close(known);
    kc_buffer_close(prompt);
    for (index = 0U; index < KAIMAN_MAX_HISTORY; index++) {
        free(messages[index].role);
        free(messages[index].content);
    }
    free(body);
    free(provider);
    free(identifier);
    free(session);
    free(state_id);
    free(state_path);
    return KC_KAIMAN_OK;
}

/**
 * Build the OpenAI-compatible model list from saved services.
 * @param kaiman Open manager.
 * @return Owned JSON text, or NULL on failure.
 */
static char *gateway_models(const kc_kaiman_t *kaiman) {
    kc_buffer_t *buffer = NULL;
    char *result = NULL;
    size_t index;
    int status;

    if (kaiman == NULL) return NULL;
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) return NULL;
    status = kc_buffer_text(buffer,
        "{\"object\":\"list\",\"data\":[");
    for (index = 0U; status == KC_KAIMAN_OK && index < kaiman->count;
        index++) {
        const kc_kaiman_service_t *service = kaiman->services[index];
        if (index != 0U) status = kc_buffer_text(buffer, ",");
        if (status != KC_KAIMAN_OK) break;
        status = kc_buffer_text(buffer, "{\"id\":");
        if (status != KC_KAIMAN_OK) break;
        status = json_write_text(buffer, service->name);
        if (status != KC_KAIMAN_OK) break;
        status = kc_buffer_text(buffer,
            ",\"object\":\"model\",\"created\":0,\"owned_by\":");
        if (status != KC_KAIMAN_OK) break;
        status = json_write_text(buffer, service->adapter != NULL
            ? service->adapter : "kaiman");
        if (status != KC_KAIMAN_OK) break;
        status = kc_buffer_text(buffer, ",\"model\":");
        if (status != KC_KAIMAN_OK) break;
        status = json_write_text(buffer, service->model != NULL
            ? service->model : "");
        if (status != KC_KAIMAN_OK) break;
        status = kc_buffer_text(buffer, "}");
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "]}");
    if (status == KC_KAIMAN_OK) result = kc_util_strdup(
        kc_buffer_value(buffer));
    kc_buffer_close(buffer);
    return result;
}

/**
 * Handle one parsed HTTP request.
 * @param request Borrowed request.
 * @param userdata Borrowed connection state.
 * @return void
 */
static void gateway_request(const kc_http_request_t *request, void *userdata) {
    kc_kaiman_connection_t *connection = (kc_kaiman_connection_t *)userdata;
    kc_kaiman_gateway_t *gateway;
    kc_json_value_t *root = NULL;
    char *models;

    if (connection == NULL || connection->gateway == NULL ||
        request == NULL) {
        return;
    }
    gateway = connection->gateway;
    (void)kc_buffer_close(connection->body);
    if (kc_buffer_open(&connection->body) != KC_KAIMAN_OK) {
        (void)gateway_failure(gateway, connection->peer, 500,
            "out of memory");
        return;
    }
    if (request->body_size > 0U && kc_buffer_append(connection->body,
        request->body, request->body_size) != KC_KAIMAN_OK) {
        (void)gateway_failure(gateway, connection->peer, 413,
            "request body is too large");
        return;
    }

    if (strcmp(request->method, "GET") == 0 &&
        strcmp(request->path, "/health") == 0) {
        (void)gateway_json(gateway, connection->peer, 200,
            "{\"status\":\"ok\"}");
        return;
    }
    if (strcmp(request->method, "GET") == 0 &&
        strcmp(request->path, "/v1/models") == 0) {
        models = gateway_models(gateway->manager);
        if (models == NULL) {
            (void)gateway_failure(gateway, connection->peer, 500,
                "could not build the model list");
        } else {
            (void)gateway_json(gateway, connection->peer, 200, models);
            kc_kaiman_free(models);
        }
        return;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/v1/chat/completions") == 0) {
        int status = kc_json_parse(kc_buffer_value(connection->body),
            connection->body->size, &root);
        if (status != KC_KAIMAN_OK) {
            (void)gateway_failure(gateway, connection->peer, 400,
                "invalid JSON request body");
            return;
        }
        if (kc_json_type(root) != KC_JSON_OBJECT) {
            (void)gateway_failure(gateway, connection->peer, 400,
                "expected a JSON object");
            kc_json_free(root);
            return;
        }
        {
            const char *adapter = NULL;
            const char *requested = kc_json_text(kc_json_member(root, "model"));
            kc_kaiman_service_t *service = NULL;
            if (gateway_select(gateway->manager, requested, &service) ==
                KC_KAIMAN_OK) {
                adapter = service->adapter;
            }
            if (adapter != NULL && strcmp(adapter, "opencode") == 0) {
                (void)opencode_chat(gateway, connection->peer, root);
            } else if (adapter != NULL && strcmp(adapter, "openai") != 0) {
                (void)native_chat(gateway, connection->peer, root, service);
            } else {
                (void)openai_chat(gateway, connection->peer, root);
            }
        }
        kc_json_free(root);
        return;
    }
    (void)gateway_failure(gateway, connection->peer, 404,
        "unknown endpoint");
}

/**
 * Report one HTTP parser failure.
 * @param status Parser status.
 * @param userdata Borrowed connection state.
 * @return void
 */
static void gateway_parse_error(int status, void *userdata) {
    kc_kaiman_connection_t *connection = (kc_kaiman_connection_t *)userdata;
    (void)status;
    if (connection == NULL) return;
    connection->failed = 1;
}

/**
 * Receive bytes from one client peer.
 * @param peer Borrowed peer.
 * @param data Borrowed bytes.
 * @param size Byte count.
 * @param userdata Borrowed gateway.
 * @return void
 */
static void gateway_receive(kc_netl_peer_t *peer, const void *data,
    size_t size, void *userdata) {
    kc_kaiman_gateway_t *gateway = (kc_kaiman_gateway_t *)userdata;
    kc_kaiman_connection_t *connection;
    kc_kaiman_connection_t *items;
    int status;

    if (gateway == NULL || peer == NULL) return;
    connection = gateway_connection(gateway, peer);
    if (connection == NULL) {
        items = (kc_kaiman_connection_t *)realloc(gateway->connections,
            (gateway->connection_count + 1U) * sizeof(*items));
        if (items == NULL) {
            (void)gateway_failure(gateway, peer, 500, "out of memory");
            return;
        }
        gateway->connections = items;
        connection = &gateway->connections[gateway->connection_count++];
        memset(connection, 0, sizeof(*connection));
        connection->peer = peer;
        connection->gateway = gateway;
        status = kc_http_parser_open(&connection->parser, gateway_request,
            NULL, gateway_parse_error, connection);
        if (status != KC_HTTP_OK) {
            gateway_forget(gateway, peer);
            (void)gateway_failure(gateway, peer, 500,
                "could not parse the HTTP request");
            return;
        }
    }
    status = kc_http_parser_write(connection->parser, data, size);
    if (status != KC_HTTP_OK || connection->failed) {
        if (!connection->failed) {
            (void)gateway_failure(gateway, peer, 400,
                "malformed HTTP request");
        }
        kc_netl_peer_close(peer);
    }
}

/**
 * Forget one disconnected client.
 * @param peer Closing peer.
 * @param userdata Borrowed gateway.
 * @return void
 */
static void gateway_disconnect(kc_netl_peer_t *peer, void *userdata) {
    gateway_forget((kc_kaiman_gateway_t *)userdata, peer);
}

/**
 * Record one asynchronous listener failure.
 * @param status Public failure status.
 * @param userdata Borrowed gateway.
 * @return void
 */
static void gateway_listener_error(int status, void *userdata) {
    kc_kaiman_gateway_t *gateway = (kc_kaiman_gateway_t *)userdata;
    gateway_error(gateway, "listener failed: %s", kc_netl_strerror(status));
    gateway->stop = 1;
}

#ifndef _WIN32
/**
 * Ask the active gateway to stop from a signal handler.
 * @param signal_number Delivered signal.
 * @return void
 */
static void gateway_signal(int signal_number) {
    (void)signal_number;
    if (kc_kaiman_signal_gateway != NULL) {
        kc_kaiman_signal_gateway->stop = 1;
    }
}
#endif

/**
 * Open an HTTP gateway bound to an existing service manager.
 * @param out Receives the gateway, or NULL on failure.
 * @param kaiman Open service manager.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_gateway_open(kc_kaiman_gateway_t **out, kc_kaiman_t *kaiman) {
    kc_kaiman_gateway_t *gateway;
    kc_netl_options_t options;
    const char *address;
    const char *separator;
    char host[256];
    unsigned short port;
    int status;

    if (out == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    if (kaiman == NULL) return KC_KAIMAN_EINVAL;
    address = kc_kaiman_address(kaiman);
    if (address == NULL) return KC_KAIMAN_EINVAL;
    separator = strrchr(address, ':');
    if (separator == NULL || separator == address ||
        (size_t)(separator - address) >= sizeof(host)) {
        return KC_KAIMAN_EINVAL;
    }
    port = (unsigned short)strtoul(separator + 1U, NULL, 10);
    if (port == 0U) return KC_KAIMAN_EINVAL;
    gateway = (kc_kaiman_gateway_t *)calloc(1, sizeof(*gateway));
    if (gateway == NULL) return KC_KAIMAN_ENOMEM;
    gateway->manager = kaiman;
    gateway->address = kc_util_strdup(address);
    status = kc_mutex_open(&gateway->mutex);
    if (status != KC_KAIMAN_OK) gateway->mutex_ready = 0;
    else gateway->mutex_ready = 1;
    if (gateway->address == NULL || !gateway->mutex_ready) {
        status = KC_KAIMAN_ENOMEM;
        kc_kaiman_gateway_close(gateway);
        return status;
    }
    memcpy(host, address, (size_t)(separator - address));
    host[separator - address] = '\0';
    memset(&options, 0, sizeof(options));
    options.host = host;
    options.port = port;
    options.protocol = KC_NETL_TCP;
    status = kc_netl_open(&gateway->listener, &options, gateway_receive,
        gateway_disconnect, gateway_listener_error, gateway);
    if (status != KC_NETL_OK) {
        gateway_error(gateway, "could not listen on %s: %s", address,
            kc_netl_strerror(status));
        kc_kaiman_gateway_close(gateway);
        return KC_KAIMAN_EIO;
    }
    *out = gateway;
    return KC_KAIMAN_OK;
}

/**
 * Close a gateway and stop accepting requests.
 * @param gateway Gateway to close, or NULL.
 * @return void
 */
void kc_kaiman_gateway_close(kc_kaiman_gateway_t *gateway) {
    size_t index;

    if (gateway == NULL) return;
    gateway->stop = 1;
    if (kc_kaiman_signal_gateway == gateway) kc_kaiman_signal_gateway = NULL;
    kc_netl_close(gateway->listener);
    for (index = 0U; index < gateway->connection_count; index++) {
        kc_http_parser_close(gateway->connections[index].parser);
        kc_buffer_close(gateway->connections[index].body);
    }
    free(gateway->connections);
    if (gateway->mutex_ready) kc_mutex_close(&gateway->mutex);
    free(gateway->address);
    free(gateway->error);
    free(gateway);
}

/**
 * Serve requests until the gateway is stopped.
 * @param gateway Open gateway.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_gateway_run(kc_kaiman_gateway_t *gateway) {
    if (gateway == NULL || gateway->listener == NULL) {
        return KC_KAIMAN_EINVAL;
    }
    kc_kaiman_signal_gateway = gateway;
#ifndef _WIN32
    (void)signal(SIGTERM, gateway_signal);
    (void)signal(SIGINT, gateway_signal);
#endif
    while (!gateway->stop) {
        kc_util_sleep(100);
    }
    kc_kaiman_signal_gateway = NULL;
    return KC_KAIMAN_OK;
}

/**
 * Request that a running gateway stop after active work completes.
 * @param gateway Open gateway, or NULL.
 * @return void
 */
void kc_kaiman_gateway_stop(kc_kaiman_gateway_t *gateway) {
    if (gateway != NULL) gateway->stop = 1;
}

/**
 * Borrow the address on which the gateway listens.
 * @param gateway Open gateway.
 * @return NUL terminated address, or NULL for an invalid gateway.
 */
const char *kc_kaiman_gateway_address(const kc_kaiman_gateway_t *gateway) {
    return gateway != NULL ? gateway->address : NULL;
}

/**
 * Borrow the most recent gateway error message.
 * @param gateway Open gateway, or NULL.
 * @return NUL terminated message, or NULL when none is set.
 */
const char *kc_kaiman_gateway_error(const kc_kaiman_gateway_t *gateway) {
    return gateway != NULL ? gateway->error : NULL;
}

/**
 * Read one saved process identifier.
 * @param pid Receives the identifier.
 * @return 1 when a valid identifier was read, 0 otherwise.
 */
static int kaiman_pid(unsigned long long *pid) {
    char *path = kc_util_pid_path();
    char *data;
    size_t size;
    char *end = NULL;
    unsigned long long value;

    if (path == NULL) return 0;
    data = kc_util_read_file(path, &size);
    free(path);
    if (data == NULL) return 0;
    value = strtoull(data, &end, 10);
    while (*end != '\0' && isspace((unsigned char)*end)) end++;
    if (end == data || *end != '\0' || value == 0ULL) {
        free(data);
        return 0;
    }
    free(data);
    *pid = value;
    return 1;
}

/**
 * Check whether one process identifier is running.
 * @param pid Process identifier.
 * @return 1 when running, 0 otherwise.
 */
static int kaiman_pid_alive(unsigned long long pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
        (DWORD)pid);
    DWORD code = 0U;
    int alive;

    if (process == NULL) return 0;
    alive = GetExitCodeProcess(process, &code) != 0 && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    if (pid > (unsigned long long)INT_MAX) return 0;
    return kill((pid_t)pid, 0) == 0;
#endif
}

/**
 * Store one process identifier in the private state directory.
 * @param pid Process identifier.
 * @return KC_KAIMAN_OK or an error status.
 */
static int kaiman_pid_save(unsigned long long pid) {
    char *path = kc_util_pid_path();
    char text[32];
    int result;

    if (path == NULL) return KC_KAIMAN_EIO;
    (void)snprintf(text, sizeof(text), "%llu\n", pid);
    result = kc_util_write_private(path, text, strlen(text))
        ? KC_KAIMAN_OK : KC_KAIMAN_EIO;
    free(path);
    return result;
}

/**
 * Remove the saved process identifier.
 * @return void
 */
static void kaiman_pid_clear(void) {
    char *path = kc_util_pid_path();
    if (path == NULL) return;
    remove(path);
    free(path);
}

/**
 * Start the gateway as an independent process.
 * @param options Process options containing the executable path.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_start(const kc_kaiman_process_options_t *options) {
    char *executable = NULL;
    char *quoted = NULL;
    char *command = NULL;
    unsigned long long pid = 0ULL;
    int status = KC_KAIMAN_EIO;

    if (kaiman_pid(&pid) && kaiman_pid_alive(pid)) {
        return KC_KAIMAN_EBUSY;
    }
    if (options != NULL && options->executable != NULL &&
        *options->executable != '\0') {
        executable = kc_util_strdup(options->executable);
    } else {
        executable = kc_util_executable_path();
    }
    if (executable == NULL) return KC_KAIMAN_EIO;
    quoted = kc_util_quote(executable);
    if (quoted == NULL) {
        free(executable);
        return KC_KAIMAN_ENOMEM;
    }
    {
        size_t length = strlen(quoted) + 32U;
        command = (char *)malloc(length);
        if (command == NULL) status = KC_KAIMAN_ENOMEM;
        else (void)snprintf(command, length, "%s --internal-serve",
            quoted);
    }
    if (command != NULL) {
#ifdef _WIN32
        STARTUPINFOA startup;
        PROCESS_INFORMATION process;
        memset(&startup, 0, sizeof(startup));
        memset(&process, 0, sizeof(process));
        startup.cb = sizeof(startup);
        if (CreateProcessA(executable, command, NULL, NULL, FALSE,
            DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, NULL, NULL,
            &startup, &process)) {
            status = kaiman_pid_save((unsigned long long)process.dwProcessId);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
        }
#else
        pid_t child = fork();
        if (child < 0) {
            status = KC_KAIMAN_EIO;
        } else if (child == 0) {
            int descriptor;
            (void)setsid();
            descriptor = open("/dev/null", O_RDWR);
            if (descriptor >= 0) {
                (void)dup2(descriptor, STDIN_FILENO);
                (void)dup2(descriptor, STDOUT_FILENO);
                (void)dup2(descriptor, STDERR_FILENO);
                if (descriptor > STDERR_FILENO) close(descriptor);
            }
            (void)execl(executable, executable, "--internal-serve",
                (char *)NULL);
            _exit(127);
        } else {
            status = kaiman_pid_save((unsigned long long)child);
        }
#endif
    }
    free(command);
    free(quoted);
    free(executable);
    return status;
}

/**
 * Stop the gateway process started by kc_kaiman_start().
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_stop(void) {
    unsigned long long pid = 0ULL;
    int index;

    if (!kaiman_pid(&pid)) return KC_KAIMAN_NOT_FOUND;
    if (kaiman_pid_alive(pid)) {
#ifdef _WIN32
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
        if (process == NULL) {
            kaiman_pid_clear();
            return KC_KAIMAN_NOT_FOUND;
        }
        (void)TerminateProcess(process, 0);
        CloseHandle(process);
#else
        if (pid > (unsigned long long)INT_MAX) return KC_KAIMAN_EINVAL;
        if (kill((pid_t)pid, SIGTERM) != 0) {
            kaiman_pid_clear();
            return KC_KAIMAN_NOT_FOUND;
        }
#endif
        for (index = 0U; index < 20 && kaiman_pid_alive(pid); index++) {
            kc_util_sleep(100);
        }
    }
    kaiman_pid_clear();
    return KC_KAIMAN_OK;
}

/**
 * Describe the gateway process and endpoint.
 * @param out Receives an owned description released with kc_kaiman_free().
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_status(char **out) {
    kc_kaiman_t *kaiman = NULL;
    kc_buffer_t *buffer = NULL;
    unsigned long long pid = 0ULL;
    int running;
    int status;

    if (out == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    status = kc_kaiman_open(&kaiman, NULL);
    if (status != KC_KAIMAN_OK) return status;
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) {
        kc_kaiman_close(kaiman);
        return status;
    }
    running = kaiman_pid(&pid) && kaiman_pid_alive(pid);
    status = kc_buffer_format(buffer, "%s\naddress: %s\n",
        running ? "running" : "stopped", kc_kaiman_address(kaiman));
    if (status == KC_KAIMAN_OK && running) {
        status = kc_buffer_format(buffer, "pid: %llu\n", pid);
    }
    if (status == KC_KAIMAN_OK) {
        *out = kc_util_strdup(kc_buffer_value(buffer));
        if (*out == NULL) status = KC_KAIMAN_ENOMEM;
    }
    kc_buffer_close(buffer);
    kc_kaiman_close(kaiman);
    return status;
}

/**
 * Release memory returned by kc_kaiman_status().
 * @param ptr Owned pointer, or NULL.
 * @return void
 */
void kc_kaiman_free(void *ptr) {
    free(ptr);
}
