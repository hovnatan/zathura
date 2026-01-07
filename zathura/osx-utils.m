/* SPDX-License-Identifier: Zlib */
/* macOS-specific utilities for Zathura */

#import <AppKit/AppKit.h>
#include <gtk/gtk.h>
#include <gdk/quartz/gdkquartz-cocoa-access.h>

/* Disable automatic window tabbing for the entire application */
void osx_disable_automatic_tabbing(void) {
    if (@available(macOS 10.12, *)) {
        [NSWindow setAllowsAutomaticWindowTabbing:NO];
    }
}

/* Disable tabbing for a specific GTK window */
void osx_disable_window_tabbing(GtkWidget* gtk_window) {
    if (gtk_window == NULL) {
        return;
    }

    GdkWindow* gdk_window = gtk_widget_get_window(gtk_window);
    if (gdk_window == NULL) {
        return;
    }

    NSWindow* nswindow = gdk_quartz_window_get_nswindow(gdk_window);
    if (nswindow == NULL) {
        return;
    }

    if (@available(macOS 10.12, *)) {
        nswindow.tabbingMode = NSWindowTabbingModeDisallowed;
    }
}
