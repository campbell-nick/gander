/* server.c - a tiny, single-threaded HTTP/1.1 server for the local UI.
 *
 * It binds to 127.0.0.1 only, serves the bundled web assets, exposes a few
 * JSON/HTML endpoints over the previewed directory, streams local files for
 * media previews (with byte-range support so video can seek), and pushes
 * live-reload events over Server-Sent Events. Everything is local; the
 * process never talks to the network.
 */
#include "gander.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNS 256
#define REQ_MAX (64 * 1024)
#define POLL_TIMEOUT_MS 40
/* With the embedded webview open we drive GTK ourselves, so poll a little more
 * often to keep the window responsive (~60 Hz). */
#define WEBVIEW_POLL_MS 16

#define CSP                                                                   \
  "default-src 'none'; img-src 'self' data: blob:; media-src 'self' blob:; "  \
  "style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src 'self'; " \
  "font-src 'self'; base-uri 'none'; form-action 'none'"

enum { C_READ, C_WRITE, C_SSE };

struct conn {
  int fd;
  int state;
  int close_after;
  struct strbuf in;
  struct strbuf out;
  size_t out_off;
  struct conn *next;
};

struct server {
  char root_real[4096];
  char web_real[4096];
  const char *token;
  struct watcher *w;
  struct conn *conns;
  int nconns;
  char user_css[4096];
};

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

/* ------------------------------------------------------------ small bits --- */

static void set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0)
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static const char *base_name(const char *path) {
  const char *s = strrchr(path, '/');
  return s ? s + 1 : path;
}

static void url_decode(const char *in, char *out, size_t outsz) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < outsz; i++) {
    if (in[i] == '%' && isxdigit((unsigned char)in[i + 1]) &&
        isxdigit((unsigned char)in[i + 2])) {
      char h[3] = {in[i + 1], in[i + 2], 0};
      out[o++] = (char)strtol(h, NULL, 16);
      i += 2;
    } else {
      out[o++] = in[i];
    }
  }
  out[o] = '\0';
}

/* Extract a query parameter (URL-decoded) by name. Returns 1 if present. */
static int query_param(const char *query, const char *key, char *out,
                       size_t outsz) {
  char qcopy[4096];
  snprintf(qcopy, sizeof qcopy, "%s", query);
  char *save = qcopy, *tok;
  while ((tok = strsep(&save, "&"))) {
    char k[256], v[4096];
    if (sscanf(tok, "%255[^=]=%4095s", k, v) == 2 && strcmp(k, key) == 0) {
      url_decode(v, out, outsz);
      return 1;
    }
  }
  return 0;
}

/* A repo-relative path is safe when it is non-empty, not absolute, and has no
 * "." or ".." component. Used where the target may no longer exist on disk
 * (e.g. the diff of a deleted file), so realpath cannot help. */
static int rel_is_safe(const char *rel) {
  if (!rel[0] || rel[0] == '/')
    return 0;
  const char *p = rel;
  while (*p) {
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n == 0)
      return 0;
    if (n == 1 && p[0] == '.')
      return 0;
    if (n == 2 && p[0] == '.' && p[1] == '.')
      return 0;
    if (!slash)
      break;
    p = slash + 1;
  }
  return 1;
}

/* Find a header value by name in a raw request buffer. Returns a pointer into
 * the buffer (not NUL-terminated) and stores its length, or NULL. */
static const char *find_header(const char *buf, const char *name, size_t *len) {
  size_t nl = strlen(name);
  const char *p = buf;
  while ((p = strchr(p, '\n'))) {
    p++;
    if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
      const char *v = p + nl + 1;
      while (*v == ' ' || *v == '\t')
        v++;
      const char *e = v;
      while (*e && *e != '\r' && *e != '\n')
        e++;
      *len = (size_t)(e - v);
      return v;
    }
  }
  return NULL;
}

/* Resolve rel under base_real, refusing anything that escapes it. */
static int resolve_under(const char *base_real, const char *rel, char *out,
                         size_t outsz) {
  char joined[8192];
  if (snprintf(joined, sizeof joined, "%s/%s", base_real, rel) >=
      (int)sizeof joined)
    return 0;
  char real[4096];
  if (!realpath(joined, real))
    return 0;
  size_t bl = strlen(base_real);
  if (strncmp(real, base_real, bl) != 0)
    return 0;
  if (real[bl] != '/' && real[bl] != '\0')
    return 0;
  snprintf(out, outsz, "%s", real);
  return 1;
}

/* --------------------------------------------------------------- responses - */

static void resp_headers(struct conn *c, int code, const char *reason,
                         const char *ctype, long len) {
  sb_printf(&c->out, "HTTP/1.1 %d %s\r\n", code, reason);
  sb_printf(&c->out, "Content-Type: %s\r\n", ctype);
  sb_printf(&c->out, "Content-Length: %ld\r\n", len);
  sb_puts(&c->out, "Cache-Control: no-store\r\n");
  sb_puts(&c->out, "X-Content-Type-Options: nosniff\r\n");
  sb_puts(&c->out, "Referrer-Policy: no-referrer\r\n");
  sb_printf(&c->out, "Content-Security-Policy: %s\r\n", CSP);
  sb_puts(&c->out, "Connection: close\r\n\r\n");
}

