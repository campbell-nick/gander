/*
 * gander - an offline previewer for a local folder or git repository.
 *
 * A small, dependency-free C program that serves a bundled web UI on
 * 127.0.0.1 and opens it in the browser. Nothing ever leaves the machine.
 */
#ifndef GANDER_H
#define GANDER_H

#include <stddef.h>

#define GANDER_VERSION "0.1.0"

/* ---------------------------------------------------------------- util --- */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* A growable byte/string buffer. Always NUL-terminated. */
struct strbuf {
  char *buf;
  size_t len;
  size_t cap;
};
void sb_init(struct strbuf *b);
void sb_free(struct strbuf *b);
void sb_putc(struct strbuf *b, char c);
void sb_puts(struct strbuf *b, const char *s);
void sb_putn(struct strbuf *b, const char *s, size_t n);
void sb_printf(struct strbuf *b, const char *fmt, ...);
void sb_put_json_string(struct strbuf *b, const char *s);

const char *mime_type(const char *path);
int path_has_ext(const char *path, const char *const *exts);

/* --------------------------------------------------------------- files --- */

struct filelist {
  char **v;
  size_t n;
  size_t cap;
};
void filelist_init(struct filelist *fl);
void filelist_free(struct filelist *fl);
void filelist_add(struct filelist *fl, char *s);
void walk_dir(const char *root, const char *rel, struct filelist *fl, int max);
void filelist_sort(struct filelist *fl);

/* --------------------------------------------------------------- watch --- */

struct watcher;

/* Start watching root recursively. Returns NULL when watching is unavailable
 * (e.g. inotify is missing); the caller should carry on without it. */
struct watcher *watch_new(const char *root);
void watch_free(struct watcher *w);

/* File descriptor to poll for readability, or -1. */
int watch_fd(struct watcher *w);

/* Drain pending inotify events. Call when watch_fd() is readable. */
void watch_drain(struct watcher *w);

/* Returns 1 once a debounced batch of changes is ready. When it does, fills
 * *paths with malloc'd repo-relative paths (caller frees each and the array)
 * and *n with their count, and clears the pending state. */
int watch_take(struct watcher *w, char ***paths, size_t *n);

/* ------------------------------------------------------------ markdown --- */

/* Render a Markdown subset to an HTML fragment. base is the directory of the
 * source file (repo-relative), used to resolve relative links and images. */
void md_to_html(struct strbuf *out, const char *text, size_t len,
                const char *base);

/* -------------------------------------------------------- highlighting --- */

/* Friendly language name for a path (e.g. "c", "python"), for display. */
const char *highlight_lang_name(const char *path);

/* Tokenise source text into HTML spans, GitHub-style. */
void highlight_to_html(struct strbuf *out, const char *text, size_t len,
                       const char *path);

/* Tokenise source by fence language name (e.g. "python"); falls back to
 * plain escaped text for unknown languages. */
void highlight_lang_to_html(struct strbuf *out, const char *text, size_t len,
                            const char *lang);

/* -------------------------------------------------------------- server --- */
/* Run the HTTP server until interrupted. root is the directory being
 * previewed; web_root holds the bundled UI assets. */
int server_run(const char *root, const char *web_root, const char *host,
               int port, int do_open, const char *token);

#endif /* GANDER_H */
