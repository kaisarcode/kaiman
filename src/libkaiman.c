/**
 * libkaiman.c - Unified AI service management for kaiman.
 * Summary: Owns managed services, gateway settings and their persistence.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#include "libkaiman.h"

/* Windows defines interface as struct; load libnetl.h before windows.h. */
#include "libnetl.h"

#include "child.h"
#include "json.h"
#include "keyring.h"
#include "libhttp.h"
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
    char *display_name;
    char *adapter;
    char *base_url;
    char *api_key_env;
    char *token_command;
    char *auth_scheme;
    char *auth_user;
    char *keyring;
    const char *local_password;
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
    kc_child_t *codex;
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

typedef struct {
    const char *slug;
    const char *name;
    const char *url;
} kc_provider_t;

static const kc_provider_t providers[] = {
    {"openai", "OpenAI", "https://api.openai.com/v1"},
    {"opencode-local", "OpenCode Local", NULL},
    {"opencode", "OpenCode Server", "http://127.0.0.1:4096"},
    {"opencode-cloud", "OpenCode Cloud", "https://opencode.ai/inference"},
    {"codex-local", "Codex Local", NULL},
    {"anthropic", "Anthropic", "https://api.anthropic.com/v1"},
    {"gemini", "Gemini", "https://generativelanguage.googleapis.com/v1beta"},
    {"responses", "Responses", "https://api.openai.com/v1"},
    {"ollama", "Ollama", "http://127.0.0.1:11434"},
    {"bedrock", "Bedrock", "https://bedrock-runtime.us-east-1.amazonaws.com"}
};

/**
 * Find a supported provider by its adapter identifier.
 * @param adapter Adapter identifier.
 * @return Borrowed provider metadata, or NULL when unknown.
 */
static const kc_provider_t *provider_find(const char *adapter) {
    size_t index;

    if (adapter == NULL) return NULL;
    for (index = 0U; index < sizeof(providers) / sizeof(providers[0]); index++) {
        if (strcmp(providers[index].slug, adapter) == 0) {
            return &providers[index];
        }
    }
    return NULL;
}

/**
 * Return the default base URL for a supported adapter.
 * @param adapter Adapter identifier.
 * @return Borrowed default URL, or NULL when unavailable.
 */