static void resp_text(struct conn *c, int code, const char *reason,
                      const char *ctype, const char *body) {
  resp_headers(c, code, reason, ctype, (long)strlen(body));
  sb_puts(&c->out, body);
  c->state = C_WRITE;
}

static void resp_error(struct conn *c, int code, const char *reason) {
  struct strbuf b;
  sb_init(&b);
  sb_printf(&b, "<!doctype html><meta charset=utf-8><title>%d</title>"
                "<h1>%d %s</h1>",
            code, code, reason);
  resp_text(c, code, reason, "text/html; charset=utf-8", b.buf);
  sb_free(&b);
}

static void resp_buf(struct conn *c, const char *ctype, struct strbuf *b) {
  resp_headers(c, 200, "OK", ctype, (long)b->len);
  sb_putn(&c->out, b->buf, b->len);
  c->state = C_WRITE;
}

/* --------------------------------------------------------------- files ----- */

static void serve_file(struct conn *c, const char *path, const char *ctype,
                       const char *range_hdr) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    resp_error(c, 404, "Not Found");
    return;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    resp_error(c, 404, "Not Found");
    return;
  }
  long size = (long)st.st_size;

  long start = 0, end = size - 1;
  int partial = 0;
  if (range_hdr && size > 0) {
    const char *p = range_hdr;
    if (strncasecmp(p, "bytes=", 6) == 0) {
      p += 6;
      const char *dash = strchr(p, '-');
      if (dash) {
        if (dash == p) { /* suffix range */
          long n = atol(dash + 1);
          if (n > 0) {
            start = size - n;
            if (start < 0)
              start = 0;
            end = size - 1;
            partial = 1;
          }
        } else {
          start = atol(p);
          end = (dash[1] == '\0' || dash[1] == ',') ? size - 1 : atol(dash + 1);
          if (end >= size)
            end = size - 1;
          if (start <= end && start < size)
            partial = 1;
        }
      }
    }
  }

  if (!partial) {
    start = 0;
    end = size - 1;
  }
  long len = (size == 0) ? 0 : (end - start + 1);

  if (partial) {
    sb_printf(&c->out, "HTTP/1.1 206 Partial Content\r\n");
    sb_printf(&c->out, "Content-Type: %s\r\n", ctype);
    sb_printf(&c->out, "Content-Range: bytes %ld-%ld/%ld\r\n", start, end, size);
    sb_printf(&c->out, "Content-Length: %ld\r\n", len);
    sb_puts(&c->out, "Accept-Ranges: bytes\r\n");
    sb_puts(&c->out, "Cache-Control: no-cache\r\n");
    sb_puts(&c->out, "X-Content-Type-Options: nosniff\r\n");
    sb_puts(&c->out, "Connection: close\r\n\r\n");
  } else {
    resp_headers(c, 200, "OK", ctype, len);
    /* replace the generic no-store with something friendlier for media */
  }

  /* Read the requested slice. */
  if (len > 0) {
    char *tmp = xmalloc((size_t)len);
    ssize_t got = pread(fd, tmp, (size_t)len, (off_t)start);
    if (got > 0)
      sb_putn(&c->out, tmp, (size_t)got);
    free(tmp);
  }
  close(fd);
  c->state = C_WRITE;
}

/* --------------------------------------------------------------- api ------- */

static void api_tree(struct server *s, struct conn *c) {
  struct filelist fl;
  filelist_init(&fl);
  walk_dir(s->root_real, "", &fl, 20000);
  filelist_sort(&fl);

  int is_git = 0;
  char branch[512] = "";
  {
    char head[4200];
    snprintf(head, sizeof head, "%s/.git/HEAD", s->root_real);
    FILE *f = fopen(head, "r");
    if (f) {
      is_git = 1;
      char line[512];
      if (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        const char *pfx = "ref: refs/heads/";
        if (strncmp(line, pfx, strlen(pfx)) == 0)
          snprintf(branch, sizeof branch, "%s", line + strlen(pfx));
        else if (line[0])
          snprintf(branch, sizeof branch, "%.12s", line); /* detached */
      }
      fclose(f);
    }
  }

  struct strbuf b;
  sb_init(&b);
  sb_puts(&b, "{\"name\":");
  sb_put_json_string(&b, base_name(s->root_real));
  sb_puts(&b, ",\"root\":");
  sb_put_json_string(&b, s->root_real);
  sb_printf(&b, ",\"is_git\":%s", is_git ? "true" : "false");
  sb_puts(&b, ",\"branch\":");
  sb_put_json_string(&b, branch);
  sb_puts(&b, ",\"files\":[");
  for (size_t i = 0; i < fl.n; i++) {
    if (i)
      sb_putc(&b, ',');
    sb_put_json_string(&b, fl.v[i]);
  }
  sb_puts(&b, "]}");

  resp_buf(c, "application/json; charset=utf-8", &b);
  sb_free(&b);
  filelist_free(&fl);
}

