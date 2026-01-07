/* SPDX-License-Identifier: Zlib */
/* macOS-specific utilities for Zathura */

#ifndef OSX_UTILS_H
#define OSX_UTILS_H

#include <gtk/gtk.h>

/* Disable automatic window tabbing for the entire application */
void osx_disable_automatic_tabbing(void);

/* Disable tabbing for a specific GTK window */
void osx_disable_window_tabbing(GtkWidget* gtk_window);

#endif /* OSX_UTILS_H */
