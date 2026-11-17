/*
 * gander - an offline previewer for a local git repository.
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

/* Escape a byte range for HTML text or a double-quoted attribute value. */
void sb_put_html_escaped(struct strbuf *b, const char *s, size_t n);

const char *mime_type(const char *path);
int path_has_ext(const char *path, const char *const *exts);

/* --------------------------------------------------------------- urls --- */

/* True when the string starts with a URL scheme (e.g. "http:", "mailto:"). */
int url_has_scheme(const char *u);

/* True when the string is a remote http(s) URL, which offline we cannot fetch. */
int url_is_remote(const char *u, size_t n);

/* Rewrite a link/image target into something the browser can fetch: a relative
 * path becomes /raw/<path>; anchors, absolute paths and URLs pass through. The
 * caller frees the result. */
char *url_to_local(const char *url, const char *base);

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

/* What live reload reacts to.
 *
 *   WATCH_OFF     no watching at all.
 *   WATCH_COMMIT  only git history changes: a commit, a tag, a branch switch
 *                 or a fetch. Working-tree edits are ignored, so the page
 *                 stays put while you edit and only refreshes when you commit.
 *   WATCH_ALL     any watched file change (edits, saves, new files, ...). */
enum watch_scope {
  WATCH_OFF = 0,
  WATCH_COMMIT = 1,
  WATCH_ALL = 2,
};

/* Start watching root. scope selects what counts as a change. Returns NULL
 * when watching is unavailable (e.g. inotify is missing); the caller should
 * carry on without it. */
struct watcher *watch_new(const char *root, int scope);
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

/* --------------------------------------------------------------- html --- */

/* Render a fragment of raw HTML: keep a whitelist of safe tags and attributes,
 * drop everything else, rewrite relative src/href to /raw/<path>, and show
 * remote images as badge chips. READMEs commonly embed raw HTML such as a
 * centred logo. base resolves relative paths, as for md_to_html. */
void html_to_html(struct strbuf *out, const char *text, size_t len,
                  const char *base);

/* If a plausible HTML tag or comment begins at s, return its byte length
 * including the closing '>', else 0. */
size_t html_tag_len(const char *s, size_t n);

/* True when a line begins an HTML block (a tag or comment after optional
 * leading whitespace). */
int html_block_start(const char *line, size_t len);

/* ---------------------------------------------------------------- repo --- */

struct langent {
  char name[32];
  char color[16];
  long bytes;
};

struct repoinfo {
  int is_git;
  char branch[256];
  char remote[512];
  int dirty;
  int changes;
  char last_hash[16];
  char last_subject[300];
  char last_author[128];
  char last_rel[64];
  long commits;
  char latest_tag[64];
  int tag_count;
  char license[64];
  int ci_count;
  char ci_names[8][160];
  char readme[256];
  char description[400];
  long files;
  long total_bytes;
  struct langent langs[8];
  int nlangs;
};

/* Gather repo metadata from disk and (if present) the local git CLI. */
void repo_info(const char *root, struct repoinfo *ri);

/* Serialise the metadata as JSON. */
void repo_info_json(struct strbuf *out, const struct repoinfo *ri);

/* One entry of `git status --porcelain`. code is the raw two-letter XY code;
 * status is a normalised word (modified/added/deleted/renamed/copied/
 * untracked/conflicted/typechange). Untracked directories keep a trailing
 * slash, so a file inside one can be recognised by its prefix. */
struct statusent {
  char path[4096];
  char code[4];
  char status[16];
};

struct statuslist {
  struct statusent *v;
  size_t n;
  size_t cap;
};

/* Collect the working-tree status. Leaves *sl empty outside a git repo. */
void repo_status(const char *root, struct statuslist *sl);
void statuslist_free(struct statuslist *sl);

/* Serialise a status list as a JSON array. */
void repo_status_json(struct strbuf *out, const struct statuslist *sl);

/* Serialise up to max recent commits as a JSON array. */
void repo_log_json(const char *root, struct strbuf *out, int max);

/* Render the per-file unified diff for rel as an HTML fragment (GitHub-style
 * rows with old/new line gutters). Untracked files are shown as all-additions.
 * Returns 1 when something was rendered, 0 when there is nothing to show. */
int repo_diff_html(const char *root, const char *rel, struct strbuf *out);

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

/* -------------------------------------------------------------- config --- */

/* User configuration, read from an optional config file. Every field has a
 * built-in default; command-line options override anything set here. */
struct gander_config {
  int port;            /* 0 = pick a free port */
  int do_open;         /* launch a browser on start */
  int watch_scope;     /* enum watch_scope */
  char host[256];
  char web_root[4096];
  char token[512];
  char css[4096];      /* user stylesheet layered over the bundled one, or "" */
  char path[4096];     /* config file actually read, or "" */
};

/* Fill cfg with built-in defaults (including the default user stylesheet). */
void config_init(struct gander_config *cfg);

/* Read settings from explicit_path, else $GANDER_CONFIG, else
 * $XDG_CONFIG_HOME/gander/config, else $HOME/.config/gander/config. A missing
 * file is not an error; a malformed line is warned about and skipped. */
void config_load(struct gander_config *cfg, const char *explicit_path);

/* -------------------------------------------------------------- server --- */
/* Run the HTTP server until interrupted. root is the directory being
 * previewed; web_root holds the bundled UI assets. user_css, when non-empty,
 * is a stylesheet appended after the bundled style.css so it can override the
 * theme. */
int server_run(const char *root, const char *web_root, const char *host,
               int port, int do_open, const char *token, int watch_scope,
               const char *user_css);

#endif /* GANDER_H */