static void api_meta(struct server *s, struct conn *c) {
  struct repoinfo ri;
  repo_info(s->root_real, &ri);
  struct strbuf b;
  sb_init(&b);
  sb_puts(&b, "{\"name\":");
  sb_put_json_string(&b, base_name(s->root_real));
  sb_puts(&b, ",\"root\":");
  sb_put_json_string(&b, s->root_real);
  sb_puts(&b, ",");
  /* splice the repo object's fields in after "name"/"root" */
  struct strbuf inner;
  sb_init(&inner);
  repo_info_json(&inner, &ri);
  /* inner starts with '{'; drop it */
  sb_puts(&b, inner.buf + 1);
  sb_free(&inner);
  resp_buf(c, "application/json; charset=utf-8", &b);
  sb_free(&b);
}

static void api_status(struct server *s, struct conn *c) {
  struct statuslist sl;
  repo_status(s->root_real, &sl);

  char gitdir[4200];
  snprintf(gitdir, sizeof gitdir, "%s/.git", s->root_real);
  struct stat st;

  struct strbuf b;
  sb_init(&b);
  sb_printf(&b, "{\"is_git\":%s,\"files\":",
            (stat(gitdir, &st) == 0) ? "true" : "false");
  repo_status_json(&b, &sl);
  sb_puts(&b, "}");
  resp_buf(c, "application/json; charset=utf-8", &b);
  sb_free(&b);
  statuslist_free(&sl);
}

static void api_log(struct server *s, struct conn *c) {
  struct strbuf b;
  sb_init(&b);
  sb_puts(&b, "{\"commits\":");
  repo_log_json(s->root_real, &b, 50);
  sb_puts(&b, "}");
  resp_buf(c, "application/json; charset=utf-8", &b);
  sb_free(&b);
}

static void api_search(struct server *s, struct conn *c, const char *q,
                       int limit) {
  struct strbuf b;
  sb_init(&b);
  repo_search_json(s->root_real, q, limit, &b);
  resp_buf(c, "application/json; charset=utf-8", &b);
  sb_free(&b);
}

static void api_diff(struct server *s, struct conn *c, const char *rel) {
  if (!rel_is_safe(rel)) {
    resp_error(c, 404, "Not Found");
    return;
  }
  struct strbuf html;
  sb_init(&html);
  if (!repo_diff_html(s->root_real, rel, &html)) {
    sb_free(&html);
    resp_error(c, 404, "No diff");
    return;
  }
  resp_buf(c, "text/html; charset=utf-8", &html);
  sb_free(&html);
}

static void api_md(struct server *s, struct conn *c, const char *rel) {  char full[4096];
  if (!resolve_under(s->root_real, rel, full, sizeof full)) {
    resp_error(c, 404, "Not Found");
    return;
  }
  FILE *f = fopen(full, "rb");
  if (!f) {
    resp_error(c, 404, "Not Found");
    return;
  }
  struct strbuf raw;
  sb_init(&raw);
  char buf[8192];
  size_t got;
  while ((got = fread(buf, 1, sizeof buf, f)) > 0)
    sb_putn(&raw, buf, got);
  fclose(f);

  /* Base directory for resolving relative links/images. */
  char base[4096];
  snprintf(base, sizeof base, "%s", rel);
  char *slash = strrchr(base, '/');
  if (slash)
    *slash = '\0';
  else
    base[0] = '\0';

  struct strbuf html;
  sb_init(&html);
  md_to_html(&html, raw.buf ? raw.buf : "", raw.len, base);
  resp_buf(c, "text/html; charset=utf-8", &html);
  sb_free(&html);
  sb_free(&raw);
}

/* Build a GitHub-style code blob: a line-number gutter beside the
 * highlighted source. The whole blob is returned as an HTML fragment. */
static void api_code(struct server *s, struct conn *c, const char *rel) {
  char full[4096];
  if (!resolve_under(s->root_real, rel, full, sizeof full)) {
    resp_error(c, 404, "Not Found");
    return;
  }
  FILE *f = fopen(full, "rb");
  if (!f) {
    resp_error(c, 404, "Not Found");
    return;
  }
  const size_t MAX = 512 * 1024;
  struct strbuf raw;
  sb_init(&raw);
  char buf[8192];
  size_t got;
  int truncated = 0;
  while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
    if (raw.len + got > MAX) {
      sb_putn(&raw, buf, MAX - raw.len);
      truncated = 1;
      break;
    }
    sb_putn(&raw, buf, got);
  }
  fclose(f);

  /* binary? */
  int binary = 0;
  for (size_t i = 0; i < raw.len && i < 8000; i++)
    if (raw.buf[i] == '\0') {
      binary = 1;
      break;
    }

  struct strbuf body;
  sb_init(&body);

  if (binary) {
    sb_puts(&body,
            "<p class=\"notice\">Binary file — no preview available.</p>");
  } else {
    size_t lines = 1;
    for (size_t i = 0; i < raw.len; i++)
      if (raw.buf[i] == '\n')
        lines++;
    if (raw.len && raw.buf[raw.len - 1] == '\n')
      lines--;

    sb_puts(&body, "<div class=\"codeblob\"><div class=\"gutter\"><pre>");
    for (size_t i = 1; i <= lines; i++) {
      if (i > 1)
        sb_putc(&body, '\n');
      sb_printf(&body, "%zu", i);
    }
    sb_puts(&body, "</pre></div><div class=\"codecol\"><pre class=\"highlight\">");
    highlight_to_html(&body, raw.buf ? raw.buf : "", raw.len, rel);
    sb_puts(&body, "</pre></div></div>");
    if (truncated)
      sb_puts(&body, "<p class=\"notice\">… truncated for preview</p>");
  }

  const char *lang = highlight_lang_name(rel);

  /* response, with a couple of display headers */
  sb_puts(&c->out, "HTTP/1.1 200 OK\r\n");
  sb_puts(&c->out, "Content-Type: text/html; charset=utf-8\r\n");
  sb_printf(&c->out, "Content-Length: %zu\r\n", body.len);
  sb_printf(&c->out, "X-Gander-Lang: %s\r\n", lang);
  sb_puts(&c->out, "Cache-Control: no-store\r\n");
  sb_puts(&c->out, "X-Content-Type-Options: nosniff\r\n");
  sb_printf(&c->out, "Content-Security-Policy: %s\r\n", CSP);
  sb_puts(&c->out, "Connection: close\r\n\r\n");
  sb_putn(&c->out, body.buf, body.len);
  c->state = C_WRITE;

  sb_free(&body);
  sb_free(&raw);
}

