/**
 * kaiman.c - Command line frontend for kaiman.
 * Summary: Manages saved AI services and the gateway process.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#include "libkaiman.h"

#ifdef KC_KAIMAN_GUI
#include "gui.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Print command usage.
 * @return void
 */
static void usage(void) {
    printf("kaiman - unified AI gateway and service manager\n\n");
    printf("Usage:\n");
    printf("  kaiman <command> [arguments]\n\n");
    printf("Commands:\n");
    printf("  add NAME [key=value ...]        Add a configured service\n");
    printf("  edit NAME [key=value ...]       Edit a configured service\n");
    printf("  remove NAME                     Remove a configured service\n");
    printf("  list                            List configured services\n");
    printf("  conf                            Show public gateway configuration\n");
    printf("  config                          Alias for conf\n");
    printf("  set KEY=VALUE [KEY=VALUE ...]   Change saved gateway configuration\n");
    printf("  start                           Start the gateway process\n");
    printf("  stop                            Stop the gateway process\n");
    printf("  status                          Show gateway process status\n");
#ifdef KC_KAIMAN_GUI
    printf("  gui                             Open the graphical manager\n");
#endif
    printf("\nOptions:\n");
    printf("  -h, --help                      Show this help\n");
    printf("  -v, --version                   Show version\n\n");
    printf("Service keys: display-name, adapter, url, env,\n");
    printf("  token-command, scheme, user, timeout\n");
}

/**
 * Report the last detailed library error, or the status description.
 * @param kaiman Open manager, or NULL.
 * @param status Status returned by the library.
 * @return void
 */
static void report_error(const kc_kaiman_t *kaiman, int status) {
    const char *detail = kc_kaiman_error(kaiman);

    if (detail != NULL) fprintf(stderr, "kaiman: %s\n", detail);
    else fprintf(stderr, "kaiman: %s\n", kc_kaiman_strerror(status));
}

/**
 * Print one managed service in a stable, parseable form.
 * @param service Managed service.
 * @return void
 */
static void print_service(const kc_kaiman_service_t *service) {
    printf("%s\n", kc_kaiman_service_effective(service, "name"));
    printf("  display-name: %s\n",
        kc_kaiman_service_effective(service, "display-name"));
    printf("  adapter: %s\n", kc_kaiman_service_effective(service, "adapter"));
    printf("  url: %s\n", kc_kaiman_service_effective(service, "url"));
    printf("  env: %s\n", kc_kaiman_service_effective(service, "env"));
    printf("  token-command: %s\n",
        kc_kaiman_service_effective(service, "token-command"));
    printf("  scheme: %s\n", kc_kaiman_service_effective(service, "scheme"));
    printf("  user: %s\n", kc_kaiman_service_effective(service, "user"));
    printf("  timeout: %d\n", kc_kaiman_service_timeout(service));
}

/**
 * Apply key=value arguments to one managed service.
 * @param kaiman Open manager.
 * @param service Managed service.
 * @param arguments Argument array.
 * @param count Number of arguments.
 * @return 0 on success, 1 on failure.
 */
static int apply_fields(kc_kaiman_t *kaiman, kc_kaiman_service_t *service,
    char **arguments, int count) {
    int index;

    for (index = 0; index < count; index++) {
        char *separator = strchr(arguments[index], '=');
        int status;
        if (separator == NULL) {
            fprintf(stderr, "kaiman: expected key=value: %s\n",
                arguments[index]);
            return 1;
        }
        *separator = '\0';
        status = kc_kaiman_service_set(service, arguments[index],
            separator + 1);
        if (status != KC_KAIMAN_OK) {
            report_error(kaiman, status);
            return 1;
        }
    }
    return 0;
}

/**
 * Run the add command.
 * @param arguments Arguments after the command.
 * @param count Number of arguments.
 * @return 0 on success, 1 on failure.
 */