const char *kc_kaiman_default_url(const char *adapter) {
    const kc_provider_t *provider = provider_find(adapter);

    return provider != NULL ? provider->url : NULL;
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
    free(service->display_name);
    free(service->adapter);
    free(service->base_url);
    free(service->api_key_env);
    free(service->token_command);
    free(service->auth_scheme);
    free(service->auth_user);
    free(service->keyring);
    service->name = NULL;
    service->display_name = NULL;
    service->adapter = NULL;
    service->base_url = NULL;
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
    service->auth_scheme = copy_text("none");
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
        fprintf(file, "display-name=%s\n",
            service->display_name != NULL ? service->display_name : "");
        fprintf(file, "adapter=%s\n",
            service->adapter != NULL ? service->adapter : "");
        fprintf(file, "url=%s\n",
            service->base_url != NULL ? service->base_url : "");
        fprintf(file, "env=%s\n",
            service->api_key_env != NULL ? service->api_key_env : "");
        fprintf(file, "token-command=%s\n",
            service->token_command != NULL ? service->token_command : "");
        fprintf(file, "scheme=%s\n",
            service->auth_scheme != NULL ? service->auth_scheme : "none");
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
    } else if (strcmp(key, "display-name") == 0 ||
        strcmp(key, "display_name") == 0) {
        target = &service->display_name;
    } else if (strcmp(key, "adapter") == 0) {
        if (provider_find(value) == NULL) {
            set_error(kaiman, "unknown adapter: %s", value);
            return KC_KAIMAN_EINVAL;
        }
        target = &service->adapter;
    } else if (strcmp(key, "url") == 0 || strcmp(key, "base_url") == 0) {
        if (*value != '\0' && !http_url(value)) {
            set_error(kaiman, "url must start with http:// or https://");
            return KC_KAIMAN_EINVAL;
        }
        target = &service->base_url;
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
    static const char *keys[] = {"adapter", "url", "env",
        "token-command", "scheme", "user"};
    const char *values[6];
    kc_kaiman_service_t *service;
    kc_kaiman_service_t **items;
    char timeout[32];
    size_t index;
    int status;

    if (kaiman == NULL || descriptor == NULL || out == NULL) {
        set_error(kaiman, "invalid service descriptor");
        return KC_KAIMAN_EINVAL;
    }
    if (!valid_name(descriptor->name)) {
        set_error(kaiman, "invalid service name: %s (use letters, digits, "
            "underscores, hyphens or dots)",
            descriptor->name != NULL ? descriptor->name : "");
        return KC_KAIMAN_EINVAL;
    }
    if (find_service(kaiman, descriptor->name) != NULL) {
        set_error(kaiman, "service already exists: %s", descriptor->name);
        return KC_KAIMAN_EINVAL;
    }
    service = service_new(kaiman, descriptor->name);
    if (service == NULL) {
        set_error(kaiman, "out of memory");
        return KC_KAIMAN_ENOMEM;
    }
    values[0] = descriptor->adapter;
    values[1] = descriptor->base_url;
    values[2] = descriptor->api_key_env;
    values[3] = descriptor->token_command;
    values[4] = descriptor->auth_scheme;
    values[5] = descriptor->auth_user;
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
    if (strcmp(key, "display-name") == 0 ||
        strcmp(key, "display_name") == 0) return service->display_name;
    if (strcmp(key, "adapter") == 0) return service->adapter;
    if (strcmp(key, "url") == 0 || strcmp(key, "base_url") == 0) {
        return service->base_url != NULL ? service->base_url : "";
    }
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
 * Resolve the effective base URL of a configured service.
 * @param service Managed service.
 * @return Borrowed explicit or default URL, or NULL when unavailable.
 */
const char *kc_kaiman_service_url(const kc_kaiman_service_t *service) {
    if (!service_valid(service)) return NULL;
    if (service->base_url != NULL && *service->base_url != '\0') {
        return service->base_url;
    }
    return kc_kaiman_default_url(service->adapter);
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
    int written = 0;
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
        if (stream >= 0 && strcmp(key, "session") == 0) continue;
        if (written++) status = kc_buffer_text(buffer, ",");
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
        status = kc_buffer_format(buffer, "%s\"stream\":%s",
            written ? "," : "", stream ? "true" : "false");
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

    if (service->local_password != NULL) {
        token = kc_util_strdup(service->local_password);
    } else if (service->token_command != NULL && *service->token_command != '\0') {
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
 * Resolve a namespaced model to its service and upstream identifier.
 * @param kaiman Open manager.
 * @param requested Client model identifier in service/model form.
 * @param out Receives a borrowed service.
 * @param model Receives the borrowed upstream model identifier.
 * @return KC_KAIMAN_OK, KC_KAIMAN_NOT_FOUND, or an error status.
 */
static int gateway_select(kc_kaiman_t *kaiman, const char *requested,
    kc_kaiman_service_t **out, const char **model) {
    const char *slash;
    size_t length;
    size_t index;

    if (kaiman == NULL || out == NULL || model == NULL) {
        return KC_KAIMAN_EINVAL;
    }
    *out = NULL;
    *model = NULL;
    if (requested == NULL) return KC_KAIMAN_NOT_FOUND;
    slash = strchr(requested, '/');
    if (slash == NULL || slash == requested || slash[1] == '\0') {
        return KC_KAIMAN_NOT_FOUND;
    }
    length = (size_t)(slash - requested);
    for (index = 0U; index < kaiman->count; index++) {
        kc_kaiman_service_t *service = kaiman->services[index];

        if (strlen(service->name) == length &&
            strncmp(service->name, requested, length) == 0) {
            *out = service;
            *model = slash + 1;
            return KC_KAIMAN_OK;
        }
    }
    return KC_KAIMAN_NOT_FOUND;
}

/**
 * Identify the session facilities supported by an adapter.
 * @param service Configured service.
 * @return 1 for OpenCode, 2 for Codex, 3 for Console, or zero.
 */
static int session_type(const kc_kaiman_service_t *service) {
    const char *adapter = service->adapter;

    if (adapter == NULL) return 0;
    if (strcmp(adapter, "opencode") == 0 ||
        strcmp(adapter, "opencode-local") == 0) return 1;
    if (strcmp(adapter, "codex-local") == 0) return 2;
    if (strcmp(adapter, "opencode-cloud") == 0) return 3;
    return 0;
}

/**
 * Validate an opaque session identifier for use in paths and headers.
 * @param id Candidate identifier.
 * @return 1 when valid, otherwise zero.
 */
static int session_valid(const char *id) {
    return id != NULL && *id != '\0' && strlen(id) < 256U &&
        strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789_-") == strlen(id);
}

/**
 * Resolve the service prefix of an explicit session.
 * @param service Selected service.
 * @param value Namespaced session identifier.
 * @return Borrowed upstream identifier, or NULL for a mismatch.
 */
static const char *session_raw(const kc_kaiman_service_t *service,
    const char *value) {
    const char *slash;

    if (value == NULL) return NULL;
    slash = strchr(value, '/');
    if (slash == NULL || (size_t)(slash - value) != strlen(service->name) ||
        strncmp(value, service->name, (size_t)(slash - value)) != 0 ||
        !session_valid(slash + 1)) return NULL;
    return slash + 1;
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
 * @param service Selected service.
 * @param model Upstream model identifier.
 * @return KC_KAIMAN_OK or an error status.
 */
static int openai_chat(kc_kaiman_gateway_t *gateway, kc_netl_peer_t *peer,
    const kc_json_value_t *root, const kc_kaiman_service_t *service,
    const char *model) {
    const char *requested = kc_json_text(kc_json_member(root, "model"));
    const char *session = session_raw(service,
        kc_json_text(kc_json_member(root, "session")));
    kc_upstream_field_t fields[2];
    size_t field_count = 0U;
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

    stream_mode = kc_json_boolean(kc_json_member(root, "stream"));
    status = service_authorization(service, &authorization);
    if (status != KC_KAIMAN_OK) {
        return gateway_failure(gateway, peer, 400,
            "the service credential is unavailable");
    }
    status = json_rewrite(root, model, NULL, stream_mode, &body);
    if (status != KC_KAIMAN_OK) {
        free(authorization);
        return gateway_failure(gateway, peer, 400, "invalid chat request");
    }
    url = api_url(kc_kaiman_service_url(service),
        strcmp(service->adapter, "opencode-cloud") == 0 ?
            "/openai/v1/chat/completions" : "/chat/completions");
    if (url == NULL) {
        free(authorization);
        free(body);
        return gateway_failure(gateway, peer, 400, "invalid service URL");
    }
    memset(fields, 0, sizeof(fields));
    if (authorization != NULL) {
        fields[field_count].name = "Authorization";
        fields[field_count++].value = authorization;
    }
    if (strcmp(service->adapter, "opencode-cloud") == 0) {
        if (session != NULL) {
            fields[field_count].name = "x-opencode-session";
            fields[field_count++].value = session;
        }
    }
    memset(&request, 0, sizeof(request));
    request.method = "POST";
    request.url = url;
    request.headers = fields;
    request.header_count = field_count;
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

    const char *url = kc_kaiman_service_url(service);

    if (url == NULL) return NULL;
    for (cursor = url; *cursor != '\0'; cursor++) {
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
 * @param model Requested upstream model.
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
    url = api_url(kc_kaiman_service_url(service), path);
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
 * @param state_path Persistent state path, or NULL for a temporary session.
 * @param model Upstream model identifier.
 * @param session Receives owned session identifier.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_ensure_session(const kc_kaiman_service_t *service,
    const char *state_path, const char *model, char **session) {
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
    status = KC_KAIMAN_OK;
    if (state_path != NULL) {
        status = opencode_state_load(state_path, &id, messages, &count);
        if (status != KC_KAIMAN_OK) return status;
    }

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

    status = opencode_split_model(model, &provider, &identifier);
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
    if (status == KC_KAIMAN_OK && state_path != NULL) {
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
 * Append a percent-encoded pagination cursor.
 * @param buffer Destination URL.
 * @param cursor Provider cursor.
 * @return KC_KAIMAN_OK or an error status.
 */
static int catalog_cursor(kc_buffer_t *buffer, const char *cursor) {
    const unsigned char *byte = (const unsigned char *)cursor;
    int status = KC_KAIMAN_OK;

    for (; *byte != '\0' && status == KC_KAIMAN_OK; byte++) {
        if ((*byte >= 'a' && *byte <= 'z') ||
            (*byte >= 'A' && *byte <= 'Z') ||
            (*byte >= '0' && *byte <= '9') ||
            *byte == '-' || *byte == '_' || *byte == '.' || *byte == '~') {
            status = kc_buffer_append(buffer, byte, 1U);
        } else {
            status = kc_buffer_format(buffer, "%%%02X", (unsigned int)*byte);
        }
    }
    return status;
}

/**
 * Fetch a page of session messages in reverse chronological order.
 * @param service Managed OpenCode service.
 * @param session Native session identifier.
 * @param cursor Optional pagination cursor.
 * @param out Receives an owned response object.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_message_page(const kc_kaiman_service_t *service,
    const char *session, const char *cursor, kc_json_value_t **out) {
    kc_buffer_t *path = NULL;
    kc_upstream_response_t response;
    int status;

    *out = NULL;
    memset(&response, 0, sizeof(response));
    status = kc_buffer_open(&path);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(path, "/api/session/%s/message", session);
    }
    if (status == KC_KAIMAN_OK && cursor == NULL) {
        status = kc_buffer_text(path, "?order=desc&limit=100");
    } else if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(path, "?cursor=");
        if (status == KC_KAIMAN_OK) status = catalog_cursor(path, cursor);
    }
    if (status == KC_KAIMAN_OK) {
        status = opencode_request(service, "GET", kc_buffer_value(path),
            NULL, &response);
    }
    if (status == KC_KAIMAN_OK && response.status != 200) {
        status = KC_KAIMAN_EHTTP;
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_json_parse(response.body, response.body_size, out);
    }
    kc_upstream_response_free(&response);
    kc_buffer_close(path);
    return status;
}

/**
 * Record the newest existing event before submitting a prompt.
 * @param service Managed OpenCode service.
 * @param session Native session identifier.
 * @param anchor Destination for the newest message identifier.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_snapshot(const kc_kaiman_service_t *service,
    const char *session, kc_buffer_t *anchor) {
    kc_json_value_t *root = NULL;
    const kc_json_value_t *items;
    const char *id;
    int status;

    status = opencode_message_page(service, session, NULL, &root);
    items = kc_json_member(root, "data");
    if (status == KC_KAIMAN_OK && kc_json_type(items) != KC_JSON_ARRAY) {
        status = KC_KAIMAN_EFORMAT;
    }
    if (status == KC_KAIMAN_OK && kc_json_length(items) != 0U) {
        id = kc_json_text(kc_json_member(kc_json_item(items, 0U), "id"));
        status = id != NULL && *id != '\0' ?
            kc_buffer_text(anchor, id) : KC_KAIMAN_EFORMAT;
    }
    kc_json_free(root);
    return status;
}

/**
 * Read only events newer than the saved session anchor.
 * @param service Managed OpenCode service.
 * @param session Native session identifier.
 * @param anchor Identifier of the last preexisting event, or empty.
 * @param text Destination for chronological assistant text.
 * @param done Receives whether the turn completed.
 * @param error Receives an owned upstream error message.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_collect(const kc_kaiman_service_t *service,
    const char *session, const char *anchor, kc_buffer_t *text, int *done,
    char **error) {
    kc_buffer_t *part = NULL;
    char *cursor = NULL;
    unsigned int page;
    int found = 0;
    int status;

    *done = 0;
    text->size = 0U;
    if (text->data != NULL) text->data[0] = '\0';
    status = kc_buffer_open(&part);
    for (page = 0U; status == KC_KAIMAN_OK && page < 100U; page++) {
        kc_json_value_t *root = NULL;
        const kc_json_value_t *items;
        const char *next;
        char *following = NULL;
        size_t index;
        size_t boundary;

        status = opencode_message_page(service, session, cursor, &root);
        items = kc_json_member(root, "data");
        if (status == KC_KAIMAN_OK && kc_json_type(items) != KC_JSON_ARRAY) {
            status = KC_KAIMAN_EFORMAT;
        }
        boundary = kc_json_length(items);
        for (index = 0U; status == KC_KAIMAN_OK && index < boundary; index++) {
            const kc_json_value_t *item = kc_json_item(items, index);
            const char *id = kc_json_text(kc_json_member(item, "id"));
            const char *kind = kc_json_text(kc_json_member(item, "type"));

            if (id == NULL) {
                status = KC_KAIMAN_EFORMAT;
                break;
            }
            if (*anchor != '\0' && strcmp(id, anchor) == 0) {
                boundary = index;
                found = 1;
                break;
            }
            if (!*done && kind != NULL && strcmp(kind, "idle") == 0) {
                const char *outcome = kc_json_text(kc_json_member(item,
                    "outcome"));

                if (outcome != NULL && strcmp(outcome, "succeeded") == 0) {
                    *done = 1;
                } else {
                    const char *message = kc_json_text(kc_json_member(
                        kc_json_member(item, "error"), "message"));

                    *error = kc_util_strdup(message != NULL ? message :
                        "opencode did not complete");
                    status = KC_KAIMAN_EHTTP;
                }
            }
        }
        part->size = 0U;
        if (part->data != NULL) part->data[0] = '\0';
        for (index = boundary; status == KC_KAIMAN_OK && index > 0U; index--) {
            const kc_json_value_t *item = kc_json_item(items, index - 1U);
            const char *kind = kc_json_text(kc_json_member(item, "type"));

            if (kind == NULL) kind = kc_json_text(kc_json_member(item, "role"));
            if (kind != NULL && strcmp(kind, "assistant") == 0) {
                status = opencode_message_text(item, part);
            }
        }
        if (status == KC_KAIMAN_OK) {
            const char *previous = kc_buffer_value(text);

            if (previous != NULL) status = kc_buffer_text(part, previous);
        }
        if (status == KC_KAIMAN_OK) {
            const char *combined = kc_buffer_value(part);

            text->size = 0U;
            if (text->data != NULL) text->data[0] = '\0';
            if (combined != NULL) status = kc_buffer_text(text, combined);
        }
        next = kc_json_text(kc_json_member(kc_json_member(root, "cursor"),
            "next"));
        if (status == KC_KAIMAN_OK && !found && next != NULL &&
            *next != '\0') {
            if (cursor != NULL && strcmp(cursor, next) == 0) {
                status = KC_KAIMAN_EFORMAT;
            } else {
                following = kc_util_strdup(next);
                if (following == NULL) status = KC_KAIMAN_ENOMEM;
            }
        }
        kc_json_free(root);
        free(cursor);
        cursor = following;
        if (status != KC_KAIMAN_OK || cursor == NULL) break;
    }
    if (status == KC_KAIMAN_OK &&
        (page == 100U || (*anchor != '\0' && !found))) {
        status = KC_KAIMAN_EFORMAT;
    }
    free(cursor);
    kc_buffer_close(part);
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
 * @param model Upstream model identifier.
 * @param body Receives owned native JSON.
 * @return KC_KAIMAN_OK or an error status.
 */
static int native_body(const kc_json_value_t *root,
    const kc_kaiman_service_t *service, const char *model, char **body) {
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
    if (model == NULL || *model == '\0') {
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
            status = json_write_text(buffer, model);
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
 * @param model Upstream model identifier.
 * @return Owned URL, or NULL on failure.
 */
static char *native_url(const kc_kaiman_service_t *service, const char *model) {
    const char *adapter = service->adapter;
    const unsigned char *cursor;
    kc_buffer_t *path = NULL;
    char *url = NULL;
    int gemini = strcmp(adapter, "gemini") == 0;
    int status;

    if (strcmp(adapter, "anthropic") == 0) {
        return api_url(kc_kaiman_service_url(service), "/messages");
    }
    if (strcmp(adapter, "responses") == 0) {
        return api_url(kc_kaiman_service_url(service), "/responses");
    }
    if (strcmp(adapter, "ollama") == 0) {
        return api_url(kc_kaiman_service_url(service), "/api/chat");
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
        url = api_url(kc_kaiman_service_url(service), kc_buffer_value(path));
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
 * Build provider-specific authentication headers.
 * @param service Managed service.
 * @param authorization Borrowed authorization header, or NULL.
 * @param fields Destination array with room for two headers.
 * @return Number of headers.
 */
static size_t provider_headers(const kc_kaiman_service_t *service,
    const char *authorization, kc_upstream_field_t *fields) {
    size_t count = 0U;

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
    return count;
}

/**
 * Forward a text chat through a native provider API.
 * @param gateway Open gateway.
 * @param peer Borrowed client peer.
 * @param root Borrowed Chat Completions request.
 * @param service Selected service.
 * @param upstream_model Upstream model identifier.
 * @return KC_KAIMAN_OK or an error status.
 */
static int native_chat(kc_kaiman_gateway_t *gateway, kc_netl_peer_t *peer,
    const kc_json_value_t *root, const kc_kaiman_service_t *service,
    const char *upstream_model) {
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
    status = native_body(root, service, upstream_model, &body);
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
    url = native_url(service, upstream_model);
    if (url == NULL) {
        status = gateway_failure(gateway, peer, 400, "invalid service URL");
        goto done;
    }
    count = provider_headers(service, authorization, fields);
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
 * Determine whether the configured OpenCode server is on loopback.
 * @param url Configured server URL.
 * @return 1 for a loopback host, otherwise 0.
 */
static int opencode_local(const char *url) {
    const char *host;
    const char *end;
    size_t length;

    if (url == NULL) return 0;
    host = strstr(url, "://");
    if (host == NULL) return 0;
    host += 3;
    end = host + strcspn(host, "/?#");
    if (host == end) return 0;
    if (memchr(host, '@', (size_t)(end - host)) != NULL) return 0;
    if (host[0] == '[') {
        return (size_t)(end - host) >= 5U &&
            strncmp(host, "[::1]", 5U) == 0 &&
            (host + 5U == end || host[5] == ':');
    }
    length = strcspn(host, ":/?#");
    return (length == 9U && strncmp(host, "localhost", length) == 0) ||
        (length == 9U && strncmp(host, "127.0.0.1", length) == 0);
}

/**
 * Capture a single line from a local provider command.
 * @param argv Executable and fixed arguments.
 * @param timeout Maximum wait in seconds.
 * @param out Receives owned command output.
 * @return KC_KAIMAN_OK or an error status.
 */
static int local_capture(char *const argv[], int timeout, char **out) {
    kc_child_t *child = NULL;
    double deadline = kc_util_seconds() + (double)timeout;
    int status;

    *out = NULL;
    status = kc_child_open(&child, argv);
    if (status == KC_KAIMAN_OK) {
        status = kc_child_line(child, deadline, out);
        if (status == KC_KAIMAN_NOT_FOUND) {
            *out = kc_util_strdup("");
            status = *out != NULL ? KC_KAIMAN_OK : KC_KAIMAN_ENOMEM;
        }
    }
    if (status == KC_KAIMAN_OK) status = kc_child_wait(child, deadline);
    kc_child_close(child);
    if (status != KC_KAIMAN_OK) {
        free(*out);
        *out = NULL;
    }
    return status;
}

/**
 * Resolve OpenCode's local service into a temporary HTTP service descriptor.
 * @param service Configured automatic service.
 * @param resolved Receives a borrowed-field copy.
 * @param url Receives the owned discovered URL.
 * @param password Receives the owned server password.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_resolve(const kc_kaiman_service_t *service,
    kc_kaiman_service_t *resolved, char **url, char **password) {
    char *status_args[] = {"opencode", "service", "status", NULL};
    char *password_args[] = {"opencode", "service", "get", "password", NULL};
    int timeout = kc_kaiman_service_timeout(service);
    int status;

    *url = NULL;
    *password = NULL;
    status = local_capture(status_args, timeout, url);
    if (status == KC_KAIMAN_OK &&
        (!http_url(*url) || !opencode_local(*url))) status = KC_KAIMAN_EFORMAT;
    if (status == KC_KAIMAN_OK) {
        status = local_capture(password_args, timeout, password);
    }
    if (status != KC_KAIMAN_OK) return status;
    *resolved = *service;
    resolved->adapter = "opencode";
    resolved->base_url = *url;
    resolved->auth_scheme = **password != '\0' ? "basic" : "none";
    resolved->auth_user = "opencode";
    resolved->local_password = *password;
    resolved->api_key_env = NULL;
    resolved->token_command = NULL;
    resolved->keyring = NULL;
    return KC_KAIMAN_OK;
}

/**
 * Switch a persistent OpenCode session to the requested model.
 * @param service Managed service.
 * @param session Session identifier.
 * @param model Requested provider/model identifier.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_switch(const kc_kaiman_service_t *service,
    const char *session, const char *model) {
    kc_buffer_t *body = NULL;
    kc_buffer_t *path = NULL;
    kc_upstream_response_t response;
    char *provider = NULL;
    char *identifier = NULL;
    int status;

    status = opencode_split_model(model, &provider, &identifier);
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&body);
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&path);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(path, "/api/session/%s/model", session);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(body, "{\"model\":{\"providerID\":");
    }
    if (status == KC_KAIMAN_OK) status = json_write_text(body, provider);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(body, ",\"id\":");
    if (status == KC_KAIMAN_OK) status = json_write_text(body, identifier);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(body, "}}");
    if (status == KC_KAIMAN_OK) {
        status = opencode_request(service, "POST", kc_buffer_value(path),
            kc_buffer_value(body), &response);
        if (status == KC_KAIMAN_OK && response.status >= 400) {
            status = KC_KAIMAN_EHTTP;
        }
        kc_upstream_response_free(&response);
    }
    free(provider);
    free(identifier);
    kc_buffer_close(path);
    kc_buffer_close(body);
    return status;
}

/**
 * Generate text on a remote OpenCode server using a temporary session.
 * @param service Managed service.
 * @param model Requested provider/model identifier.
 * @param prompt Text prompt.
 * @param reply Destination text buffer.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_remote(const kc_kaiman_service_t *service,
    const char *model, const char *prompt, kc_buffer_t *reply) {
    kc_buffer_t *body = NULL;
    kc_buffer_t *path = NULL;
    kc_json_value_t *root = NULL;
    kc_upstream_response_t response;
    char *session = NULL;
    const char *text;
    int status;

    status = kc_buffer_open(&body);
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&path);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(body, "{\"prompt\":");
    }
    if (status == KC_KAIMAN_OK) status = json_write_text(body, prompt);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(body, "}");
    if (status == KC_KAIMAN_OK) {
        status = opencode_ensure_session(service, NULL, model, &session);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(path, "/api/session/%s/generate", session);
    }
    if (status == KC_KAIMAN_OK) {
        status = opencode_request(service, "POST", kc_buffer_value(path),
            kc_buffer_value(body), &response);
        if (status == KC_KAIMAN_OK && response.status >= 400) {
            status = KC_KAIMAN_EHTTP;
        }
        if (status == KC_KAIMAN_OK) {
            status = kc_json_parse(response.body, response.body_size, &root);
        }
        if (status == KC_KAIMAN_OK) {
            text = kc_json_text(kc_json_member(kc_json_member(root, "data"),
                "text"));
            status = text != NULL && *text != '\0' ?
                kc_buffer_text(reply, text) : KC_KAIMAN_EFORMAT;
        }
        kc_upstream_response_free(&response);
    }
    if (session != NULL && path != NULL) {
        path->size = 0U;
        if (path->data != NULL) path->data[0] = '\0';
        if (kc_buffer_format(path, "/api/session/%s", session) == KC_KAIMAN_OK) {
            int removed = opencode_request(service, "DELETE",
                kc_buffer_value(path), NULL, &response);

            if (removed == KC_KAIMAN_OK && response.status >= 400) {
                removed = KC_KAIMAN_EHTTP;
            }
            if (status == KC_KAIMAN_OK) status = removed;
            kc_upstream_response_free(&response);
        }
    }
    kc_json_free(root);
    kc_buffer_close(body);
    kc_buffer_close(path);
    free(session);
    return status;
}

/**
 * Read the OpenCode server's active session map.
 * @param service Managed HTTP service.
 * @param out Receives owned JSON map.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_active(const kc_kaiman_service_t *service,
    kc_json_value_t **out) {
    kc_upstream_response_t response;
    kc_json_value_t *root = NULL;
    const kc_json_value_t *data;
    char *json = NULL;
    int status;

    *out = NULL;
    status = opencode_request(service, "GET", "/api/session/active", NULL,
        &response);
    if (status == KC_KAIMAN_OK && response.status != 200) {
        status = KC_KAIMAN_EHTTP;
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_json_parse(response.body, response.body_size, &root);
    }
    if (status == KC_KAIMAN_OK) {
        data = kc_json_member(root, "data");
        if (kc_json_type(data) != KC_JSON_OBJECT) {
            status = KC_KAIMAN_EFORMAT;
        } else {
            status = kc_json_stringify(data, &json);
            if (status == KC_KAIMAN_OK) {
                status = kc_json_parse(json, strlen(json), out);
            }
        }
    }
    free(json);
    kc_json_free(root);
    kc_upstream_response_free(&response);
    return status;
}

/**
 * Check whether a session is available without interrupting another client.
 * @param service Managed HTTP service.
 * @param session Upstream session identifier.
 * @return KC_KAIMAN_OK, KC_KAIMAN_EBUSY, or an error status.
 */
static int opencode_idle(const kc_kaiman_service_t *service,
    const char *session) {
    kc_json_value_t *active = NULL;
    int status = opencode_active(service, &active);

    if (status == KC_KAIMAN_OK && kc_json_member(active, session) != NULL) {
        status = KC_KAIMAN_EBUSY;
    }
    kc_json_free(active);
    return status;
}

/**
 * Forward one chat request through an OpenCode server.
 * @param gateway Open gateway.
 * @param peer Borrowed client peer.
 * @param root Borrowed chat request object.
 * @param service Selected service.
 * @param model Upstream provider/model identifier.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_chat(kc_kaiman_gateway_t *gateway, kc_netl_peer_t *peer,
    const kc_json_value_t *root, const kc_kaiman_service_t *service,
    const char *model) {
    const char *requested = kc_json_text(kc_json_member(root, "model"));
    const char *selected = session_raw(service,
        kc_json_text(kc_json_member(root, "session")));
    kc_upstream_response_t response;
    kc_kaiman_message_t messages[KAIMAN_MAX_HISTORY];
    kc_kaiman_stream_t stream;
    kc_buffer_t *prompt = NULL;
    kc_buffer_t *anchor = NULL;
    kc_buffer_t *reply = NULL;
    char *state_path = NULL;
    char *session = NULL;
    char *state_id = NULL;
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

    if (model == NULL || strchr(model, '/') == NULL) {
        return gateway_failure(gateway, peer, 400,
            "opencode model must be providerID/modelID");
    }
    stream_mode = kc_json_boolean(kc_json_member(root, "stream"));
    memset(messages, 0, sizeof(messages));
    memset(&stream, 0, sizeof(stream));
    kc_mutex_lock(&gateway->mutex);
    if (selected == NULL && !opencode_local(kc_kaiman_service_url(service))) {
        status = request_prompt(root, &text);
        if (status == KC_KAIMAN_OK) status = kc_buffer_open(&reply);
        if (status == KC_KAIMAN_OK) {
            status = opencode_remote(service, model, text, reply);
        }
        free(text);
        text = NULL;
        goto respond;
    }
    status = KC_KAIMAN_OK;
    if (selected != NULL) {
        session = kc_util_strdup(selected);
        status = session != NULL ? opencode_idle(service, session) :
            KC_KAIMAN_ENOMEM;
    } else {
        state_path = opencode_session_path(service);
        if (state_path == NULL) status = KC_KAIMAN_ENOMEM;
        if (status == KC_KAIMAN_OK) {
            status = opencode_ensure_session(service, state_path, model,
                &session);
        }
    }
    if (status == KC_KAIMAN_OK) {
        status = opencode_switch(service, session, model);
    }
    if (status == KC_KAIMAN_OK) {
        status = request_prompt(root, &text);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_open(&prompt);
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(prompt, text);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_open(&anchor);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_open(&reply);
    }
    if (status == KC_KAIMAN_OK) {
        status = opencode_snapshot(service, session, anchor);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_open(&stream.sse);
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
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "}");
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
                status = response.status == 409 ? KC_KAIMAN_EBUSY :
                    KC_KAIMAN_EHTTP;
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

        while (!done && kc_util_seconds() < deadline) {
            status = opencode_collect(service, session,
                kc_buffer_value(anchor), reply, &done, &failure);
            if (status != KC_KAIMAN_OK || done) break;
            kc_util_sleep(200);
        }
        if (status == KC_KAIMAN_OK && !done) {
            status = KC_KAIMAN_ETIMEOUT;
        }
    }
    if (state_path == NULL) goto respond;
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
respond:
    free(text);
    text = NULL;
    kc_mutex_unlock(&gateway->mutex);

    if (status != KC_KAIMAN_OK) {
        gateway_failure(gateway, peer, status == KC_KAIMAN_EBUSY ? 409 : 502,
            status == KC_KAIMAN_EBUSY ? "session is active" :
                (failure != NULL ? failure : kc_kaiman_strerror(status)));
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
    kc_buffer_close(reply);
    kc_buffer_close(anchor);
    kc_buffer_close(prompt);
    for (index = 0U; index < KAIMAN_MAX_HISTORY; index++) {
        free(messages[index].role);
        free(messages[index].content);
    }
    free(body);
    free(session);
    free(state_id);
    free(state_path);
    return KC_KAIMAN_OK;
}

/**
 * Route a request through the automatically discovered local OpenCode service.
 * @param gateway Open gateway.
 * @param peer Borrowed client peer.
 * @param root Borrowed Chat Completions request.
 * @param service Configured local service.
 * @param model Requested upstream model.
 * @return KC_KAIMAN_OK or an error status.
 */
static int opencode_auto_chat(kc_kaiman_gateway_t *gateway, kc_netl_peer_t *peer,
    const kc_json_value_t *root, const kc_kaiman_service_t *service,
    const char *model) {
    kc_kaiman_service_t resolved;
    char *url = NULL;
    char *password = NULL;
    int status;

    status = opencode_resolve(service, &resolved, &url, &password);
    if (status == KC_KAIMAN_OK) {
        status = opencode_chat(gateway, peer, root, &resolved, model);
    } else {
        status = gateway_failure(gateway, peer, 502,
            "could not discover local OpenCode service");
    }
    free(url);
    free(password);
    return status;
}

/**
 * Read one JSON-RPC message from Codex without granting tool approvals.
 * @param child Running app-server process.
 * @param deadline Monotonic deadline.
 * @param out Receives an owned message.
 * @return KC_KAIMAN_OK or an error status.
 */
static int codex_read(kc_child_t *child, double deadline, kc_json_value_t **out) {
    char *line = NULL;
    int status;

    *out = NULL;
    status = kc_child_line(child, deadline, &line);
    if (status == KC_KAIMAN_NOT_FOUND) status = KC_KAIMAN_EIO;
    if (status == KC_KAIMAN_OK) {
        status = kc_json_parse(line, strlen(line), out);
    }
    free(line);
    if (status == KC_KAIMAN_OK &&
        (kc_json_type(*out) != KC_JSON_OBJECT ||
            (kc_json_member(*out, "method") != NULL &&
                kc_json_member(*out, "id") != NULL))) {
        status = KC_KAIMAN_EPROTO;
    }
    if (status != KC_KAIMAN_OK) {
        kc_json_free(*out);
        *out = NULL;
    }
    return status;
}

/**
 * Send a JSON-RPC request and receive the matching response.
 * @param child Running app-server process.
 * @param id Numeric request identifier.
 * @param method RPC method name.
 * @param params Serialized JSON parameter object.
 * @param deadline Monotonic deadline.
 * @param out Receives an owned response object.
 * @return KC_KAIMAN_OK or an error status.
 */
static int codex_rpc(kc_child_t *child, unsigned int id, const char *method,
    const char *params, double deadline, kc_json_value_t **out) {
    kc_buffer_t *body = NULL;
    int status;

    *out = NULL;
    status = kc_buffer_open(&body);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(body,
            "{\"id\":%u,\"method\":\"%s\",\"params\":%s}", id, method, params);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_child_write(child, kc_buffer_value(body), deadline);
    }
    kc_buffer_close(body);
    while (status == KC_KAIMAN_OK && kc_util_seconds() < deadline) {
        kc_json_value_t *message = NULL;

        status = codex_read(child, deadline, &message);
        if (status != KC_KAIMAN_OK) break;
        if (kc_json_number(kc_json_member(message, "id")) == (double)id) {
            if (kc_json_member(message, "result") == NULL ||
                kc_json_member(message, "error") != NULL) {
                kc_json_free(message);
                return KC_KAIMAN_EPROTO;
            }
            *out = message;
            return KC_KAIMAN_OK;
        }
        kc_json_free(message);
    }
    return status == KC_KAIMAN_OK ? KC_KAIMAN_ETIMEOUT : status;
}

/**
 * Initialize Codex and check the authentication it already manages.
 * @param out Receives an owned initialized process.
 * @param deadline Monotonic deadline.
 * @return KC_KAIMAN_OK or an error status.
 */
static int codex_open(kc_child_t **out, double deadline) {
    char *argv[] = {"codex", "app-server", NULL};
    kc_json_value_t *response = NULL;
    const kc_json_value_t *result;
    int status;

    status = kc_child_open(out, argv);
    if (status == KC_KAIMAN_OK) {
        status = codex_rpc(*out, 1U, "initialize",
            "{\"clientInfo\":{\"name\":\"kaiman\",\"title\":\"Kaiman\","
            "\"version\":\"" KC_KAIMAN_VERSION "\"}}", deadline, &response);
    }
    kc_json_free(response);
    response = NULL;
    if (status == KC_KAIMAN_OK) {
        status = kc_child_write(*out, "{\"method\":\"initialized\"}", deadline);
    }
    if (status == KC_KAIMAN_OK) {
        status = codex_rpc(*out, 2U, "account/read",
            "{\"refreshToken\":false}", deadline, &response);
    }
    if (status == KC_KAIMAN_OK) {
        result = kc_json_member(response, "result");
        if (kc_json_boolean(kc_json_member(result, "requiresOpenaiAuth")) &&
            kc_json_type(kc_json_member(result, "account")) == KC_JSON_NULL) {
            status = KC_KAIMAN_EPROTO;
        }
    }
    kc_json_free(response);
    if (status != KC_KAIMAN_OK) {
        kc_child_close(*out);
        *out = NULL;
    }
    return status;
}

/**
 * Obtain the app-server owned by the gateway for persistent sessions.
 * @param gateway Open gateway with its mutex held.
 * @param deadline Monotonic deadline.
 * @return KC_KAIMAN_OK or an error status.
 */
static int codex_connect(kc_kaiman_gateway_t *gateway, double deadline) {
    if (gateway->codex != NULL) return KC_KAIMAN_OK;
    return codex_open(&gateway->codex, deadline);
}

/**
 * Release the gateway's app-server after a transport error.
 * @param gateway Open gateway with its mutex held.
 * @return void
 */
static void codex_disconnect(kc_kaiman_gateway_t *gateway) {
    kc_child_close(gateway->codex);
    gateway->codex = NULL;
}

/**
 * Convert a text-only chat request to the prompt consumed by Codex.
 * @param root Borrowed Chat Completions request.
 * @param prompt Destination prompt buffer.
 * @return KC_KAIMAN_OK or an error status.
 */
static int codex_prompt(const kc_json_value_t *root, kc_buffer_t *prompt) {
    const kc_json_value_t *messages = kc_json_member(root, "messages");
    const kc_json_value_t *n = kc_json_member(root, "n");
    size_t index;
    int status = KC_KAIMAN_OK;

    if (kc_json_type(messages) != KC_JSON_ARRAY ||
        kc_json_length(messages) == 0U ||
        kc_json_member(root, "tools") != NULL ||
        kc_json_member(root, "functions") != NULL ||
        kc_json_member(root, "tool_choice") != NULL ||
        kc_json_member(root, "response_format") != NULL ||
        (n != NULL && kc_json_number(n) != 1.0)) return KC_KAIMAN_EINVAL;
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < kc_json_length(messages); index++) {
        const kc_json_value_t *message = kc_json_item(messages, index);
        const char *role = kc_json_text(kc_json_member(message, "role"));

        if (role == NULL || (strcmp(role, "system") != 0 &&
            strcmp(role, "developer") != 0 && strcmp(role, "user") != 0 &&
            strcmp(role, "assistant") != 0)) return KC_KAIMAN_EINVAL;
        status = kc_buffer_format(prompt, "%s: ", role);
        if (status == KC_KAIMAN_OK) {
            status = native_content(kc_json_member(message, "content"), prompt);
        }
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(prompt, "\n\n");
    }
    return status;
}

/**
 * Read the completed assistant text for a Codex turn.
 * @param child Running app-server process.
 * @param thread Thread identifier.
 * @param turn Turn identifier.
 * @param deadline Monotonic deadline.
 * @param reply Destination text buffer.
 * @return KC_KAIMAN_OK or an error status.
 */
static int codex_reply(kc_child_t *child, const char *thread, const char *turn,
    double deadline, kc_buffer_t *reply) {
    int status = KC_KAIMAN_OK;

    while (status == KC_KAIMAN_OK && kc_util_seconds() < deadline) {
        kc_json_value_t *event = NULL;
        const kc_json_value_t *params;
        const kc_json_value_t *item;
        const kc_json_value_t *completed;
        const char *method;
        const char *text;

        status = codex_read(child, deadline, &event);
        if (status != KC_KAIMAN_OK) break;
        params = kc_json_member(event, "params");
        method = kc_json_text(kc_json_member(event, "method"));
        if (method == NULL ||
            !native_equals(kc_json_member(params, "threadId"), thread)) {
            kc_json_free(event);
            continue;
        }
        if (strcmp(method, "item/completed") == 0 &&
            native_equals(kc_json_member(params, "turnId"), turn)) {
            item = kc_json_member(params, "item");
            if (native_equals(kc_json_member(item, "type"), "agentMessage")) {
                text = kc_json_text(kc_json_member(item, "text"));
                if (text != NULL) {
                    reply->size = 0U;
                    if (reply->data != NULL) reply->data[0] = '\0';
                    status = kc_buffer_text(reply, text);
                }
            }
        } else if (strcmp(method, "turn/completed") == 0) {
            completed = kc_json_member(params, "turn");
            if (native_equals(kc_json_member(completed, "id"), turn)) {
                status = native_equals(kc_json_member(completed, "status"),
                    "completed") && reply->size != 0U ?
                    KC_KAIMAN_OK : KC_KAIMAN_EPROTO;
                kc_json_free(event);
                return status;
            }
        }
        kc_json_free(event);
    }
    return status == KC_KAIMAN_OK ? KC_KAIMAN_ETIMEOUT : status;
}

/**
 * Run one Codex turn in a new or explicitly selected thread.
 * @param gateway Open gateway.
 * @param service Configured local Codex service.
 * @param model Requested model.
 * @param session Upstream thread identifier, or NULL for an ephemeral turn.
 * @param prompt Text prompt.
 * @param reply Destination reply.
 * @return KC_KAIMAN_OK or an error status.
 */
static int codex_generate(kc_kaiman_gateway_t *gateway,
    const kc_kaiman_service_t *service, const char *model,
    const char *session, const char *prompt, kc_buffer_t *reply) {
    kc_child_t *child = NULL;
    kc_buffer_t *params = NULL;
    kc_json_value_t *response = NULL;
    char *thread = NULL;
    char *turn = NULL;
    const char *id;
    double deadline = kc_util_seconds() +
        (double)kc_kaiman_service_timeout(service);
    int status;

    if (session != NULL) {
        kc_mutex_lock(&gateway->mutex);
        status = codex_connect(gateway, deadline);
        child = gateway->codex;
    } else {
        status = codex_open(&child, deadline);
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&params);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(params, "{\"model\":");
    }
    if (status == KC_KAIMAN_OK) status = json_write_text(params, model);
    if (status == KC_KAIMAN_OK && session != NULL) {
        status = kc_buffer_text(params, ",\"threadId\":");
        if (status == KC_KAIMAN_OK) status = json_write_text(params, session);
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_text(params, ",\"excludeTurns\":true");
        }
    } else if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(params, ",\"ephemeral\":true");
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(params,
            ",\"approvalPolicy\":\"never\",\"sandbox\":\"read-only\"}");
    }
    if (status == KC_KAIMAN_OK) {
        status = codex_rpc(child, 3U, session != NULL ?
            "thread/resume" : "thread/start", kc_buffer_value(params),
            deadline, &response);
    }
    if (status == KC_KAIMAN_OK && session != NULL) {
        const kc_json_value_t *info = kc_json_member(
            kc_json_member(response, "result"), "thread");

        if (native_equals(kc_json_member(kc_json_member(info, "status"),
            "type"), "active")) status = KC_KAIMAN_EBUSY;
    }
    if (status == KC_KAIMAN_OK) {
        id = kc_json_text(kc_json_member(kc_json_member(
            kc_json_member(response, "result"), "thread"), "id"));
        thread = id != NULL ? kc_util_strdup(id) : NULL;
        if (thread == NULL) status = KC_KAIMAN_EPROTO;
    }
    kc_json_free(response);
    response = NULL;
    if (status == KC_KAIMAN_OK) {
        params->size = 0U;
        params->data[0] = '\0';
        status = kc_buffer_text(params, "{\"threadId\":");
    }
    if (status == KC_KAIMAN_OK) status = json_write_text(params, thread);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(params, ",\"input\":[{\"type\":\"text\",\"text\":");
    }
    if (status == KC_KAIMAN_OK) status = json_write_text(params, prompt);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(params, ",\"text_elements\":[]}]}");
    }
    if (status == KC_KAIMAN_OK) {
        status = codex_rpc(child, 4U, "turn/start", kc_buffer_value(params),
            deadline, &response);
    }
    if (status == KC_KAIMAN_OK) {
        id = kc_json_text(kc_json_member(kc_json_member(
            kc_json_member(response, "result"), "turn"), "id"));
        turn = id != NULL ? kc_util_strdup(id) : NULL;
        if (turn == NULL) status = KC_KAIMAN_EPROTO;
    }
    if (status == KC_KAIMAN_OK) {
        status = codex_reply(child, thread, turn, deadline, reply);
    }
    kc_json_free(response);
    kc_buffer_close(params);
    if (session != NULL) {
        if (status != KC_KAIMAN_OK && status != KC_KAIMAN_EBUSY) {
            codex_disconnect(gateway);
        }
        kc_mutex_unlock(&gateway->mutex);
    } else {
        kc_child_close(child);
    }
    free(thread);
    free(turn);
    return status;
}

/**
 * Route a text chat through the installed Codex app-server.
 * @param gateway Open gateway.
 * @param peer Borrowed client peer.
 * @param root Borrowed Chat Completions request.
 * @param service Selected local service.
 * @param model Requested upstream model.
 * @return KC_KAIMAN_OK or an error status.
 */
static int codex_chat(kc_kaiman_gateway_t *gateway, kc_netl_peer_t *peer,
    const kc_json_value_t *root, const kc_kaiman_service_t *service,
    const char *model) {
    const char *requested = kc_json_text(kc_json_member(root, "model"));
    kc_buffer_t *prompt = NULL;
    kc_buffer_t *reply = NULL;
    kc_kaiman_stream_t stream;
    char *text = NULL;
    char *final = NULL;
    int streaming = kc_json_boolean(kc_json_member(root, "stream"));
    int status;

    memset(&stream, 0, sizeof(stream));
    status = kc_buffer_open(&prompt);
    if (status == KC_KAIMAN_OK) status = codex_prompt(root, prompt);
    if (status != KC_KAIMAN_OK) {
        status = gateway_failure(gateway, peer, 400,
            "invalid or unsupported Codex text request");
        goto done;
    }
    status = kc_buffer_open(&reply);
    if (status == KC_KAIMAN_OK) {
        status = codex_generate(gateway, service, model,
            session_raw(service, kc_json_text(kc_json_member(root, "session"))),
            kc_buffer_value(prompt), reply);
    }
    if (status != KC_KAIMAN_OK) {
        status = gateway_failure(gateway, peer,
            status == KC_KAIMAN_EBUSY ? 409 : 502,
            status == KC_KAIMAN_EBUSY ? "session is active" :
                "Codex local inference failed; check the installation and login");
        goto done;
    }
    text = native_completion(kc_buffer_value(reply), requested,
        streaming ? NULL : "stop", streaming);
    if (streaming) final = native_completion(NULL, requested, "stop", 1);
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
    free(text);
    free(final);
    kc_buffer_close(prompt);
    kc_buffer_close(reply);
    return status;
}

/**
 * Append one discovered model using a collision-free service namespace.
 * @param buffer Destination list.
 * @param service Source service.
 * @param provider Nested provider identifier, or NULL.
 * @param model Upstream model identifier.
 * @param count Number of models already written, updated on success.
 * @return KC_KAIMAN_OK or an error status.
 */
static int catalog_append(kc_buffer_t *buffer,
    const kc_kaiman_service_t *service, const char *provider,
    const char *model, size_t *count) {
    kc_buffer_t *id = NULL;
    int status;

    if (model == NULL || *model == '\0') return KC_KAIMAN_EFORMAT;
    status = kc_buffer_open(&id);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(id, "%s/", service->name);
    }
    if (status == KC_KAIMAN_OK && provider != NULL) {
        status = kc_buffer_format(id, "%s/", provider);
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(id, model);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(buffer, *count == 0U ? "{\"id\":" : ",{\"id\":");
    }
    if (status == KC_KAIMAN_OK) {
        status = json_write_text(buffer, kc_buffer_value(id));
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(buffer,
            ",\"object\":\"model\",\"created\":0,\"owned_by\":");
    }
    if (status == KC_KAIMAN_OK) {
        status = json_write_text(buffer, service->name);
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "}");
    if (status == KC_KAIMAN_OK) (*count)++;
    kc_buffer_close(id);
    return status;
}

/**
 * Check whether a JSON string array contains a value.
 * @param array Borrowed array.
 * @param text Expected value.
 * @return 1 on match, otherwise 0.
 */
static int catalog_contains(const kc_json_value_t *array, const char *text) {
    size_t index;

    if (kc_json_type(array) != KC_JSON_ARRAY) return 0;
    for (index = 0U; index < kc_json_length(array); index++) {
        if (native_equals(kc_json_item(array, index), text)) return 1;
    }
    return 0;
}

/**
 * Decode model identifiers returned by the OpenCode server.
 * @param root Borrowed model array.
 * @param service Source service.
 * @param buffer Destination list.
 * @param count Number of models already written.
 * @return KC_KAIMAN_OK or an error status.
 */
static int catalog_opencode(const kc_json_value_t *root,
    const kc_kaiman_service_t *service, kc_buffer_t *buffer, size_t *count) {
    size_t index;
    int status = KC_KAIMAN_OK;

    if (kc_json_type(root) != KC_JSON_ARRAY) return KC_KAIMAN_EFORMAT;
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < kc_json_length(root); index++) {
        const kc_json_value_t *item = kc_json_item(root, index);
        const char *provider = kc_json_text(kc_json_member(item, "providerID"));
        const char *model = kc_json_text(kc_json_member(item, "modelID"));

        if (model == NULL) model = kc_json_text(kc_json_member(item, "id"));
        if (provider == NULL || *provider == '\0') return KC_KAIMAN_EFORMAT;
        status = catalog_append(buffer, service, provider, model, count);
    }
    return status;
}

/**
 * Decode one page of a provider model catalog.
 * @param root Borrowed response object.
 * @param service Source service.
 * @param profiles Whether this is a Bedrock inference profile page.
 * @param buffer Destination list.
 * @param count Number of models already written.
 * @return KC_KAIMAN_OK or an error status.
 */
static int catalog_page(const kc_json_value_t *root,
    const kc_kaiman_service_t *service, int profiles,
    kc_buffer_t *buffer, size_t *count) {
    const char *adapter = service->adapter;
    const char *array = "data";
    const char *key = "id";
    const kc_json_value_t *items;
    size_t index;
    int gemini = strcmp(adapter, "gemini") == 0;
    int bedrock = strcmp(adapter, "bedrock") == 0;
    int status = KC_KAIMAN_OK;

    if (strcmp(adapter, "opencode") == 0) {
        const kc_json_value_t *data = kc_json_member(root, "data");

        return catalog_opencode(data != NULL ? data : root, service,
            buffer, count);
    }
    if (gemini || strcmp(adapter, "ollama") == 0) {
        array = "models";
        key = gemini ? "name" : "model";
    } else if (bedrock) {
        array = profiles ? "inferenceProfileSummaries" : "modelSummaries";
        key = profiles ? "inferenceProfileId" : "modelId";
    }
    items = kc_json_member(root, array);
    if (kc_json_type(items) != KC_JSON_ARRAY) return KC_KAIMAN_EFORMAT;
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < kc_json_length(items); index++) {
        const kc_json_value_t *item = kc_json_item(items, index);
        const char *model = kc_json_text(kc_json_member(item, key));

        if (strcmp(adapter, "opencode-cloud") == 0) {
            const char *type = kc_json_text(kc_json_member(item, "type"));

            if (type != NULL && strcmp(type, "chat") != 0) continue;
        }
        if (gemini) {
            if (!catalog_contains(kc_json_member(item,
                "supportedGenerationMethods"), "generateContent")) continue;
            if (model != NULL && strncmp(model, "models/", 7U) == 0) model += 7U;
        }
        if (bedrock && !profiles &&
            !catalog_contains(kc_json_member(item, "outputModalities"), "TEXT")) {
            continue;
        }
        if (bedrock && profiles &&
            !native_equals(kc_json_member(item, "status"), "ACTIVE")) continue;
        if (strcmp(adapter, "ollama") == 0 && model == NULL) {
            model = kc_json_text(kc_json_member(item, "name"));
        }
        status = catalog_append(buffer, service, NULL, model, count);
    }
    return status;
}

/**
 * Resolve the Bedrock control-plane base URL from a runtime URL.
 * @param base Configured runtime or proxy base URL.
 * @return Owned control-plane URL, or NULL on failure.
 */
static char *catalog_bedrock_base(const char *base) {
    static const char prefix[] = "https://bedrock-runtime.";
    const char *region;
    const char *suffix;
    kc_buffer_t *buffer = NULL;
    char *result = NULL;
    int status;

    if (strncmp(base, prefix, sizeof(prefix) - 1U) != 0) {
        return kc_util_strdup(base);
    }
    region = base + sizeof(prefix) - 1U;
    suffix = strchr(region, '.');
    if (suffix == NULL ||
        (strcmp(suffix, ".amazonaws.com") != 0 &&
            strcmp(suffix, ".amazonaws.com/") != 0 &&
            strcmp(suffix, ".amazonaws.com.cn") != 0 &&
            strcmp(suffix, ".amazonaws.com.cn/") != 0) ||
        strspn(region, "abcdefghijklmnopqrstuvwxyz0123456789-") !=
            (size_t)(suffix - region)) {
        return kc_util_strdup(base);
    }
    status = kc_buffer_open(&buffer);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(buffer, "https://bedrock.%s", region);
    }
    if (status == KC_KAIMAN_OK) result = kc_util_strdup(kc_buffer_value(buffer));
    kc_buffer_close(buffer);
    return result;
}

/**
 * Fetch and parse a catalog endpoint using the service credentials.
 * @param service Source service.
 * @param base Catalog base URL.
 * @param path Endpoint path.
 * @param query Pagination parameter name, or NULL.
 * @param cursor Pagination cursor, or NULL.
 * @param authorization Borrowed authorization header, or NULL.
 * @param out Receives the owned catalog response.
 * @return KC_KAIMAN_OK or an error status.
 */
static int catalog_request(const kc_kaiman_service_t *service,
    const char *base, const char *path, const char *query, const char *cursor,
    const char *authorization, kc_json_value_t **out) {
    kc_upstream_request_t request;
    kc_upstream_response_t response;
    kc_upstream_field_t fields[2];
    kc_buffer_t *url = NULL;
    char *endpoint = api_url(base, path);
    int status;

    *out = NULL;
    if (endpoint == NULL) return KC_KAIMAN_ENOMEM;
    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    status = kc_buffer_open(&url);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(url, endpoint);
    if (status == KC_KAIMAN_OK && query != NULL && cursor != NULL) {
        status = kc_buffer_format(url, "%c%s=",
            strchr(endpoint, '?') == NULL ? '?' : '&', query);
        if (status == KC_KAIMAN_OK) status = catalog_cursor(url, cursor);
    }
    if (status == KC_KAIMAN_OK) {
        request.method = "GET";
        request.url = kc_buffer_value(url);
        request.headers = fields;
        request.header_count = provider_headers(service, authorization, fields);
        request.timeout = kc_kaiman_service_timeout(service);
        status = kc_upstream_send(&request, &response);
    }
    if (status == KC_KAIMAN_OK && response.status != 200) {
        status = KC_KAIMAN_EHTTP;
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_json_parse(response.body, response.body_size, out);
    }
    kc_upstream_response_free(&response);
    kc_buffer_close(url);
    kc_kaiman_free(endpoint);
    return status;
}

/**
 * Fetch all pages of a provider catalog endpoint.
 * @param service Source service.
 * @param base Catalog base URL.
 * @param path Endpoint path.
 * @param profiles Whether this lists Bedrock inference profiles.
 * @param authorization Borrowed authorization header, or NULL.
 * @param buffer Destination list.
 * @param count Number of models already written.
 * @return KC_KAIMAN_OK or an error status.
 */
static int catalog_fetch(const kc_kaiman_service_t *service,
    const char *base, const char *path, int profiles, const char *authorization,
    kc_buffer_t *buffer, size_t *count) {
    const char *query = NULL;
    const char *next = NULL;
    char *cursor = NULL;
    size_t page;
    int status = KC_KAIMAN_OK;

    if (strcmp(service->adapter, "anthropic") == 0) {
        query = "after_id";
        next = "last_id";
    } else if (strcmp(service->adapter, "gemini") == 0) {
        query = "pageToken";
        next = "nextPageToken";
    } else if (profiles) {
        query = "nextToken";
        next = "nextToken";
    }
    for (page = 0U; page < 100U; page++) {
        kc_json_value_t *root = NULL;
        const char *value = NULL;
        char *following = NULL;

        status = catalog_request(service, base, path, query, cursor,
            authorization, &root);
        if (status == KC_KAIMAN_OK) {
            status = catalog_page(root, service, profiles, buffer, count);
        }
        if (status == KC_KAIMAN_OK && next != NULL &&
            (strcmp(query, "after_id") != 0 ||
                kc_json_boolean(kc_json_member(root, "has_more")))) {
            value = kc_json_text(kc_json_member(root, next));
            if (value != NULL && *value != '\0') {
                if (cursor != NULL && strcmp(value, cursor) == 0) {
                    status = KC_KAIMAN_EFORMAT;
                } else {
                    following = kc_util_strdup(value);
                    if (following == NULL) status = KC_KAIMAN_ENOMEM;
                }
            } else if (strcmp(query, "after_id") == 0) {
                status = KC_KAIMAN_EFORMAT;
            }
        }
        kc_json_free(root);
        kc_kaiman_free(cursor);
        cursor = following;
        if (status != KC_KAIMAN_OK || cursor == NULL) break;
    }
    if (page == 100U) status = KC_KAIMAN_EFORMAT;
    kc_kaiman_free(cursor);
    return status;
}

/**
 * Discover a service catalog without using configured model restrictions.
 * @param service Source service.
 * @param buffer Destination list.
 * @param count Number of models already written.
 * @return KC_KAIMAN_OK or an error status.
 */
static int catalog_http(const kc_kaiman_service_t *service,
    kc_buffer_t *buffer, size_t *count) {
    const char *path = "/models";
    char *authorization = NULL;
    char *base = NULL;
    size_t initial_size = buffer->size;
    size_t initial_count = *count;
    int status;

    const char *url = kc_kaiman_service_url(service);

    if (service->adapter == NULL || url == NULL) return KC_KAIMAN_EINVAL;
    status = service_authorization(service, &authorization);
    if (status != KC_KAIMAN_OK) return status;
    if (strcmp(service->adapter, "opencode") == 0) path = "/api/model";
    else if (strcmp(service->adapter, "opencode-cloud") == 0) path = "/v1/models";
    else if (strcmp(service->adapter, "ollama") == 0) path = "/api/tags";
    else if (strcmp(service->adapter, "bedrock") == 0) path = "/foundation-models";
    base = strcmp(service->adapter, "bedrock") == 0 ?
        catalog_bedrock_base(url) : kc_util_strdup(url);
    if (base == NULL) {
        kc_kaiman_free(authorization);
        return KC_KAIMAN_ENOMEM;
    }
    status = catalog_fetch(service, base, path, 0, authorization, buffer, count);
    if (strcmp(service->adapter, "bedrock") == 0) {
        size_t before = buffer->size;
        size_t before_count = *count;
        int profiles;

        if (status != KC_KAIMAN_OK) {
            buffer->size = initial_size;
            buffer->data[initial_size] = '\0';
            *count = initial_count;
            before = initial_size;
            before_count = initial_count;
        }
        profiles = catalog_fetch(service, base, "/inference-profiles", 1,
            authorization, buffer, count);
        if (profiles != KC_KAIMAN_OK) {
            buffer->size = before;
            buffer->data[before] = '\0';
            *count = before_count;
        } else {
            status = KC_KAIMAN_OK;
        }
    }
    kc_kaiman_free(base);
    kc_kaiman_free(authorization);
    return status;
}

/**
 * Discover model identifiers from the installed Codex app-server.
 * @param service Source service.
 * @param buffer Destination list.
 * @param count Number of models already written.
 * @return KC_KAIMAN_OK or an error status.
 */
static int catalog_codex(const kc_kaiman_service_t *service,
    kc_buffer_t *buffer, size_t *count) {
    kc_child_t *child = NULL;
    kc_buffer_t *params = NULL;
    char *cursor = NULL;
    double deadline = kc_util_seconds() +
        (double)kc_kaiman_service_timeout(service);
    unsigned int page;
    int status;

    status = codex_open(&child, deadline);
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&params);
    for (page = 0U; status == KC_KAIMAN_OK && page < 100U; page++) {
        kc_json_value_t *response = NULL;
        const kc_json_value_t *result;
        const kc_json_value_t *items;
        const char *next;
        char *following = NULL;
        size_t index;

        params->size = 0U;
        if (params->data != NULL) params->data[0] = '\0';
        status = kc_buffer_text(params, "{\"limit\":100,\"includeHidden\":true");
        if (status == KC_KAIMAN_OK && cursor != NULL) {
            status = kc_buffer_text(params, ",\"cursor\":");
            if (status == KC_KAIMAN_OK) status = json_write_text(params, cursor);
        }
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(params, "}");
        if (status == KC_KAIMAN_OK) {
            status = codex_rpc(child, page + 3U, "model/list",
                kc_buffer_value(params), deadline, &response);
        }
        result = kc_json_member(response, "result");
        items = kc_json_member(result, "data");
        if (status == KC_KAIMAN_OK && kc_json_type(items) != KC_JSON_ARRAY) {
            status = KC_KAIMAN_EFORMAT;
        }
        for (index = 0U; status == KC_KAIMAN_OK &&
            index < kc_json_length(items); index++) {
            const kc_json_value_t *item = kc_json_item(items, index);
            const char *model = kc_json_text(kc_json_member(item, "model"));

            if (model == NULL) model = kc_json_text(kc_json_member(item, "id"));
            status = catalog_append(buffer, service, NULL, model, count);
        }
        next = kc_json_text(kc_json_member(result, "nextCursor"));
        if (status == KC_KAIMAN_OK && next != NULL && *next != '\0') {
            if (cursor != NULL && strcmp(cursor, next) == 0) {
                status = KC_KAIMAN_EFORMAT;
            } else {
                following = kc_util_strdup(next);
                if (following == NULL) status = KC_KAIMAN_ENOMEM;
            }
        }
        kc_json_free(response);
        free(cursor);
        cursor = following;
        if (cursor == NULL) break;
    }
    if (page == 100U) status = KC_KAIMAN_EFORMAT;
    free(cursor);
    kc_buffer_close(params);
    kc_child_close(child);
    return status;
}

/**
 * Discover a service catalog through its configured transport.
 * @param service Source service.
 * @param buffer Destination list.
 * @param count Number of models already written.
 * @return KC_KAIMAN_OK or an error status.
 */
static int catalog_service(const kc_kaiman_service_t *service,
    kc_buffer_t *buffer, size_t *count) {
    kc_kaiman_service_t resolved;
    char *url = NULL;
    char *password = NULL;
    int status;

    if (service->adapter == NULL) return KC_KAIMAN_EINVAL;
    if (strcmp(service->adapter, "codex-local") == 0) {
        return catalog_codex(service, buffer, count);
    }
    if (strcmp(service->adapter, "opencode-local") != 0) {
        return catalog_http(service, buffer, count);
    }
    status = opencode_resolve(service, &resolved, &url, &password);
    if (status == KC_KAIMAN_OK) {
        status = catalog_http(&resolved, buffer, count);
    }
    free(url);
    free(password);
    return status;
}

/**
 * Test a proposed service configuration by discovering its models.
 * @param kaiman Open manager containing any saved credentials.
 * @param descriptor Proposed service fields.
 * @param secret Temporary credential, or NULL to use a saved source.
 * @param count Receives the discovered model count.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_kaiman_service_test(kc_kaiman_t *kaiman,
    const kc_kaiman_descriptor_t *descriptor, const char *secret, size_t *count) {
    static const char *const keys[] = {"adapter", "url", "env",
        "token-command", "scheme", "user"};
    const char *values[6];
    kc_kaiman_service_t preview;
    kc_kaiman_service_t *saved;
    kc_buffer_t *buffer = NULL;
    const char *scheme;
    size_t index;
    int local;
    int status = KC_KAIMAN_OK;

    if (count != NULL) *count = 0U;
    if (kaiman == NULL || descriptor == NULL || count == NULL ||
        !valid_name(descriptor->name) || descriptor->adapter == NULL ||
        descriptor->timeout < 0 || descriptor->timeout > 600) {
        set_error(kaiman, "invalid service configuration");
        return KC_KAIMAN_EINVAL;
    }
    free(kaiman->error);
    kaiman->error = NULL;
    memset(&preview, 0, sizeof(preview));
    preview.owner = kaiman;
    preview.alive = 1;
    preview.timeout = descriptor->timeout != 0 ?
        descriptor->timeout : DEFAULT_TIMEOUT;
    preview.name = copy_text(descriptor->name);
    if (preview.name == NULL) return KC_KAIMAN_ENOMEM;
    saved = find_service(kaiman, descriptor->name);
    local = strcmp(descriptor->adapter, "opencode-local") == 0 ||
        strcmp(descriptor->adapter, "codex-local") == 0;
    scheme = local ? "none" : descriptor->auth_scheme != NULL ?
        descriptor->auth_scheme : "bearer";
    values[0] = descriptor->adapter;
    values[1] = local ? "" : descriptor->base_url;
    values[2] = descriptor->api_key_env;
    values[3] = descriptor->token_command;
    values[4] = scheme;
    values[5] = descriptor->auth_user;
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < sizeof(keys) / sizeof(keys[0]); index++) {
        if (values[index] != NULL) {
            status = apply_field(&preview, keys[index], values[index]);
        }
    }
    if (status != KC_KAIMAN_OK) goto done;
    if (!local && strcmp(scheme, "basic") == 0 &&
        (preview.auth_user == NULL || *preview.auth_user == '\0' ||
            strchr(preview.auth_user, ':') != NULL)) {
        set_error(kaiman, "a valid username is required");
        status = KC_KAIMAN_EINVAL;
        goto done;
    }
    if (!local && strcmp(scheme, "none") != 0) {
        if (secret != NULL) {
            if (*secret == '\0' || !valid_value(secret)) {
                set_error(kaiman, "invalid credential");
                status = KC_KAIMAN_EINVAL;
                goto done;
            }
            preview.local_password = secret;
        } else if (preview.api_key_env == NULL &&
            preview.token_command == NULL && saved != NULL &&
            saved->auth_scheme != NULL &&
            strcmp(saved->auth_scheme, scheme) == 0) {
            if (saved->api_key_env != NULL) {
                status = apply_field(&preview, "env", saved->api_key_env);
            }
            if (status == KC_KAIMAN_OK && saved->token_command != NULL) {
                status = apply_field(&preview, "token-command",
                    saved->token_command);
            }
            if (status == KC_KAIMAN_OK && saved->keyring != NULL) {
                status = apply_field(&preview, "keyring", saved->keyring);
            }
        }
        if (status == KC_KAIMAN_OK && preview.local_password == NULL &&
            (preview.api_key_env == NULL || *preview.api_key_env == '\0') &&
            (preview.token_command == NULL || *preview.token_command == '\0') &&
            (preview.keyring == NULL || *preview.keyring == '\0')) {
            set_error(kaiman, "a credential is required");
            status = KC_KAIMAN_EINVAL;
        }
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&buffer);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(buffer, "[");
    }
    if (status == KC_KAIMAN_OK) {
        status = catalog_service(&preview, buffer, count);
        if (status != KC_KAIMAN_OK) {
            set_error(kaiman, "model discovery failed: %s",
                kc_kaiman_strerror(status));
        }
    }

done:
    if (status != KC_KAIMAN_OK) *count = 0U;
    kc_buffer_close(buffer);
    service_fields_clear(&preview);
    return status;
}

/**
 * Decode a single hexadecimal URL escape digit.
 * @param byte ASCII character.
 * @return Digit value, or -1 on failure.
 */
static int catalog_hex(unsigned char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

/**
 * Parse optional service filters into a comma-separated service list.
 * @param query Borrowed HTTP query string, or NULL.
 * @param out Receives owned filter text, or NULL when absent.
 * @return KC_KAIMAN_OK or an error status.
 */
static int catalog_filter(const char *query, char **out) {
    const char *start = query;
    char *filter;
    size_t size = 0U;
    size_t name_size = 0U;

    *out = NULL;
    if (query == NULL || *query == '\0') return KC_KAIMAN_OK;
    filter = (char *)malloc(strlen(query) + 1U);
    if (filter == NULL) return KC_KAIMAN_ENOMEM;
    while (*start != '\0') {
        const char *end = strchr(start, '&');
        const char *cursor;

        if (end == NULL) end = start + strlen(start);
        if ((size_t)(end - start) >= 8U &&
            strncmp(start, "provider", 8U) == 0 &&
            (start + 8U == end || start[8] == '=')) goto invalid;
        if ((size_t)(end - start) >= 7U &&
            strncmp(start, "service", 7U) == 0 &&
            (start + 7U == end || start[7] == '=')) {
            if (start + 7U == end) goto invalid;
            if (size != 0U) filter[size++] = ',';
            name_size = 0U;
            for (cursor = start + 8U; cursor < end; cursor++) {
                unsigned char byte = (unsigned char)*cursor;

                if (byte == '%') {
                    int high;
                    int low;

                    if (end - cursor < 3) goto invalid;
                    high = catalog_hex((unsigned char)cursor[1]);
                    low = catalog_hex((unsigned char)cursor[2]);
                    if (high < 0 || low < 0) goto invalid;
                    byte = (unsigned char)(high * 16 + low);
                    cursor += 2;
                }
                if (byte == ',') {
                    if (name_size == 0U) goto invalid;
                    name_size = 0U;
                } else {
                    if (!isalnum(byte) && byte != '_' && byte != '-' &&
                        byte != '.') goto invalid;
                    name_size++;
                }
                filter[size++] = (char)byte;
            }
            if (name_size == 0U) goto invalid;
        }
        start = *end == '\0' ? end : end + 1U;
    }
    if (size == 0U) {
        free(filter);
        return KC_KAIMAN_OK;
    }
    filter[size] = '\0';
    *out = filter;
    return KC_KAIMAN_OK;

invalid:
    free(filter);
    return KC_KAIMAN_EINVAL;
}

/**
 * Check whether a service is selected by an optional catalog filter.
 * @param filter Comma-separated service names, or NULL for all.
 * @param name Configured service name.
 * @return 1 when selected, otherwise 0.
 */
static int catalog_selected(const char *filter, const char *name) {
    const char *start = filter;
    size_t length = strlen(name);

    if (filter == NULL) return 1;
    while (*start != '\0') {
        const char *end = strchr(start, ',');
        size_t size = end != NULL ? (size_t)(end - start) : strlen(start);

        if (size == length && strncmp(start, name, length) == 0) return 1;
        if (end == NULL) break;
        start = end + 1U;
    }
    return 0;
}

/**
 * Build a catalog of supported provider types.
 * @param out Receives owned JSON text.
 * @return KC_KAIMAN_OK or an error status.
 */
static int gateway_providers(char **out) {
    kc_buffer_t *buffer = NULL;
    size_t index;
    int status;

    *out = NULL;
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) return status;
    status = kc_buffer_text(buffer, "{\"data\":[");
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < sizeof(providers) / sizeof(providers[0]); index++) {
        status = kc_buffer_text(buffer, index == 0U ?
            "{\"name\":" : ",{\"name\":");
        if (status == KC_KAIMAN_OK) {
            status = json_write_text(buffer, providers[index].name);
        }
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, ",\"slug\":");
        if (status == KC_KAIMAN_OK) {
            status = json_write_text(buffer, providers[index].slug);
        }
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "}");
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "]}");
    if (status == KC_KAIMAN_OK) {
        *out = kc_util_strdup(kc_buffer_value(buffer));
        if (*out == NULL) status = KC_KAIMAN_ENOMEM;
    }
    kc_buffer_close(buffer);
    return status;
}