static void api_raw(struct server *s, struct conn *c, const char *rel,
                    const char *range_hdr) {
  char full[4096];
  if (!resolve_under(s->root_real, rel, full, sizeof full)) {
    resp_error(c, 404, "Not Found");
    return;
  }
  serve_file(c, full, mime_type(rel), range_hdr);
}

static void api_events(struct conn *c) {
  sb_puts(&c->out, "HTTP/1.1 200 OK\r\n");
  sb_puts(&c->out, "Content-Type: text/event-stream\r\n");
  sb_puts(&c->out, "Cache-Control: no-cache\r\n");
  sb_puts(&c->out, "X-Accel-Buffering: no\r\n");
  sb_puts(&c->out, "Connection: keep-alive\r\n\r\n");
  sb_puts(&c->out, ": connected\n\n");
  c->state = C_SSE;
  c->close_after = 0;
}

/* --------------------------------------------------------------- web ------- */

static void serve_web(struct server *s, struct conn *c, const char *name) {
  char full[4096];
  if (!resolve_under(s->web_real, name, full, sizeof full)) {
    resp_error(c, 404, "Not Found");
    return;
  }
  serve_file(c, full, mime_type(name), NULL);
}

static void append_file(struct strbuf *b, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return;
  char buf[8192];
  size_t got;
  while ((got = fread(buf, 1, sizeof buf, f)) > 0)
    sb_putn(b, buf, got);
  fclose(f);
}

/* The bundled theme, with the user's style.css (if any) layered on top so a
 * small override file is enough to re-skin gander. */
static void serve_style(struct server *s, struct conn *c) {
  struct strbuf b;
  sb_init(&b);
  char full[4096];
  if (resolve_under(s->web_real, "style.css", full, sizeof full))
    append_file(&b, full);
  if (s->user_css[0]) {
    sb_puts(&b, "\n/* ---- user style.css ---- */\n");
    append_file(&b, s->user_css);
  }
  resp_buf(c, "text/css; charset=utf-8", &b);
  sb_free(&b);
}

/* -------------------------------------------------------------- routing ----- */