static int add_command(char **arguments, int count) {
    kc_kaiman_t *kaiman = NULL;
    kc_kaiman_service_t *service = NULL;
    kc_kaiman_descriptor_t descriptor;
    int status;
    int result = 1;

    if (count < 1) {
        usage();
        return 1;
    }
    status = kc_kaiman_open(&kaiman, NULL);
    if (status != KC_KAIMAN_OK) {
        report_error(NULL, status);
        return 1;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.name = arguments[0];
    status = kc_kaiman_add_service(kaiman, &descriptor, &service);
    if (status != KC_KAIMAN_OK) {
        report_error(kaiman, status);
        goto done;
    }
    if (apply_fields(kaiman, service, arguments + 1, count - 1) == 0) {
        result = 0;
    }

done:
    kc_kaiman_close(kaiman);
    return result;
}

/**
 * Run the edit command.
 * @param arguments Arguments after the command.
 * @param count Number of arguments.
 * @return 0 on success, 1 on failure.
 */
static int edit_command(char **arguments, int count) {
    kc_kaiman_t *kaiman = NULL;
    kc_kaiman_service_t *service = NULL;
    int status;
    int result = 1;

    if (count < 1) {
        usage();
        return 1;
    }
    status = kc_kaiman_open(&kaiman, NULL);
    if (status != KC_KAIMAN_OK) {
        report_error(NULL, status);
        return 1;
    }
    status = kc_kaiman_get_service(kaiman, arguments[0], &service);
    if (status != KC_KAIMAN_OK) {
        report_error(kaiman, status);
        goto done;
    }
    if (apply_fields(kaiman, service, arguments + 1, count - 1) == 0) {
        result = 0;
    }

done:
    kc_kaiman_close(kaiman);
    return result;
}

/**
 * Run the remove command.
 * @param arguments Arguments after the command.
 * @param count Number of arguments.
 * @return 0 on success, 1 on failure.
 */
static int remove_command(char **arguments, int count) {
    kc_kaiman_t *kaiman = NULL;
    kc_kaiman_service_t *service = NULL;
    int status;
    int result = 1;

    if (count != 1) {
        usage();
        return 1;
    }
    status = kc_kaiman_open(&kaiman, NULL);
    if (status != KC_KAIMAN_OK) {
        report_error(NULL, status);
        return 1;
    }
    status = kc_kaiman_get_service(kaiman, arguments[0], &service);
    if (status != KC_KAIMAN_OK) {
        report_error(kaiman, status);
        goto done;
    }
    status = kc_kaiman_service_remove(service);
    if (status != KC_KAIMAN_OK) {
        report_error(kaiman, status);
        goto done;
    }
    result = 0;

done:
    kc_kaiman_close(kaiman);
    return result;
}

/**
 * Run the list command.
 * @return 0 on success, 1 on failure.
 */
static int list_command(void) {
    kc_kaiman_t *kaiman = NULL;
    kc_kaiman_service_t **services = NULL;
    size_t count = 0U;
    size_t index;
    int status;
    int result = 1;

    status = kc_kaiman_open(&kaiman, NULL);
    if (status != KC_KAIMAN_OK) {
        report_error(NULL, status);
        return 1;
    }
    status = kc_kaiman_get_services(kaiman, &services, &count);
    if (status != KC_KAIMAN_OK) {
        report_error(kaiman, status);
        goto done;
    }
    for (index = 0U; index < count; index++) {
        print_service(services[index]);
    }
    result = 0;

done:
    kc_kaiman_close(kaiman);
    return result;
}

/**
 * Show the saved gateway configuration.
 * @return 0 on success, 1 on failure.
 */
static int config_command(void) {
    kc_kaiman_t *kaiman = NULL;
    int status;

    status = kc_kaiman_open(&kaiman, NULL);
    if (status != KC_KAIMAN_OK) {
        report_error(NULL, status);
        return 1;
    }
    printf("address: %s\n", kc_kaiman_address(kaiman));
    printf("path: %s\n", kc_kaiman_path(kaiman));
    kc_kaiman_close(kaiman);
    return 0;
}

/**
 * Resolve one CLI address value against the saved address.
 * @param kaiman Open manager with the current address.
 * @param value Address supplied by the user.
 * @return Owned address, or NULL on invalid input or allocation failure.
 */
static char *resolve_address(const kc_kaiman_t *kaiman, const char *value) {
    const char *current;
    const char *port;
    size_t length;
    char *resolved;

    if (value == NULL || *value == '\0') return NULL;
    if (strchr(value, ':') != NULL) {
        length = strlen(value) + 1U;
        resolved = (char *)malloc(length);
        if (resolved == NULL) return NULL;
        memcpy(resolved, value, length);
        return resolved;
    }
    current = kc_kaiman_address(kaiman);
    if (current == NULL) return NULL;
    port = strrchr(current, ':');
    if (port == NULL) return NULL;
    length = strlen(value) + strlen(port) + 1U;
    resolved = (char *)malloc(length);
    if (resolved == NULL) return NULL;
    snprintf(resolved, length, "%s%s", value, port);
    return resolved;
}

/**
 * Resolve one CLI port value against the saved address.
 * @param kaiman Open manager with the current address.
 * @param value Port supplied by the user.
 * @return Owned address, or NULL on invalid input or allocation failure.
 */
static char *resolve_port(const kc_kaiman_t *kaiman, const char *value) {
    const char *current;
    const char *separator;
    char *end;
    size_t host_length;
    size_t length;
    long port;
    char *resolved;

    if (value == NULL || *value == '\0') return NULL;
    port = strtol(value, &end, 10);
    if (end == value || *end != '\0' || port < 1 || port > 65535) {
        return NULL;
    }
    current = kc_kaiman_address(kaiman);
    if (current == NULL) return NULL;
    separator = strrchr(current, ':');
    if (separator == NULL || separator == current) return NULL;
    host_length = (size_t)(separator - current);
    length = strlen(current) + 8U;
    resolved = (char *)malloc(length);
    if (resolved == NULL) return NULL;
    memcpy(resolved, current, host_length);
    resolved[host_length] = ':';
    snprintf(resolved + host_length + 1U, length - host_length - 1U, "%ld",
        port);
    return resolved;
}

/**
 * Apply key=value changes to the saved gateway configuration.
 * @param arguments Arguments after the command.
 * @param count Number of arguments.
 * @return 0 on success, 1 on failure.
 */
static int set_command(char **arguments, int count) {
    kc_kaiman_t *kaiman = NULL;
    char *resolved = NULL;
    char *separator;
    int index;
    int status;
    int result = 1;

    if (count < 1) {
        usage();
        return 1;
    }
    status = kc_kaiman_open(&kaiman, NULL);
    if (status != KC_KAIMAN_OK) {
        report_error(NULL, status);
        return 1;
    }
    for (index = 0; index < count; index++) {
        separator = strchr(arguments[index], '=');
        if (separator == NULL) {
            fprintf(stderr, "kaiman: expected key=value: %s\n",
                arguments[index]);
            goto done;
        }
        *separator = '\0';
        free(resolved);
        resolved = NULL;
        if (strcmp(arguments[index], "address") == 0) {
            resolved = resolve_address(kaiman, separator + 1);
        } else if (strcmp(arguments[index], "port") == 0) {
            resolved = resolve_port(kaiman, separator + 1);
        } else {
            fprintf(stderr, "kaiman: unknown setting: %s\n",
                arguments[index]);
            goto done;
        }
        if (resolved == NULL) {
            fprintf(stderr, "kaiman: invalid value for %s\n",
                arguments[index]);
            goto done;
        }
        status = kc_kaiman_listen(kaiman, resolved);
        if (status != KC_KAIMAN_OK) {
            report_error(kaiman, status);
            goto done;
        }
    }
    printf("address: %s\n", kc_kaiman_address(kaiman));
    result = 0;

done:
    free(resolved);
    kc_kaiman_close(kaiman);
    return result;
}

/**
 * Run the gateway in the foreground.
 * @return 0 on success, 1 on failure.
 */
static int serve_command(void) {
    kc_kaiman_t *kaiman = NULL;
    kc_kaiman_gateway_t *gateway = NULL;
    int status;
    int result = 1;

    status = kc_kaiman_open(&kaiman, NULL);
    if (status != KC_KAIMAN_OK) {
        report_error(NULL, status);
        return 1;
    }
    status = kc_kaiman_gateway_open(&gateway, kaiman);
    if (status != KC_KAIMAN_OK) {
        report_error(kaiman, status);
        if (kc_kaiman_gateway_error(gateway) != NULL) {
            fprintf(stderr, "kaiman: %s\n", kc_kaiman_gateway_error(gateway));
        }
        goto done;
    }
    status = kc_kaiman_gateway_run(gateway);
    if (status != KC_KAIMAN_OK) {
        report_error(NULL, status);
        goto done;
    }
    result = 0;

done:
    kc_kaiman_gateway_close(gateway);
    kc_kaiman_close(kaiman);
    return result;
}

/**
 * Run the command line frontend.
 * @param argc Argument count.
 * @param argv Argument values.
 * @return 0 on success, 1 on failure.
 */
int main(int argc, char **argv) {
    char *description = NULL;
    int status;

    if (argc < 2) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
        printf("%s\n", kc_kaiman_version());
        return 0;
    }
    if (strcmp(argv[1], "--internal-serve") == 0) {
        return serve_command();
    }
    if (strcmp(argv[1], "add") == 0) {
        return add_command(argv + 2, argc - 2);
    }
    if (strcmp(argv[1], "edit") == 0) {
        return edit_command(argv + 2, argc - 2);
    }
    if (strcmp(argv[1], "remove") == 0) {
        return remove_command(argv + 2, argc - 2);
    }
    if (strcmp(argv[1], "list") == 0 && argc == 2) {
        return list_command();
    }
    if ((strcmp(argv[1], "conf") == 0 || strcmp(argv[1], "config") == 0) &&
        argc == 2) {
        return config_command();
    }
    if (strcmp(argv[1], "set") == 0) {
        return set_command(argv + 2, argc - 2);
    }
    if (strcmp(argv[1], "start") == 0 && argc == 2) {
        status = kc_kaiman_start(NULL);
        if (status != KC_KAIMAN_OK) {
            report_error(NULL, status);
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "stop") == 0 && argc == 2) {
        status = kc_kaiman_stop();
        if (status != KC_KAIMAN_OK) {
            report_error(NULL, status);
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "status") == 0 && argc == 2) {
        status = kc_kaiman_status(&description);
        if (status != KC_KAIMAN_OK) {
            report_error(NULL, status);
            return 1;
        }
        printf("%s", description);
        kc_kaiman_free(description);
        return 0;
    }
    if (strcmp(argv[1], "gui") == 0) {
        if (argc != 2) {
            usage();
            return 1;
        }
#ifdef KC_KAIMAN_GUI
        return kc_kaiman_gui_run();
#else
        fprintf(stderr, "kaiman: GUI is not available in this build\n");
        return 1;
#endif
    }
    usage();
    return 1;
}