/**
 * Build the list of configured services without contacting upstream servers.
 * @param kaiman Open manager.
 * @param out Receives owned JSON text.
 * @return KC_KAIMAN_OK or an error status.
 */
static int gateway_services(const kc_kaiman_t *kaiman, char **out) {
    kc_buffer_t *buffer = NULL;
    size_t index;
    int status;

    *out = NULL;
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) return status;
    status = kc_buffer_text(buffer, "{\"data\":[");
    for (index = 0U; status == KC_KAIMAN_OK && index < kaiman->count; index++) {
        const kc_kaiman_service_t *service = kaiman->services[index];
        const char *name = service->display_name;

        if (name == NULL || *name == '\0') name = service->name;
        status = kc_buffer_text(buffer, index == 0U ?
            "{\"name\":" : ",{\"name\":");
        if (status == KC_KAIMAN_OK) status = json_write_text(buffer, name);
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, ",\"slug\":");
        if (status == KC_KAIMAN_OK) {
            status = json_write_text(buffer, service->name);
        }
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_text(buffer, ",\"provider\":");
        }
        if (status == KC_KAIMAN_OK) {
            status = json_write_text(buffer, service->adapter);
        }
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "}");
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "]}");
    if (status == KC_KAIMAN_OK) {
        *out = kc_util_strdup(kc_buffer_value(buffer));
        if (*out == NULL) status = KC_KAIMAN_ENOMEM;
    }
    kc_buffer_close(buffer);
    return status;
}

