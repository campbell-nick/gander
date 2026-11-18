/* webview.c - the GTK backend (and the no-op stubs) for the embedded webview.
 *
 * By default this file is inert: gander stays libc-only and opens the UI in a
 * normal browser. Build with `make WEBVIEW=1` to compile a real backend - this
 * file's GTK/WebKitGTK implementation, or the Qt one in webview_qt.cpp - which
 * runs the toolkit's own event loop (see webview_run) and calls back into the
 * server for socket I/O. The window is created lazily, so a headless run never
 * touches a toolkit.
 */
#include "gander.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(GANDER_WEBVIEW_GTK)

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

/* How often the server gets a turn while GTK owns the event loop. */
#define WEBVIEW_TICK_MS 4

struct webview_win {
  GtkWidget *window;
  GtkWidget *view;
  int alive;
  int (*tick)(void *);
  void *ud;
  guint tick_id;
};

/* The window manager asked to close the window (or we destroyed it). GTK
 * finalises the widget right after this signal, so drop our pointers to it: the
 * main loop then stops, and webview_destroy knows there is nothing left. */
static void on_destroy(GtkWidget *widget, gpointer data) {
  struct webview_win *wv = data;
  if (wv->window == widget)
    wv->window = NULL;
  wv->view = NULL;
  wv->alive = 0;
  gtk_main_quit();
}

/* Periodic server turn, so sockets are serviced while GTK runs the loop. */
static gboolean on_tick(gpointer data) {
  struct webview_win *wv = data;
  if (!wv->alive || (wv->tick && wv->tick(wv->ud))) {
    wv->tick_id = 0;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

int webview_available(void) { return 1; }

struct webview_win *webview_open(const char *url, const char *title,
                                 const char *icon, int w, int h) {
  /* gtk_init_check, not gtk_init: a missing display should be a clean "no",
   * not a call that terminates the process. */
  if (!gtk_init_check(NULL, NULL)) {
    fprintf(stderr, "gander: cannot open the embedded webview (no display)\n");
    return NULL;
  }

  struct webview_win *wv = xmalloc(sizeof *wv);
  memset(wv, 0, sizeof *wv);
  wv->alive = 1;

  wv->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(wv->window), title ? title : "gander");
  gtk_window_set_default_size(GTK_WINDOW(wv->window), w > 0 ? w : 1100,
                              h > 0 ? h : 800);
  if (icon && icon[0]) {
    /* The bundled logo is an SVG, so this needs the gdk-pixbuf SVG loader; if
     * it or the file is missing, keep the toolkit's default icon. */
    GError *err = NULL;
    if (!gtk_window_set_icon_from_file(GTK_WINDOW(wv->window), icon, &err)) {
      if (err)
        g_error_free(err);
    }
  }
  g_signal_connect(wv->window, "destroy", G_CALLBACK(on_destroy), wv);

  wv->view = webkit_web_view_new();
  gtk_container_add(GTK_CONTAINER(wv->window), wv->view);
  webkit_web_view_load_uri(WEBKIT_WEB_VIEW(wv->view), url);
  gtk_widget_show_all(wv->window);

  /* Flush once so the window is on screen before the main loop takes over. */
  while (gtk_events_pending())
    gtk_main_iteration();
  return wv;
}

void webview_run(struct webview_win *wv, int (*tick)(void *), void *ud) {
  if (!wv || !wv->alive)
    return;
  wv->tick = tick;
  wv->ud = ud;
  /* GTK owns the loop, so input and rendering are handled the moment they
   * arrive; the timeout just gives the server a regular turn for its sockets. */
  wv->tick_id = g_timeout_add(WEBVIEW_TICK_MS, on_tick, wv);
  gtk_main();
  if (wv->tick_id) {
    g_source_remove(wv->tick_id);
    wv->tick_id = 0;
  }
}

void webview_destroy(struct webview_win *wv) {
  if (!wv)
    return;
  if (wv->window) {
    /* Detach the handler first: we are tearing down on purpose, and the
     * "destroy" signal would otherwise be harmless but noisy. */
    g_signal_handlers_disconnect_by_func(wv->window, G_CALLBACK(on_destroy),
                                         wv);
    gtk_widget_destroy(wv->window);
    wv->window = NULL;
  }
  free(wv);
}

#elif defined(GANDER_WEBVIEW_QT)

/* The Qt WebEngine backend lives in webview_qt.cpp (C++); nothing to define
 * here. */

#else /* no backend - inert stubs, so the default build stays libc-only */

int webview_available(void) { return 0; }

struct webview_win *webview_open(const char *url, const char *title,
                                 const char *icon, int w, int h) {
  (void)url;
  (void)title;
  (void)icon;
  (void)w;
  (void)h;
  return NULL;
}

void webview_run(struct webview_win *wv, int (*tick)(void *), void *ud) {
  (void)wv;
  (void)tick;
  (void)ud;
}

void webview_destroy(struct webview_win *wv) { (void)wv; }

#endif /* backend */