static void handle_request(struct server *s, struct conn *c) {
  char *hdr_end = strstr(c->in.buf, "\r\n\r\n");
  if (!hdr_end)
    return;

  char method[16] = "", target[4096] = "";
  if (sscanf(c->in.buf, "%15s %4095s", method, target) != 2) {
    resp_error(c, 400, "Bad Request");
    return;
  }
  if (strcmp(method, "GET") != 0) {
    resp_error(c, 405, "Method Not Allowed");
    return;
  }

  /* split path and query */
  char path[4096], query[4096] = "";
  snprintf(path, sizeof path, "%s", target);
  char *q = strchr(path, '?');
  if (q) {
    snprintf(query, sizeof query, "%s", q + 1);
    *q = '\0';
  }
  char decoded[4096];
  url_decode(path, decoded, sizeof decoded);

  /* token gate (query or cookie) */
  if (s->token && s->token[0]) {
    int ok = 0;
    if (query[0]) {
      char qv[512];
      char key[256], val[512];
      char qcopy[4096];
      snprintf(qcopy, sizeof qcopy, "%s", query);
      char *save = qcopy, *tok;
      while ((tok = strsep(&save, "&"))) {
        if (sscanf(tok, "%255[^=]=%511s", key, val) == 2 &&
            strcmp(key, "token") == 0) {
          url_decode(val, qv, sizeof qv);
          if (strcmp(qv, s->token) == 0)
            ok = 1;
          break;
        }
      }
    }
    if (!ok) {
      size_t cl;
      const char *ck = find_header(c->in.buf, "Cookie", &cl);
      if (ck) {
        char ccopy[4096];
        size_t n = cl < sizeof ccopy - 1 ? cl : sizeof ccopy - 1;
        memcpy(ccopy, ck, n);
        ccopy[n] = '\0';
        char needle[600];
        snprintf(needle, sizeof needle, "gander_token=%s", s->token);
        if (strstr(ccopy, needle))
          ok = 1;
      }
    }
    if (!ok) {
      resp_error(c, 403, "Forbidden");
      return;
    }
  }

  size_t rl;
  const char *range = find_header(c->in.buf, "Range", &rl);
  char range_buf[256] = "";
  if (range) {
    size_t n = rl < sizeof range_buf - 1 ? rl : sizeof range_buf - 1;
    memcpy(range_buf, range, n);
    range_buf[n] = '\0';
  }

  if (strcmp(decoded, "/") == 0 || strcmp(decoded, "/index.html") == 0) {
    serve_web(s, c, "index.html");
    if (s->token && s->token[0]) {
      /* insert Set-Cookie right after the status line */
      struct strbuf nb;
      sb_init(&nb);
      char *eol = strstr(c->out.buf, "\r\n");
      if (eol) {
        sb_putn(&nb, c->out.buf, (size_t)(eol - c->out.buf) + 2);
        sb_printf(&nb, "Set-Cookie: gander_token=%s; Path=/; SameSite=Strict\r\n",
                  s->token);
        sb_puts(&nb, eol + 2);
        sb_free(&c->out);
        c->out = nb;
      } else {
        sb_free(&nb);
      }
    }
    return;
  }
  if (strcmp(decoded, "/style.css") == 0) {
    serve_style(s, c);
    return;
  }
  if (strcmp(decoded, "/app.js") == 0) {
    serve_web(s, c, "app.js");
    return;
  }
  if (strcmp(decoded, "/logo.svg") == 0) {
    serve_web(s, c, "logo.svg");
    return;
  }
  if (strcmp(decoded, "/api/tree") == 0) {
    api_tree(s, c);
    return;
  }
  if (strcmp(decoded, "/api/meta") == 0) {
    api_meta(s, c);
    return;
  }
  if (strcmp(decoded, "/api/status") == 0) {
    api_status(s, c);
    return;
  }
  if (strcmp(decoded, "/api/log") == 0) {
    api_log(s, c);
    return;
  }
  if (strcmp(decoded, "/api/search") == 0) {
    char sq[4096] = "";
    if (!query_param(query, "q", sq, sizeof sq) || !sq[0]) {
      resp_error(c, 400, "Bad Request");
      return;
    }
    char lim[32] = "";
    int limit = query_param(query, "limit", lim, sizeof lim) ? atoi(lim) : 100;
    api_search(s, c, sq, limit);
    return;
  }
  if (strcmp(decoded, "/api/events") == 0) {
    api_events(c);
    return;
  }
  if (strcmp(decoded, "/api/md") == 0) {
    char rel[4096] = "";
    if (!query_param(query, "path", rel, sizeof rel) || !rel[0]) {
      resp_error(c, 400, "Bad Request");
      return;
    }
    api_md(s, c, rel);
    return;
  }
  if (strcmp(decoded, "/api/code") == 0) {
    char rel[4096] = "";
    if (!query_param(query, "path", rel, sizeof rel) || !rel[0]) {
      resp_error(c, 400, "Bad Request");
      return;
    }
    api_code(s, c, rel);
    return;
  }
  if (strcmp(decoded, "/api/diff") == 0) {
    char rel[4096] = "";
    if (!query_param(query, "path", rel, sizeof rel) || !rel[0]) {
      resp_error(c, 400, "Bad Request");
      return;
    }
    api_diff(s, c, rel);
    return;
  }
  if (strncmp(decoded, "/raw/", 5) == 0) {
    api_raw(s, c, decoded + 5, range_buf[0] ? range_buf : NULL);
    return;
  }

  resp_error(c, 404, "Not Found");
}

/* -------------------------------------------------------------- events ------ */

static void broadcast(struct server *s, char **paths, size_t n) {
  struct strbuf msg;
  sb_init(&msg);
  sb_puts(&msg, "event: change\ndata: {\"paths\":[");
  for (size_t i = 0; i < n; i++) {
    if (i)
      sb_putc(&msg, ',');
    sb_put_json_string(&msg, paths[i]);
  }
  sb_puts(&msg, "]}\n\n");

  for (struct conn *c = s->conns; c; c = c->next)
    if (c->state == C_SSE)
      sb_putn(&c->out, msg.buf, msg.len);

  sb_free(&msg);
}

/* -------------------------------------------------------------- conns ------- */

static struct conn *conn_new(struct server *s, int fd) {
  struct conn *c = xmalloc(sizeof *c);
  memset(c, 0, sizeof *c);
  c->fd = fd;
  c->state = C_READ;
  sb_init(&c->in);
  sb_init(&c->out);
  c->next = s->conns;
  s->conns = c;
  s->nconns++;
  return c;
}

static void conn_free(struct server *s, struct conn *c) {
  struct conn **pp = &s->conns;
  while (*pp && *pp != c)
    pp = &(*pp)->next;
  if (*pp)
    *pp = c->next;
  s->nconns--;
  close(c->fd);
  sb_free(&c->in);
  sb_free(&c->out);
  free(c);
}

