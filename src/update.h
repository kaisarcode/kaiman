/**
 * update.h - Desktop software updates.
 * Summary: Internal release discovery and staged installer handoff.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef KC_KAIMAN_UPDATE_H
#define KC_KAIMAN_UPDATE_H

#include <stddef.h>
#include <stdatomic.h>

typedef struct {
    char version[64];
    char url[512];
    char digest[65];
    char notes[8193];
    size_t size;
    atomic_size_t received;
    unsigned long long installer_pid;
    const char *error;
} kc_update_t;

/**
 * Compare numeric release versions, with an optional v prefix.
 * @param left First version.
 * @param right Second version.
 * @return 1 when left is newer, otherwise 0.
 */
int kc_update_newer(const char *left, const char *right);

/**
 * Resolve the registered installation for the current executable.
 * @return Owned directory, or NULL when running a portable build.
 */
char *kc_update_installed(void);

/**
 * Discover the highest compatible release containing a verified asset.
 * @param update Receives release metadata.
 * @return 1 when available, 0 when current, -1 on failure.
 */
int kc_update_check(kc_update_t *update);

/**
 * Download, verify and extract the selected release, then launch its installer.
 * @param update Selected release and progress destination.
 * @return 1 when handed off, otherwise 0.
 */
int kc_update_download(kc_update_t *update);

/**
 * Resolve the private update staging directory.
 * @return Owned path, or NULL on failure.
 */
char *kc_update_directory(void);

/**
 * Join an update directory and relative filename.
 * @param directory Base directory.
 * @param name Relative filename.
 * @return Owned path, or NULL on failure.
 */
char *kc_update_path(const char *directory, const char *name);

/**
 * Launch an executable with a single fixed argument.
 * @param executable Absolute executable path.
 * @param argument Argument passed to the executable.
 * @return 1 on success, otherwise 0.
 */
int kc_update_launch(const char *executable, const char *argument);

/**
 * Check whether the staged installer exited while the GUI is running.
 * @param update Active update context.
 * @return 1 when exited, otherwise 0.
 */
int kc_update_exited(kc_update_t *update);

/**
 * Verify the current installer belongs to the staging directory.
 * @return 1 when running from the staged release.
 */
int kc_update_staged(void);

/**
 * Remove the owned staging tree without following symbolic links.
 * @param directory Staging directory.
 * @return 1 on success, otherwise 0.
 */
int kc_update_remove(const char *directory);

/**
 * Collect the result of a completed update and clean its staging tree.
 * @return Owned message, or NULL when no completed update exists.
 */
char *kc_update_collect(void);

#endif