/**
 * Build the unified model catalog from currently reachable services.
 * @param gateway Open gateway.
 * @param filter Selected service names, or NULL for all.
 * @param out Receives owned OpenAI-compatible JSON.
 * @return KC_KAIMAN_OK or an error status.
 */
static int gateway_models(kc_kaiman_gateway_t *gateway, const char *filter,
    char **out) {
    kc_buffer_t *buffer = NULL;
    kc_kaiman_t *kaiman = gateway->manager;
    size_t index;
    size_t count = 0U;
    size_t available = 0U;
    size_t selected = 0U;
    int status;

    *out = NULL;
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) return status;
    status = kc_buffer_text(buffer, "{\"object\":\"list\",\"data\":[");
    for (index = 0U; status == KC_KAIMAN_OK && index < kaiman->count; index++) {
        size_t before = buffer->size;
        size_t before_count = count;
        int discovered;

        if (!catalog_selected(filter, kaiman->services[index]->name)) continue;
        selected++;
        discovered = catalog_service(kaiman->services[index], buffer, &count);
        if (discovered == KC_KAIMAN_OK) {
            available++;
        } else {
            buffer->size = before;
            buffer->data[before] = '\0';
            count = before_count;
        }
    }
    if (status == KC_KAIMAN_OK && selected != 0U && available == 0U) {
        status = KC_KAIMAN_EHTTP;
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "]}");
    if (status == KC_KAIMAN_OK) {
        *out = kc_util_strdup(kc_buffer_value(buffer));
        if (*out == NULL) status = KC_KAIMAN_ENOMEM;
    }
    kc_buffer_close(buffer);
    return status;
}