static void conn_read(struct server *s, struct conn *c) {
  char buf[8192];
  ssize_t n = recv(c->fd, buf, sizeof buf, 0);
  if (n == 0) {
    c->close_after = 1;
    return;
  }
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    c->close_after = 1;
    return;
  }
  if (c->state == C_SSE) {
    /* ignore any client chatter; we only write */
    return;
  }
  sb_putn(&c->in, buf, (size_t)n);
  if (c->in.len > REQ_MAX) {
    c->close_after = 1;
    return;
  }
  if (strstr(c->in.buf, "\r\n\r\n"))
    handle_request(s, c);
}

static void conn_write(struct conn *c) {
  while (c->out_off < c->out.len) {
    ssize_t n = send(c->fd, c->out.buf + c->out_off,
                     c->out.len - c->out_off, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      c->close_after = 1;
      return;
    }
    c->out_off += (size_t)n;
  }
  /* fully flushed */
  if (c->state == C_WRITE)
    c->close_after = 1;
  else if (c->state == C_SSE) {
    sb_free(&c->out);
    sb_init(&c->out);
    c->out_off = 0;
  }
}

/* -------------------------------------------------------------- browser ----- */

/* Is `name` an executable file somewhere on PATH? Fills out with its full path
 * when found. */
static int find_in_path(const char *name, char *out, size_t outsz) {
  const char *path = getenv("PATH");
  if (!path || !path[0])
    path = "/usr/local/bin:/usr/bin:/bin";
  const char *p = path;
  while (*p) {
    const char *colon = strchr(p, ':');
    size_t n = colon ? (size_t)(colon - p) : strlen(p);
    if (n > 0 && n < 4000) {
      char cand[4096];
      snprintf(cand, sizeof cand, "%.*s/%s", (int)n, p, name);
      if (access(cand, X_OK) == 0) {
        snprintf(out, outsz, "%s", cand);
        return 1;
      }
    }
    if (!colon)
      break;
    p = colon + 1;
  }
  return 0;
}

/* The first Chromium-family browser on PATH, if any. Firefox has no --app
 * equivalent, so it is deliberately not listed. */
static int find_chromium(char *out, size_t outsz) {
  static const char *const names[] = {
      "chromium",       "chromium-browser",      "google-chrome",
      "google-chrome-stable", "brave",           "brave-browser",
      "helium",         "microsoft-edge",        "microsoft-edge-stable",
      "vivaldi",        "vivaldi-stable",        NULL};
  for (int i = 0; names[i]; i++)
    if (find_in_path(names[i], out, outsz))
      return 1;
  return 0;
}

/* mkdir -p, mode 0700. Returns 1 on success (or if it already exists). */
static int mkdir_p(const char *dir) {
  char tmp[4096];
  snprintf(tmp, sizeof tmp, "%s", dir);
  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = '\0';
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
      return 0;
    *p = '/';
  }
  if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
    return 0;
  return 1;
}

/* A throwaway Chromium profile, so --app never touches the user's own browser
 * data. Prefers $XDG_CACHE_HOME/gander/app, then ~/.cache/gander/app; leaves
 * out empty if neither can be created. */
static void app_profile(char *out, size_t outsz) {
  out[0] = '\0';
  char base[4096] = "";
  const char *xdg = getenv("XDG_CACHE_HOME");
  const char *home = getenv("HOME");
  if (xdg && xdg[0])
    snprintf(base, sizeof base, "%s/gander/app", xdg);
  else if (home && home[0])
    snprintf(base, sizeof base, "%s/.cache/gander/app", home);
  else
    return;
  if (mkdir_p(base))
    snprintf(out, outsz, "%s", base);
}

static void open_in_browser(const char *url) {
  pid_t pid = fork();
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execlp("xdg-open", "xdg-open", url, (char *)NULL);
    execlp("open", "open", url, (char *)NULL);
    execlp("sensible-browser", "sensible-browser", url, (char *)NULL);
    _exit(1);
  }
}

/* The URL the UI is reachable at, from the host's point of view. A wildcard
 * bind address is reported as loopback, which is where a local browser or
 * webview actually connects. */
static void ui_url(const char *host, int port, const char *token, char *out,
                   size_t outsz) {
  const char *h = host;
  if (strcmp(host, "0.0.0.0") == 0 || strcmp(host, "::") == 0)
    h = "127.0.0.1";
  if (token && token[0])
    snprintf(out, outsz, "http://%s:%d/?token=%s", h, port, token);
  else
    snprintf(out, outsz, "http://%s:%d/", h, port);
}

