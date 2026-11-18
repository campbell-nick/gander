/* webview_qt.cpp - the Qt WebEngine backend for the embedded webview.
 *
 * Built only when the webview selects Qt (`make WEBVIEW=qt`, or `make WEBVIEW=1`
 * when WebKitGTK is absent). The shared C API is declared in gander.h; the GTK
 * backend and the no-op stubs live in webview.c. Like the GTK one, this is
 * driven from the server's poll loop (see webview_pump) rather than a Qt event
 * loop of its own.
 *
 * Qt is C++, so this is the one C++ translation unit in gander; the default
 * build never sees it.
 */
#ifdef GANDER_WEBVIEW_QT

#include "gander.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <QApplication>
#include <QCloseEvent>
#include <QString>
#include <QUrl>
#include <QWebEngineView>

namespace {

/* A QWebEngineView that notices when its window is closed. closeEvent is a
 * plain virtual, so this needs no Q_OBJECT and no moc. */
class GanderView : public QWebEngineView {
 public:
  explicit GanderView(int *alive) : alive_(alive) {}

 protected:
  void closeEvent(QCloseEvent *event) override {
    *alive_ = 0;
    QWebEngineView::closeEvent(event);
  }

 private:
  int *alive_;
};

/* Qt WebEngine needs a QApplication before any view exists, and QApplication
 * aborts the process (rather than failing softly) when there is no display, so
 * check for one first. Created once and never torn down. */
QApplication *ensure_app() {
  static QApplication *app = nullptr;
  if (app)
    return app;
  if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
    fprintf(stderr, "gander: cannot open the embedded webview (no display)\n");
    return nullptr;
  }
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  static char name[] = "gander";
  static char *argv[] = {name, nullptr};
  int argc = 1;
  app = new QApplication(argc, argv);
  return app;
}

}  // namespace

struct webview_win {
  QApplication *app;
  GanderView *view;
  int alive;
};

int webview_available(void) { return 1; }

struct webview_win *webview_open(const char *url, const char *title, int w,
                                 int h) {
  QApplication *app = ensure_app();
  if (!app)
    return nullptr;

  struct webview_win *wv =
      static_cast<struct webview_win *>(xmalloc(sizeof *wv));
  memset(wv, 0, sizeof *wv);
  wv->app = app;
  wv->alive = 1;

  GanderView *view = new GanderView(&wv->alive);
  wv->view = view;
  view->setWindowTitle(title ? QString::fromUtf8(title)
                             : QStringLiteral("gander"));
  view->resize(w > 0 ? w : 1100, h > 0 ? h : 800);
  view->load(QUrl(QString::fromUtf8(url)));
  view->show();

  /* Flush once so the window is on screen before the server loop takes over. */
  app->processEvents();
  return wv;
}

int webview_pump(struct webview_win *wv) {
  if (!wv || !wv->alive)
    return 0;
  if (wv->app)
    wv->app->processEvents();
  return wv->alive;
}

void webview_destroy(struct webview_win *wv) {
  if (!wv)
    return;
  if (wv->view) {
    delete wv->view;
    wv->view = nullptr;
  }
  free(wv);
}

#endif /* GANDER_WEBVIEW_QT */
