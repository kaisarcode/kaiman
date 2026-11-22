/**
 * libkaiman.c - Unified AI service management for kaiman.
 * Summary: Owns managed services, gateway settings and their persistence.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#include "libkaiman.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define KAIMAN_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
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
    service->name = NULL;
    service->adapter = NULL;
    service->base_url = NULL;
    service->model = NULL;
    service->api_key_env = NULL;
    service->token_command = NULL;
    service->auth_scheme = NULL;
    service->auth_user = NULL;
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
        if (strcmp(value, "openai") != 0 && strcmp(value, "opencode") != 0) {
            set_error(kaiman, "adapter must be openai or opencode");
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

    if (kaiman == NULL || listen == NULL || *listen == '\0') {
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
    status = apply_field(service, key, value);
    if (status != KC_KAIMAN_OK) return status;
    return persist(service->owner);
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
    service_retire(service->owner, service);
    status = persist(service->owner);
    return status;
}