static void launch_ui(const char *host, int port, const char *token, int app) {
  char url[600];
  ui_url(host, port, token, url, sizeof url);

  if (app) {
    char browser[4096];
    if (find_chromium(browser, sizeof browser)) {
      char profile[4096];
      app_profile(profile, sizeof profile);
      char appflag[700];
      snprintf(appflag, sizeof appflag, "--app=%s", url);
      pid_t pid = fork();
      if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
          dup2(devnull, STDOUT_FILENO);
          dup2(devnull, STDERR_FILENO);
          close(devnull);
        }
        if (profile[0]) {
          char udd[4200];
          snprintf(udd, sizeof udd, "--user-data-dir=%s", profile);
          execl(browser, browser, appflag, udd, "--no-first-run",
                "--no-default-browser-check", (char *)NULL);
        } else {
          execl(browser, browser, appflag, "--no-first-run",
                "--no-default-browser-check", (char *)NULL);
        }
        _exit(1);
      }
      return;
    }
    fprintf(stderr,
            "gander: no Chromium-family browser found for --app; "
            "opening a tab instead\n");
  }

  open_in_browser(url);
}

/* --------------------------------------------------------------- run -------- */

/* gander only previews git repositories. Point it at a plain folder and the
 * goose says its piece, then we bow out without serving anything. */
static void note_no_git(const char *path) {
  fprintf(stderr,
          "gander: This isn't a grain, it's a bread! (not a git repository: "
          "%s)\n",
          path);
}

/* A goose never says the same goodbye twice. */
static void farewell(void) {
  static const char *const lines[] = {
      "Have a nice day.",
      "See you next time.",
      "Finished?",
      "Happy now?",
      "Fix again?",
      "Honk.",
      "Fly safe.",
      "Off you waddle.",
      "Gander out.",
      "See you, space goose.",
      "Keep it local.",
      "Nothing left to see here.",
      "Waddle you do now?",
      "Take care.",
      "Fare thee well, gentle traveller.",
      "Hie thee hence, and tarry not.",
      "Wend thy way in peace.",
      "Go forth; the road is thine.",
      "Anon we part, as all good things must.",
      "Farewell, dear friend; till we meet anon.",
      "Your mileage may vary, and as you wish.",
      "Make it yours.",
  };
  size_t n = sizeof lines / sizeof lines[0];
  srand((unsigned)time(NULL) ^ (unsigned)getpid());
  printf("\ngander: %s\n", lines[(unsigned)rand() % n]);
}

/* The tty driver echoes a typed Ctrl-C as "^C" before the signal ever reaches
 * us. gander never reads the keyboard, so switch echo off for the duration -
 * Ctrl-C then arrives as a signal with nothing echoed - and put it back on the
 * way out. Returns the tty fd it changed (and fills *saved), or -1. */
static int tty_quiet(struct termios *saved) {
  int fds[3] = {STDIN_FILENO, STDERR_FILENO, STDOUT_FILENO};
  for (int i = 0; i < 3; i++) {
    if (!isatty(fds[i]))
      continue;
    if (tcgetattr(fds[i], saved) != 0)
      return -1;
    struct termios t = *saved;
    t.c_lflag &= ~(tcflag_t)ECHO;
#ifdef ECHOCTL
    t.c_lflag &= ~(tcflag_t)ECHOCTL;
#endif
    if (tcsetattr(fds[i], TCSANOW, &t) != 0)
      return -1;
    return fds[i];
  }
  return -1;
}

static void tty_restore(int fd, const struct termios *saved) {
  if (fd >= 0)
    tcsetattr(fd, TCSANOW, saved);
}

