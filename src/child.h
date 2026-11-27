/**
 * child.h - Local provider process transport.
 * Summary: Owns a child process and its standard input and output.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef KC_KAIMAN_CHILD_H
#define KC_KAIMAN_CHILD_H

#include "util.h"

typedef struct kc_child kc_child_t;

/**
 * Start an installed executable with fixed arguments and private pipes.
 * @param out Receives an owned child process.
 * @param argv Executable name and NULL-terminated arguments.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_child_open(kc_child_t **out, char *const argv[]);

/**
 * Write a complete line to the child input stream.
 * @param child Open child.
 * @param line Text to send, without a newline.
 * @param deadline Monotonic deadline.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_child_write(kc_child_t *child, const char *line, double deadline);

/**
 * Read a line or report the end of the child output stream.
 * @param child Open child.
 * @param deadline Monotonic deadline.
 * @param out Receives an owned line.
 * @return KC_KAIMAN_OK, KC_KAIMAN_NOT_FOUND at EOF, or an error status.
 */
int kc_child_line(kc_child_t *child, double deadline, char **out);

/**
 * Wait for the child to exit successfully.
 * @param child Open child.
 * @param deadline Monotonic deadline.
 * @return KC_KAIMAN_OK or an error status.
 */
int kc_child_wait(kc_child_t *child, double deadline);

/**
 * Close pipes and stop an owned child if it is still running.
 * @param child Owned child, or NULL.
 * @return void
 */
void kc_child_close(kc_child_t *child);

#endif