/**
 * Write normalized session metadata without exposing the transcript.
 * @param buffer Destination buffer.
 * @param service Configured service.
 * @param id Upstream session identifier.
 * @param title Optional session title.
 * @param state Normalized runtime state.
 * @return KC_KAIMAN_OK or an error status.
 */
static int session_write(kc_buffer_t *buffer,
    const kc_kaiman_service_t *service, const char *id, const char *title,
    const char *state) {
    kc_buffer_t *name = NULL;
    int status;

    if (!session_valid(id)) return KC_KAIMAN_EFORMAT;
    status = kc_buffer_open(&name);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(name, "%s/%s", service->name, id);
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "{\"id\":");
    if (status == KC_KAIMAN_OK) {
        status = json_write_text(buffer, kc_buffer_value(name));
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(buffer, ",\"service\":");
    }
    if (status == KC_KAIMAN_OK) status = json_write_text(buffer, service->name);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(buffer, ",\"title\":");
    }
    if (status == KC_KAIMAN_OK) {
        status = title != NULL ? json_write_text(buffer, title) :
            kc_buffer_text(buffer, "null");
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(buffer, ",\"status\":");
    }
    if (status == KC_KAIMAN_OK) status = json_write_text(buffer, state);
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "}");
    kc_buffer_close(name);
    return status;
}