int server_run(const char *root, const char *web_root, const char *host,
               int port, int do_open, int do_app, int do_webview,
               const char *token, int watch_scope, const char *user_css) {
  /* Never let git take its optional locks. Without this, `git status` (which
   * we run to build the tree badges and the home card) refreshes and rewrites
   * .git/index, the watcher sees that as a change, the page refetches status,
   * and the two feed each other - the page blinks and scroll jumps to the
   * top. GIT_OPTIONAL_LOCKS=0 tells git to skip those optional index writes. */
  setenv("GIT_OPTIONAL_LOCKS", "0", 1);

  struct server s;
  memset(&s, 0, sizeof s);
  s.token = token;
  if (user_css && user_css[0])
    snprintf(s.user_css, sizeof s.user_css, "%s", user_css);

  if (!realpath(root, s.root_real)) {
    fprintf(stderr, "gander: cannot resolve %s: %s\n", root, strerror(errno));
    return 1;
  }
  if (!realpath(web_root, s.web_real)) {
    fprintf(stderr, "gander: warning: web root %s not found\n", web_root);
    snprintf(s.web_real, sizeof s.web_real, "%s", web_root);
  }

  /* gander previews git repositories. A plain folder gets the goose's line and
   * nothing else - no server, no browser. A `.git` file (a linked worktree)
   * counts as a repository too. */
  char gitdir[4200];
  snprintf(gitdir, sizeof gitdir, "%s/.git", s.root_real);
  struct stat gitst;
  if (stat(gitdir, &gitst) != 0) {
    note_no_git(s.root_real);
    return 1;
  }

  int lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) {
    perror("gander: socket");
    return 1;
  }
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (strcmp(host, "0.0.0.0") == 0)
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
  else if (inet_pton(AF_INET, host, &sa.sin_addr) != 1)
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0) {
    fprintf(stderr, "gander: bind %s:%d: %s\n", host, port, strerror(errno));
    close(lfd);
    return 1;
  }
  if (listen(lfd, 64) != 0) {
    perror("gander: listen");
    close(lfd);
    return 1;
  }

  socklen_t sl = sizeof sa;
  getsockname(lfd, (struct sockaddr *)&sa, &sl);
  int actual = ntohs(sa.sin_port);
  set_nonblock(lfd);

  char url[600];
  ui_url(host, actual, token, url, sizeof url);

  /* The embedded webview is opt-in at build time. Without it, --webview reports
   * that and a browser is used instead, so the flag is never a dead end. */
  struct webview_win *wv = NULL;
  if (do_webview && !webview_available())
    fprintf(stderr,
            "gander: this build has no embedded webview "
            "(rebuild with 'make WEBVIEW=1')\n");

  if (do_open) {
    if (do_webview && webview_available()) {
      /* Use the bundled logo as the window icon, next to the served assets. */
      char icon[4200];
      snprintf(icon, sizeof icon, "%s/logo.svg", s.web_real);
      wv = webview_open(url, "gander", icon, 1100, 800);
      if (!wv)
        fprintf(stderr,
                "gander: could not open the embedded webview; "
                "using a browser instead\n");
    }
    if (!wv)
      launch_ui(host, actual, token, do_app);
  }

  s.w = watch_scope != WATCH_OFF ? watch_new(s.root_real, watch_scope) : NULL;

  printf("gander %s\n", GANDER_VERSION);
  printf("  root:   %s\n", s.root_real);
  printf("  url:    http://%s:%d/%s\n", host, actual,
         (token && token[0]) ? "?token=***" : "");
  if (s.w)
    printf("  watch:  inotify (%s)\n",
           watch_scope == WATCH_COMMIT ? "commit" : "all files");
  else
    printf("  watch:  %s\n", watch_scope == WATCH_OFF ? "disabled" : "unavailable");
  if (s.user_css[0])
    printf("  style:  %s\n", s.user_css);
  if (wv)
    printf("  view:   embedded webview\n");
  printf("\nPress Ctrl-C to stop.\n");
  fflush(stdout);

  /* Quit quietly: no echoed "^C" when you stop the server. */
  struct termios tty_saved;
  int tty_q = tty_quiet(&tty_saved);

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  struct pollfd pfds[MAX_CONNS + 4];
  struct conn *pmap[MAX_CONNS + 4];

  while (!g_stop) {
    int nf = 0;
    pfds[nf].fd = lfd;
    pfds[nf].events = POLLIN;
    pfds[nf].revents = 0;
    pmap[nf] = NULL;
    nf++;

    int wfd = watch_fd(s.w);
    int wslot = -1;
    if (wfd >= 0) {
      wslot = nf;
      pfds[nf].fd = wfd;
      pfds[nf].events = POLLIN;
      pfds[nf].revents = 0;
      pmap[nf] = NULL;
      nf++;
    }

    for (struct conn *c = s.conns; c && nf < MAX_CONNS + 4; c = c->next) {
      pfds[nf].fd = c->fd;
      pfds[nf].events = (c->state == C_WRITE) ? POLLOUT : POLLIN;
      pfds[nf].revents = 0;
      pmap[nf] = c;
      nf++;
    }

    int pr = poll(pfds, (nfds_t)nf, wv ? WEBVIEW_POLL_MS : POLL_TIMEOUT_MS);
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      perror("gander: poll");
      break;
    }

    /* Drive the embedded webview; closing its window stops the server. */
    if (wv && !webview_pump(wv))
      break;

    /* accept new connections */
    if (pfds[0].revents & POLLIN) {
      for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0)
          break;
        if (s.nconns >= MAX_CONNS) {
          close(fd);
          continue;
        }
        set_nonblock(fd);
        conn_new(&s, fd);
      }
    }

    if (wslot >= 0 && (pfds[wslot].revents & POLLIN))
      watch_drain(s.w);

    for (int i = 1; i < nf; i++) {
      struct conn *c = pmap[i];
      if (!c)
        continue;
      short re = pfds[i].revents;
      if (re & (POLLERR | POLLNVAL)) {
        c->close_after = 1;
        continue;
      }
      if (re & (POLLIN | POLLHUP)) {
        if (c->state == C_READ || c->state == C_SSE)
          conn_read(&s, c);
      }
      if (!c->close_after && (c->state == C_WRITE) && (re & POLLOUT))
        conn_write(c);
    }

    /* flush any responses queued this round */
    for (struct conn *c = s.conns; c; c = c->next)
      if (c->out_off < c->out.len)
        conn_write(c);

    /* debounced live-reload broadcast */
    char **paths;
    size_t np;
    if (watch_take(s.w, &paths, &np)) {
      broadcast(&s, paths, np);
      for (size_t i = 0; i < np; i++)
        free(paths[i]);
      free(paths);
    }

    /* reap */
    struct conn *c = s.conns;
    while (c) {
      struct conn *next = c->next;
      if (c->close_after)
        conn_free(&s, c);
      c = next;
    }
  }

  tty_restore(tty_q, &tty_saved);
  webview_destroy(wv);
  farewell();
  while (s.conns)
    conn_free(&s, s.conns);
  watch_free(s.w);
  close(lfd);
  return 0;
}
