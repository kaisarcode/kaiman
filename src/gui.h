/**
 * gui.h - Graphical frontend for kaiman.
 * Summary: Declares the native GUI command entry point.
 *
 * Author:  KaisarCode
 * Website: https://kaisarcode.com
 * License: https://www.gnu.org/licenses/lgpl-3.0.html
 */

#ifndef KC_KAIMAN_GUI_H
#define KC_KAIMAN_GUI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Run the native service manager GUI.
 * @return 0 on success, 1 on failure.
 */
int kc_kaiman_gui_run(void);

#ifdef __cplusplus
}
#endif

#endif