/**
 * Normalize the state returned for a Codex thread.
 * @param thread Borrowed thread metadata.
 * @return Borrowed normalized state.
 */
static const char *codex_state(const kc_json_value_t *thread) {
    const kc_json_value_t *type = kc_json_member(
        kc_json_member(thread, "status"), "type");

    if (native_equals(type, "active")) return "active";
    if (native_equals(type, "idle")) return "idle";
    if (native_equals(type, "systemError")) return "error";
    return "unknown";
}

/**
 * Append a page of upstream sessions to a normalized catalog.
 * @param items Borrowed array of sessions or threads.
 * @param active Borrowed OpenCode activity map, or NULL when unavailable.
 * @param service Source provider.
 * @param buffer Destination list.
 * @param count Number of entries already written.
 * @return KC_KAIMAN_OK or an error status.
 */
static int session_page(const kc_json_value_t *items,
    const kc_json_value_t *active, const kc_kaiman_service_t *service,
    kc_buffer_t *buffer, size_t *count) {
    size_t index;
    int codex = session_type(service) == 2;
    int status = KC_KAIMAN_OK;

    if (kc_json_type(items) != KC_JSON_ARRAY) return KC_KAIMAN_EFORMAT;
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < kc_json_length(items); index++) {
        const kc_json_value_t *item = kc_json_item(items, index);
        const char *id = kc_json_text(kc_json_member(item, "id"));
        const char *title = kc_json_text(kc_json_member(item,
            codex ? "name" : "title"));
        const char *state = "unknown";

        if (!session_valid(id)) return KC_KAIMAN_EFORMAT;
        if (codex) {
            if (kc_json_boolean(kc_json_member(item, "ephemeral"))) continue;
            state = codex_state(item);
        } else if (active != NULL) {
            state = kc_json_member(active, id) != NULL ? "active" : "idle";
        }
        if (*count != 0U) status = kc_buffer_text(buffer, ",");
        if (status == KC_KAIMAN_OK) {
            status = session_write(buffer, service, id, title, state);
        }
        if (status == KC_KAIMAN_OK) (*count)++;
    }
    return status;
}

