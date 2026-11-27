/**
 * child.c - Local provider process transport.
 * Summary: Runs installed desktop agents without shell interpolation.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "child.h"
#include "libkaiman.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if !defined(_WIN32) && !defined(__ANDROID__) && \
    !(defined(__APPLE__) && TARGET_OS_IPHONE)
#define KC_CHILD_POSIX 1
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

struct kc_child {
    kc_buffer_t *pending;
    int exited;
    int result;
#ifdef _WIN32
    HANDLE process;
    HANDLE input;
    HANDLE output;
#elif defined(KC_CHILD_POSIX)
    pid_t pid;
    int input;
    int output;
#endif
};

#ifdef _WIN32
/**
 * Start a native Windows executable with redirected standard streams.
 * @param child Allocated child.
 * @param argv Executable name and NULL-terminated arguments.
 * @return KC_KAIMAN_OK or an error status.
 */
static int child_start(kc_child_t *child, char *const argv[]) {
    SECURITY_ATTRIBUTES security;
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    HANDLE input = NULL;
    HANDLE output = NULL;
    HANDLE errors = INVALID_HANDLE_VALUE;
    kc_buffer_t *command = NULL;
    char executable[MAX_PATH];
    DWORD length;
    size_t index;
    int status = KC_KAIMAN_EIO;

    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    length = SearchPathA(NULL, argv[0], ".exe", MAX_PATH, executable, NULL);
    if (length == 0U && strcmp(argv[0], "opencode") == 0) {
        const char *home = getenv("USERPROFILE");

        if (home != NULL && strlen(home) + 30U < MAX_PATH) {
            strcpy(executable, home);
            strcat(executable, "\\.opencode\\bin\\opencode.exe");
            if (GetFileAttributesA(executable) != INVALID_FILE_ATTRIBUTES) {
                length = (DWORD)strlen(executable);
            }
        }
    }
    if (length == 0U || length >= MAX_PATH) return KC_KAIMAN_EIO;
    if (!CreatePipe(&input, &child->input, &security, 0) ||
        !CreatePipe(&child->output, &output, &security, 0) ||
        !SetHandleInformation(child->input, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(child->output, HANDLE_FLAG_INHERIT, 0)) goto done;
    errors = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ |
        FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, NULL);
    if (errors == INVALID_HANDLE_VALUE) goto done;
    status = kc_buffer_open(&command);
    if (status == KC_KAIMAN_OK) {
        status = kc_buffer_format(command, "\"%s\"", executable);
    }
    for (index = 1U; status == KC_KAIMAN_OK && argv[index] != NULL; index++) {
        if (strpbrk(argv[index], " \t\"\\") != NULL) {
            status = KC_KAIMAN_EINVAL;
        } else {
            status = kc_buffer_format(command, " %s", argv[index]);
        }
    }
    if (status != KC_KAIMAN_OK) goto done;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = errors;
    memset(&process, 0, sizeof(process));
    if (!CreateProcessA(executable, command->data, NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        status = KC_KAIMAN_EIO;
    } else {
        child->process = process.hProcess;
        CloseHandle(process.hThread);
    }

done:
    if (input != NULL) CloseHandle(input);
    if (output != NULL) CloseHandle(output);
    if (errors != INVALID_HANDLE_VALUE) CloseHandle(errors);
    kc_buffer_close(command);
    return status;
}
#elif defined(KC_CHILD_POSIX)
/**
 * Start a POSIX executable with redirected standard streams.
 * @param child Allocated child.
 * @param argv Executable name and NULL-terminated arguments.
 * @return KC_KAIMAN_OK or an error status.
 */
static int child_start(kc_child_t *child, char *const argv[]) {
    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    kc_buffer_t *path = NULL;
    int ready = 0;
    int status = KC_KAIMAN_EIO;
    int rc;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, input) != 0 ||
        pipe(output) != 0) goto done;
    if (fcntl(input[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(input[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(output[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(output[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(input[0], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(output[0], F_SETFL, O_NONBLOCK) < 0) goto done;
#ifdef SO_NOSIGPIPE
    {
        int enabled = 1;

        if (setsockopt(input[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled,
            sizeof(enabled)) != 0) goto done;
    }
#endif
    if (posix_spawn_file_actions_init(&actions) != 0) goto done;
    ready = 1;
    if (posix_spawn_file_actions_adddup2(&actions, input[1], STDIN_FILENO) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
            O_WRONLY, 0) != 0) goto done;
    rc = posix_spawnp(&child->pid, argv[0], &actions, NULL, argv, environ);
    if (rc == ENOENT && getenv("HOME") != NULL) {
        const char *suffix = strcmp(argv[0], "opencode") == 0 ?
            "/.opencode/bin/opencode" : "/.local/bin/codex";

        if (kc_buffer_open(&path) == KC_KAIMAN_OK &&
            kc_buffer_format(path, "%s%s", getenv("HOME"), suffix) ==
                KC_KAIMAN_OK) {
            rc = posix_spawn(&child->pid, kc_buffer_value(path), &actions,
                NULL, argv, environ);
        }
    }
    if (rc == 0) {
        child->input = input[0];
        child->output = output[0];
        input[0] = -1;
        output[0] = -1;
        status = KC_KAIMAN_OK;
    } else {
        child->pid = 0;
    }

done:
    if (ready) posix_spawn_file_actions_destroy(&actions);
    if (input[0] >= 0) close(input[0]);
    if (input[1] >= 0) close(input[1]);
    if (output[0] >= 0) close(output[0]);
    if (output[1] >= 0) close(output[1]);
    kc_buffer_close(path);
    return status;
}
#endif

/**
 * Start an installed executable with fixed arguments and private pipes.
 * @param out Receives an owned child process.
 * @param argv Executable name and NULL-terminated arguments.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_child_open(kc_child_t **out, char *const argv[]) {
    kc_child_t *child;
    int status;

    if (out == NULL || argv == NULL || argv[0] == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    child = (kc_child_t *)calloc(1, sizeof(*child));
    if (child == NULL) return KC_KAIMAN_ENOMEM;
#ifdef KC_CHILD_POSIX
    child->input = -1;
    child->output = -1;
#endif
    status = kc_buffer_open(&child->pending);
    if (status == KC_KAIMAN_OK) {
#if defined(_WIN32) || defined(KC_CHILD_POSIX)
        status = child_start(child, argv);
#else
        status = KC_KAIMAN_EPROTO;
#endif
    }
    if (status == KC_KAIMAN_OK) *out = child;
    else kc_child_close(child);
    return status;
}

#ifdef KC_CHILD_POSIX
/**
 * Wait for a pipe or socket to become readable or writable.
 * @param fd Descriptor.
 * @param events Poll event mask.
 * @param deadline Monotonic deadline.
 * @return KC_KAIMAN_OK or an error status.
 */
static int child_poll(int fd, short events, double deadline) {
    struct pollfd item;
    int rc;

    item.fd = fd;
    item.events = events;
    while (kc_util_seconds() < deadline) {
        item.revents = 0;
        rc = poll(&item, 1, 50);
        if (rc > 0) return KC_KAIMAN_OK;
        if (rc < 0 && errno != EINTR) return KC_KAIMAN_EIO;
    }
    return KC_KAIMAN_ETIMEOUT;
}
#endif

/**
 * Write bytes to the child input stream.
 * @param child Open child.
 * @param data Bytes to send.
 * @param size Byte count.
 * @param deadline Monotonic deadline.
 * @return KC_KAIMAN_OK or an error status.
 */
static int child_send(kc_child_t *child, const char *data, size_t size,
    double deadline) {
#if defined(_WIN32) || defined(KC_CHILD_POSIX)
    while (size != 0U && kc_util_seconds() < deadline) {
#ifdef _WIN32
        DWORD written;

        if (!WriteFile(child->input, data, size > 4096U ? 4096U : (DWORD)size,
            &written, NULL) || written == 0U) return KC_KAIMAN_EIO;
#else
        ssize_t written;
        int status = child_poll(child->input, POLLOUT, deadline);

        if (status != KC_KAIMAN_OK) return status;
#ifdef MSG_NOSIGNAL
        written = send(child->input, data, size, MSG_NOSIGNAL);
#else
        written = send(child->input, data, size, 0);
#endif
        if (written < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (written <= 0) return KC_KAIMAN_EIO;
#endif
        data += (size_t)written;
        size -= (size_t)written;
    }
    return size == 0U ? KC_KAIMAN_OK : KC_KAIMAN_ETIMEOUT;
#else
    (void)child;
    (void)data;
    (void)size;
    (void)deadline;
    return KC_KAIMAN_EPROTO;
#endif
}

/**
 * Write a complete line to the child input stream.
 * @param child Open child.
 * @param line Text to send, without a newline.
 * @param deadline Monotonic deadline.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_child_write(kc_child_t *child, const char *line, double deadline) {
    int status;

    if (child == NULL || line == NULL) return KC_KAIMAN_EINVAL;
    status = child_send(child, line, strlen(line), deadline);
    if (status == KC_KAIMAN_OK) status = child_send(child, "\n", 1U, deadline);
    return status;
}

/**
 * Read another chunk from the child output stream.
 * @param child Open child.
 * @param deadline Monotonic deadline.
 * @return KC_KAIMAN_OK, KC_KAIMAN_NOT_FOUND at EOF, or an error status.
 */
static int child_receive(kc_child_t *child, double deadline) {
#if defined(_WIN32) || defined(KC_CHILD_POSIX)
    char bytes[4096];

    while (kc_util_seconds() < deadline) {
#ifdef _WIN32
        DWORD available;
        DWORD received;

        if (!PeekNamedPipe(child->output, NULL, 0, NULL, &available, NULL)) {
            return GetLastError() == ERROR_BROKEN_PIPE ?
                KC_KAIMAN_NOT_FOUND : KC_KAIMAN_EIO;
        }
        if (available == 0U) {
            kc_util_sleep(10);
            continue;
        }
        if (!ReadFile(child->output, bytes,
            available > sizeof(bytes) ? (DWORD)sizeof(bytes) : available,
            &received, NULL)) return KC_KAIMAN_EIO;
#else
        ssize_t received;
        int status = child_poll(child->output, POLLIN, deadline);

        if (status != KC_KAIMAN_OK) return status;
        received = read(child->output, bytes, sizeof(bytes));
        if (received < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (received < 0) return KC_KAIMAN_EIO;
#endif
        if (received == 0) return KC_KAIMAN_NOT_FOUND;
        if (child->pending->size + (size_t)received > 16777216U) {
            return KC_KAIMAN_EFORMAT;
        }
        return kc_buffer_append(child->pending, bytes, (size_t)received);
    }
    return KC_KAIMAN_ETIMEOUT;
#else
    (void)child;
    (void)deadline;
    return KC_KAIMAN_EPROTO;
#endif
}

/**
 * Read a line or report the end of the child output stream.
 * @param child Open child.
 * @param deadline Monotonic deadline.
 * @param out Receives an owned line.
 * @return KC_KAIMAN_OK, KC_KAIMAN_NOT_FOUND at EOF, or an error status.
 */
int kc_child_line(kc_child_t *child, double deadline, char **out) {
    kc_buffer_t *pending;
    size_t size;
    size_t consumed;
    int status = KC_KAIMAN_OK;

    if (child == NULL || out == NULL) return KC_KAIMAN_EINVAL;
    *out = NULL;
    pending = child->pending;
    for (;;) {
        char *newline = pending->size != 0U ?
            memchr(pending->data, '\n', pending->size) : NULL;

        if (newline != NULL || status == KC_KAIMAN_NOT_FOUND) {
            if (pending->size == 0U) return KC_KAIMAN_NOT_FOUND;
            size = newline != NULL ?
                (size_t)(newline - pending->data) : pending->size;
            consumed = size + (newline != NULL ? 1U : 0U);
            if (size != 0U && pending->data[size - 1U] == '\r') size--;
            *out = (char *)malloc(size + 1U);
            if (*out == NULL) return KC_KAIMAN_ENOMEM;
            memcpy(*out, pending->data, size);
            (*out)[size] = '\0';
            memmove(pending->data, pending->data + consumed,
                pending->size - consumed);
            pending->size -= consumed;
            pending->data[pending->size] = '\0';
            return KC_KAIMAN_OK;
        }
        status = child_receive(child, deadline);
        if (status != KC_KAIMAN_OK && status != KC_KAIMAN_NOT_FOUND) {
            return status;
        }
    }
}

/**
 * Wait for the child to exit successfully.
 * @param child Open child.
 * @param deadline Monotonic deadline.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_child_wait(kc_child_t *child, double deadline) {
    if (child == NULL) return KC_KAIMAN_EINVAL;
#if defined(_WIN32) || defined(KC_CHILD_POSIX)
    while (!child->exited && kc_util_seconds() < deadline) {
#ifdef _WIN32
        DWORD code;

        if (WaitForSingleObject(child->process, 10) == WAIT_OBJECT_0) {
            child->exited = 1;
            child->result = GetExitCodeProcess(child->process, &code) &&
                code == 0U ? KC_KAIMAN_OK : KC_KAIMAN_EIO;
        }
#else
        int code;
        pid_t rc = waitpid(child->pid, &code, WNOHANG);

        if (rc == child->pid) {
            child->exited = 1;
            child->result = WIFEXITED(code) && WEXITSTATUS(code) == 0 ?
                KC_KAIMAN_OK : KC_KAIMAN_EIO;
        } else if (rc < 0 && errno != EINTR) {
            return KC_KAIMAN_EIO;
        } else {
            kc_util_sleep(10);
        }
#endif
    }
    return child->exited ? child->result : KC_KAIMAN_ETIMEOUT;
#else
    (void)deadline;
    return KC_KAIMAN_EPROTO;
#endif
}

/**
 * Close pipes and stop an owned child if it is still running.
 * @param child Owned child, or NULL.
 * @return void
 */
void kc_child_close(kc_child_t *child) {
    if (child == NULL) return;
#ifdef _WIN32
    if (child->input != NULL) CloseHandle(child->input);
    if (child->output != NULL) CloseHandle(child->output);
    if (child->process != NULL) {
        if (!child->exited &&
            WaitForSingleObject(child->process, 100) == WAIT_TIMEOUT) {
            TerminateProcess(child->process, 1);
            (void)WaitForSingleObject(child->process, 1000);
        }
        CloseHandle(child->process);
    }
#elif defined(KC_CHILD_POSIX)
    if (child->input >= 0) close(child->input);
    if (child->output >= 0) close(child->output);
    if (child->pid > 0 && !child->exited) {
        (void)kc_child_wait(child, kc_util_seconds() + 0.1);
        if (!child->exited) {
            (void)kill(child->pid, SIGKILL);
            while (waitpid(child->pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
#endif
    kc_buffer_close(child->pending);
    free(child);
}