/**
 * Discover non-archived sessions through the OpenCode HTTP API.
 * @param service Resolved HTTP service.
 * @param buffer Destination list.
 * @param count Number of entries already written.
 * @return KC_KAIMAN_OK or an error status.
 */
static int session_opencode(const kc_kaiman_service_t *service,
    kc_buffer_t *buffer, size_t *count) {
    kc_json_value_t *active = NULL;
    char *authorization = NULL;
    char *cursor = NULL;
    unsigned int page;
    int status;

    status = service_authorization(service, &authorization);
    if (status != KC_KAIMAN_OK) return status;
    (void)opencode_active(service, &active);
    for (page = 0U; page < 100U; page++) {
        kc_json_value_t *root = NULL;
        const char *next;
        char *following = NULL;

        status = catalog_request(service, kc_kaiman_service_url(service),
            "/api/session?limit=100&archived=false", "cursor", cursor,
            authorization, &root);
        if (status == KC_KAIMAN_OK) {
            status = session_page(kc_json_member(root, "data"), active,
                service, buffer, count);
        }
        next = kc_json_text(kc_json_member(kc_json_member(root, "cursor"),
            "next"));
        if (status == KC_KAIMAN_OK && next != NULL && *next != '\0') {
            if (cursor != NULL && strcmp(cursor, next) == 0) {
                status = KC_KAIMAN_EFORMAT;
            } else {
                following = kc_util_strdup(next);
                if (following == NULL) status = KC_KAIMAN_ENOMEM;
            }
        }
        kc_json_free(root);
        free(cursor);
        cursor = following;
        if (status != KC_KAIMAN_OK || cursor == NULL) break;
    }
    if (page == 100U) status = KC_KAIMAN_EFORMAT;
    kc_json_free(active);
    free(authorization);
    free(cursor);
    return status;
}

/**
 * Discover non-archived local Codex threads for the current account.
 * @param gateway Open gateway with its mutex held.
 * @param service Configured Codex service.
 * @param buffer Destination list.
 * @param count Number of entries already written.
 * @return KC_KAIMAN_OK or an error status.
 */
static int session_codex(kc_kaiman_gateway_t *gateway,
    const kc_kaiman_service_t *service, kc_buffer_t *buffer, size_t *count) {
    kc_buffer_t *params = NULL;
    char *cursor = NULL;
    double deadline = kc_util_seconds() +
        (double)kc_kaiman_service_timeout(service);
    unsigned int page;
    int status;

    status = codex_connect(gateway, deadline);
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&params);
    for (page = 0U; status == KC_KAIMAN_OK && page < 100U; page++) {
        kc_json_value_t *response = NULL;
        const kc_json_value_t *result;
        const char *next;
        char *following = NULL;

        params->size = 0U;
        if (params->data != NULL) params->data[0] = '\0';
        status = kc_buffer_text(params,
            "{\"limit\":100,\"archived\":false,\"currentAccountOnly\":true");
        if (status == KC_KAIMAN_OK && cursor != NULL) {
            status = kc_buffer_text(params, ",\"cursor\":");
            if (status == KC_KAIMAN_OK) status = json_write_text(params, cursor);
        }
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(params, "}");
        if (status == KC_KAIMAN_OK) {
            status = codex_rpc(gateway->codex, page + 3U, "thread/list",
                kc_buffer_value(params), deadline, &response);
        }
        result = kc_json_member(response, "result");
        if (status == KC_KAIMAN_OK) {
            status = session_page(kc_json_member(result, "data"), NULL,
                service, buffer, count);
        }
        next = kc_json_text(kc_json_member(result, "nextCursor"));
        if (status == KC_KAIMAN_OK && next != NULL && *next != '\0') {
            if (cursor != NULL && strcmp(cursor, next) == 0) {
                status = KC_KAIMAN_EFORMAT;
            } else {
                following = kc_util_strdup(next);
                if (following == NULL) status = KC_KAIMAN_ENOMEM;
            }
        }
        kc_json_free(response);
        free(cursor);
        cursor = following;
        if (cursor == NULL) break;
    }
    if (page == 100U) status = KC_KAIMAN_EFORMAT;
    if (status != KC_KAIMAN_OK) codex_disconnect(gateway);
    free(cursor);
    kc_buffer_close(params);
    return status;
}

/**
 * Build a session catalog from selected session-capable providers.
 * @param gateway Open gateway.
 * @param filter Selected service names, or NULL for all.
 * @param out Receives owned JSON text.
 * @return KC_KAIMAN_OK or an error status.
 */
static int gateway_sessions(kc_kaiman_gateway_t *gateway, const char *filter,
    char **out) {
    kc_buffer_t *buffer = NULL;
    size_t index;
    size_t count = 0U;
    size_t selected = 0U;
    size_t available = 0U;
    int status;

    *out = NULL;
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) return status;
    status = kc_buffer_text(buffer, "{\"data\":[");
    kc_mutex_lock(&gateway->mutex);
    for (index = 0U; status == KC_KAIMAN_OK &&
        index < gateway->manager->count; index++) {
        kc_kaiman_service_t *service = gateway->manager->services[index];
        kc_kaiman_service_t resolved;
        char *url = NULL;
        char *password = NULL;
        size_t before = buffer->size;
        size_t previous = count;
        int type = session_type(service);
        int discovered = KC_KAIMAN_OK;

        if ((type != 1 && type != 2) ||
            !catalog_selected(filter, service->name)) continue;
        selected++;
        if (type == 2) {
            discovered = session_codex(gateway, service, buffer, &count);
        } else {
            if (strcmp(service->adapter, "opencode-local") == 0) {
                discovered = opencode_resolve(service, &resolved, &url,
                    &password);
                if (discovered == KC_KAIMAN_OK) service = &resolved;
            }
            if (discovered == KC_KAIMAN_OK) {
                discovered = session_opencode(service, buffer, &count);
            }
        }
        free(url);
        free(password);
        if (discovered == KC_KAIMAN_OK) {
            available++;
        } else {
            buffer->size = before;
            buffer->data[before] = '\0';
            count = previous;
        }
    }
    kc_mutex_unlock(&gateway->mutex);
    if (status == KC_KAIMAN_OK && selected != 0U && available == 0U) {
        status = KC_KAIMAN_EHTTP;
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(buffer, "]}");
    if (status == KC_KAIMAN_OK) {
        *out = kc_util_strdup(kc_buffer_value(buffer));
        if (*out == NULL) status = KC_KAIMAN_ENOMEM;
    }
    kc_buffer_close(buffer);
    return status;
}

/**
 * Create or fork an OpenCode session through its server API.
 * @param service Resolved HTTP service.
 * @param model Upstream model for a new session, or NULL.
 * @param parent Upstream source session for a fork, or NULL.
 * @param title Optional title for a new session.
 * @param before Optional message before which to fork.
 * @param out Destination for normalized session metadata.
 * @return KC_KAIMAN_OK or an error status.
 */
static int session_open_opencode(const kc_kaiman_service_t *service,
    const char *model, const char *parent, const char *title,
    const char *before, kc_buffer_t *out) {
    kc_upstream_response_t response;
    kc_json_value_t *root = NULL;
    kc_buffer_t *path = NULL;
    kc_buffer_t *body = NULL;
    char *provider = NULL;
    char *identifier = NULL;
    const kc_json_value_t *data;
    const char *name;
    int status;

    memset(&response, 0, sizeof(response));
    status = kc_buffer_open(&path);
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&body);
    if (status == KC_KAIMAN_OK) {
        status = parent != NULL ?
            kc_buffer_format(path, "/api/session/%s/fork", parent) :
            kc_buffer_text(path, "/api/session");
    }
    if (status == KC_KAIMAN_OK && parent != NULL) {
        status = kc_buffer_text(body, "{");
        if (status == KC_KAIMAN_OK && before != NULL) {
            status = kc_buffer_text(body, "\"before\":");
            if (status == KC_KAIMAN_OK) status = json_write_text(body, before);
        }
    } else if (status == KC_KAIMAN_OK) {
        status = opencode_split_model(model, &provider, &identifier);
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_text(body, "{\"title\":");
        }
        if (status == KC_KAIMAN_OK) {
            status = json_write_text(body, title != NULL ? title : "kaiman");
        }
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_text(body, ",\"model\":{\"providerID\":");
        }
        if (status == KC_KAIMAN_OK) status = json_write_text(body, provider);
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_text(body, ",\"id\":");
        }
        if (status == KC_KAIMAN_OK) status = json_write_text(body, identifier);
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(body, "}");
    }
    if (status == KC_KAIMAN_OK) status = kc_buffer_text(body, "}");
    if (status == KC_KAIMAN_OK) {
        status = opencode_request(service, "POST", kc_buffer_value(path),
            kc_buffer_value(body), &response);
    }
    if (status == KC_KAIMAN_OK && response.status >= 400) {
        status = response.status == 404 ? KC_KAIMAN_NOT_FOUND :
            (response.status == 409 ? KC_KAIMAN_EBUSY : KC_KAIMAN_EHTTP);
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_json_parse(response.body, response.body_size, &root);
    }
    if (status == KC_KAIMAN_OK) {
        data = kc_json_member(root, "data");
        name = kc_json_text(kc_json_member(data, "title"));
        status = session_write(out, service,
            kc_json_text(kc_json_member(data, "id")),
            name != NULL ? name : title, "idle");
    }
    kc_json_free(root);
    kc_upstream_response_free(&response);
    kc_buffer_close(path);
    kc_buffer_close(body);
    free(provider);
    free(identifier);
    return status;
}

/**
 * Create or fork a persistent Codex thread.
 * @param gateway Open gateway with its mutex held.
 * @param service Configured Codex service.
 * @param model Upstream model for a new thread, or NULL.
 * @param parent Source thread identifier for a fork, or NULL.
 * @param title Optional title for a new thread.
 * @param last_turn Optional last turn included in a fork.
 * @param out Destination for normalized session metadata.
 * @return KC_KAIMAN_OK or an error status.
 */
static int session_open_codex(kc_kaiman_gateway_t *gateway,
    const kc_kaiman_service_t *service, const char *model, const char *parent,
    const char *title, const char *last_turn, kc_buffer_t *out) {
    kc_buffer_t *params = NULL;
    kc_json_value_t *response = NULL;
    kc_json_value_t *renamed = NULL;
    const kc_json_value_t *thread;
    const char *id;
    const char *name;
    double deadline = kc_util_seconds() +
        (double)kc_kaiman_service_timeout(service);
    int status;

    status = codex_connect(gateway, deadline);
    if (status == KC_KAIMAN_OK) status = kc_buffer_open(&params);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(params, parent != NULL ?
            "{\"threadId\":" : "{\"model\":");
    }
    if (status == KC_KAIMAN_OK) {
        status = json_write_text(params, parent != NULL ? parent : model);
    }
    if (status == KC_KAIMAN_OK && parent != NULL) {
        status = kc_buffer_text(params, ",\"excludeTurns\":true");
        if (status == KC_KAIMAN_OK && last_turn != NULL) {
            status = kc_buffer_text(params, ",\"lastTurnId\":");
            if (status == KC_KAIMAN_OK) {
                status = json_write_text(params, last_turn);
            }
        }
    }
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_text(params,
            ",\"ephemeral\":false,\"approvalPolicy\":\"never\","
            "\"sandbox\":\"read-only\"}");
    }
    if (status == KC_KAIMAN_OK) {
        status = codex_rpc(gateway->codex, 3U, parent != NULL ?
            "thread/fork" : "thread/start", kc_buffer_value(params), deadline,
            &response);
    }
    thread = kc_json_member(kc_json_member(response, "result"), "thread");
    id = kc_json_text(kc_json_member(thread, "id"));
    name = kc_json_text(kc_json_member(thread, "name"));
    if (status == KC_KAIMAN_OK && !session_valid(id)) {
        status = KC_KAIMAN_EFORMAT;
    }
    if (status == KC_KAIMAN_OK && title != NULL) {
        params->size = 0U;
        params->data[0] = '\0';
        status = kc_buffer_text(params, "{\"threadId\":");
        if (status == KC_KAIMAN_OK) status = json_write_text(params, id);
        if (status == KC_KAIMAN_OK) {
            status = kc_buffer_text(params, ",\"name\":");
        }
        if (status == KC_KAIMAN_OK) status = json_write_text(params, title);
        if (status == KC_KAIMAN_OK) status = kc_buffer_text(params, "}");
        if (status == KC_KAIMAN_OK) {
            status = codex_rpc(gateway->codex, 4U, "thread/name/set",
                kc_buffer_value(params), deadline, &renamed);
        }
    }
    if (status == KC_KAIMAN_OK) {
        status = session_write(out, service, id,
            title != NULL ? title : name, codex_state(thread));
    }
    if (status != KC_KAIMAN_OK) codex_disconnect(gateway);
    kc_json_free(renamed);
    kc_json_free(response);
    kc_buffer_close(params);
    return status;
}

/**
 * Process an explicit session creation or fork request.
 * @param gateway Open gateway.
 * @param root Borrowed request object.
 * @param fork Whether to fork an existing session.
 * @param out Receives owned JSON metadata.
 * @return KC_KAIMAN_OK or an error status.
 */
static int gateway_session_open(kc_kaiman_gateway_t *gateway,
    const kc_json_value_t *root, int fork, char **out) {
    kc_kaiman_service_t *service = NULL;
    kc_kaiman_service_t resolved;
    kc_buffer_t *buffer = NULL;
    const char *requested = kc_json_text(kc_json_member(root,
        fork ? "session" : "model"));
    const char *title = kc_json_text(kc_json_member(root, "title"));
    const char *before = kc_json_text(kc_json_member(root, "before"));
    const char *last_turn = kc_json_text(kc_json_member(root, "last_turn"));
    const char *id = NULL;
    char *url = NULL;
    char *password = NULL;
    int type;
    int status;

    *out = NULL;
    if (kc_json_type(root) != KC_JSON_OBJECT || requested == NULL ||
        (kc_json_member(root, "title") != NULL && title == NULL) ||
        (kc_json_member(root, "before") != NULL && before == NULL) ||
        (kc_json_member(root, "last_turn") != NULL && last_turn == NULL)) {
        return KC_KAIMAN_EINVAL;
    }
    status = gateway_select(gateway->manager, requested, &service, &id);
    if (status != KC_KAIMAN_OK) return status;
    type = session_type(service);
    if ((type != 1 && type != 2) ||
        (fork && !session_valid(id)) ||
        (!fork && (before != NULL || last_turn != NULL)) ||
        (fork && title != NULL) ||
        (before != NULL && (type != 1 || !session_valid(before))) ||
        (last_turn != NULL && (type != 2 || !session_valid(last_turn)))) {
        return KC_KAIMAN_EINVAL;
    }
    status = kc_buffer_open(&buffer);
    if (status != KC_KAIMAN_OK) return status;
    kc_mutex_lock(&gateway->mutex);
    if (type == 2) {
        status = session_open_codex(gateway, service, fork ? NULL : id,
            fork ? id : NULL, title, last_turn, buffer);
    } else {
        if (strcmp(service->adapter, "opencode-local") == 0) {
            status = opencode_resolve(service, &resolved, &url, &password);
            if (status == KC_KAIMAN_OK) service = &resolved;
        }
        if (status == KC_KAIMAN_OK) {
            status = session_open_opencode(service, fork ? NULL : id,
                fork ? id : NULL, title, before, buffer);
        }
    }
    kc_mutex_unlock(&gateway->mutex);
    if (status == KC_KAIMAN_OK) {
        *out = kc_util_strdup(kc_buffer_value(buffer));
        if (*out == NULL) status = KC_KAIMAN_ENOMEM;
    }
    kc_buffer_close(buffer);
    free(url);
    free(password);
    return status;
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
        strcmp(request->path, "/v1/providers") == 0) {
        char *providers = NULL;
        int status = gateway_providers(&providers);

        if (status != KC_KAIMAN_OK) {
            (void)gateway_failure(gateway, connection->peer, 500,
                "could not list supported providers");
        } else {
            (void)gateway_json(gateway, connection->peer, 200, providers);
            kc_kaiman_free(providers);
        }
        return;
    }
    if (strcmp(request->method, "GET") == 0 &&
        strcmp(request->path, "/v1/services") == 0) {
        char *services = NULL;
        int status = gateway_services(gateway->manager, &services);

        if (status != KC_KAIMAN_OK) {
            (void)gateway_failure(gateway, connection->peer, 500,
                "could not list configured services");
        } else {
            (void)gateway_json(gateway, connection->peer, 200, services);
        }
        free(services);
        return;
    }
    if (strcmp(request->method, "GET") == 0 &&
        strcmp(request->path, "/v1/sessions") == 0) {
        char *filter = NULL;
        char *sessions = NULL;
        int status = catalog_filter(request->query, &filter);

        if (status == KC_KAIMAN_OK) {
            status = gateway_sessions(gateway, filter, &sessions);
        }
        free(filter);
        if (status != KC_KAIMAN_OK) {
            (void)gateway_failure(gateway, connection->peer,
                status == KC_KAIMAN_EINVAL ? 400 :
                (status == KC_KAIMAN_EHTTP ? 502 : 500),
                "could not discover sessions");
        } else {
            (void)gateway_json(gateway, connection->peer, 200, sessions);
        }
        free(sessions);
        return;
    }
    if (strcmp(request->method, "POST") == 0 &&
        (strcmp(request->path, "/v1/sessions") == 0 ||
            strcmp(request->path, "/v1/sessions/fork") == 0)) {
        char *session = NULL;
        int fork = strcmp(request->path, "/v1/sessions/fork") == 0;
        int status = kc_json_parse(kc_buffer_value(connection->body),
            connection->body->size, &root);

        if (status != KC_KAIMAN_OK) status = KC_KAIMAN_EINVAL;
        else status = gateway_session_open(gateway, root, fork, &session);
        if (status != KC_KAIMAN_OK) {
            int http = 502;

            if (status == KC_KAIMAN_EINVAL) http = 400;
            else if (status == KC_KAIMAN_NOT_FOUND) http = 404;
            else if (status == KC_KAIMAN_EBUSY) http = 409;
            else if (status == KC_KAIMAN_ENOMEM) http = 500;
            (void)gateway_failure(gateway, connection->peer, http,
                "could not create or fork session");
        } else {
            (void)gateway_json(gateway, connection->peer, 200, session);
        }
        free(session);
        kc_json_free(root);
        return;
    }
    if (strcmp(request->method, "GET") == 0 &&
        strcmp(request->path, "/v1/models") == 0) {
        char *filter = NULL;
        int status = catalog_filter(request->query, &filter);

        if (status == KC_KAIMAN_EINVAL) {
            (void)gateway_failure(gateway, connection->peer, 400,
                "invalid service filter");
            return;
        }
        if (status == KC_KAIMAN_OK) {
            status = gateway_models(gateway, filter, &models);
        }
        free(filter);
        if (status != KC_KAIMAN_OK) {
            (void)gateway_failure(gateway, connection->peer,
                status == KC_KAIMAN_EHTTP ? 502 : 500,
                "could not discover the model catalog");
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
            const char *requested = kc_json_text(kc_json_member(root, "model"));
            const char *model = NULL;
            kc_kaiman_service_t *service = NULL;

            if (gateway_select(gateway->manager, requested, &service, &model) !=
                KC_KAIMAN_OK || service->adapter == NULL) {
                (void)gateway_failure(gateway, connection->peer, 404,
                    "unknown model; use a service/model identifier");
            } else if (kc_json_member(root, "session") != NULL &&
                (session_type(service) == 0 ||
                    session_raw(service, kc_json_text(
                        kc_json_member(root, "session"))) == NULL)) {
                (void)gateway_failure(gateway, connection->peer, 400,
                    "invalid or unsupported session for selected service");
            } else if (strcmp(service->adapter, "opencode-local") == 0) {
                (void)opencode_auto_chat(gateway, connection->peer, root,
                    service, model);
            } else if (strcmp(service->adapter, "codex-local") == 0) {
                (void)codex_chat(gateway, connection->peer, root, service, model);
            } else if (strcmp(service->adapter, "opencode") == 0) {
                (void)opencode_chat(gateway, connection->peer, root, service, model);
            } else if (strcmp(service->adapter, "openai") == 0 ||
                strcmp(service->adapter, "opencode-cloud") == 0) {
                (void)openai_chat(gateway, connection->peer, root, service, model);
            } else {
                (void)native_chat(gateway, connection->peer, root, service, model);
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
    if (host[0] == '[' && host[separator - address - 1] == ']') {
        size_t length = strlen(host);

        memmove(host, host + 1, length - 2U);
        host[length - 2U] = '\0';
    }
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
    codex_disconnect(gateway);
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
    return kc_util_pid_alive(pid);
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
