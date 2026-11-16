/* util.c - small helpers: memory, a string buffer, JSON escaping, MIME. */
#include "gander.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------------------------------------------------------------- mem ---- */

void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) {
    fputs("gander: out of memory\n", stderr);
    exit(1);
  }
  return p;
}

void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) {
    fputs("gander: out of memory\n", stderr);
    exit(1);
  }
  return q;
}

char *xstrdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = xmalloc(n);
  memcpy(p, s, n);
  return p;
}

char *xstrndup(const char *s, size_t n) {
  char *p = xmalloc(n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

/* -------------------------------------------------------------- strbuf --- */

void sb_init(struct strbuf *b) {
  b->buf = NULL;
  b->len = 0;
  b->cap = 0;
}

void sb_free(struct strbuf *b) {
  free(b->buf);
  b->buf = NULL;
  b->len = b->cap = 0;
}

static void sb_reserve(struct strbuf *b, size_t extra) {
  if (b->len + extra + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + extra + 1)
      nc *= 2;
    b->buf = xrealloc(b->buf, nc);
    b->cap = nc;
  }
}

void sb_putc(struct strbuf *b, char c) {
  sb_reserve(b, 1);
  b->buf[b->len++] = c;
  b->buf[b->len] = '\0';
}

void sb_putn(struct strbuf *b, const char *s, size_t n) {
  sb_reserve(b, n);
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  b->buf[b->len] = '\0';
}

void sb_puts(struct strbuf *b, const char *s) { sb_putn(b, s, strlen(s)); }

void sb_printf(struct strbuf *b, const char *fmt, ...) {
  va_list ap;
  char tmp[1024];
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((size_t)n < sizeof tmp) {
    sb_putn(b, tmp, (size_t)n);
    return;
  }
  char *big = xmalloc((size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  sb_puts(b, big);
  free(big);
}

void sb_put_json_string(struct strbuf *b, const char *s) {
  sb_putc(b, '"');
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
    case '"':
      sb_puts(b, "\\\"");
      break;
    case '\\':
      sb_puts(b, "\\\\");
      break;
    case '\n':
      sb_puts(b, "\\n");
      break;
    case '\r':
      sb_puts(b, "\\r");
      break;
    case '\t':
      sb_puts(b, "\\t");
      break;
    case '\b':
      sb_puts(b, "\\b");
      break;
    case '\f':
      sb_puts(b, "\\f");
      break;
    default:
      if (*p < 0x20)
        sb_printf(b, "\\u%04x", *p);
      else
        sb_putc(b, (char)*p);
    }
  }
  sb_putc(b, '"');
}

/* ---------------------------------------------------------------- mime --- */

static const struct {
  const char *ext;
  const char *type;
} MIME[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"json", "application/json; charset=utf-8"},
    {"map", "application/json; charset=utf-8"},
    {"svg", "image/svg+xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"avif", "image/avif"},
    {"bmp", "image/bmp"},
    {"ico", "image/x-icon"},
    {"tif", "image/tiff"},
    {"tiff", "image/tiff"},
    {"mp4", "video/mp4"},
    {"m4v", "video/mp4"},
    {"webm", "video/webm"},
    {"ogv", "video/ogg"},
    {"mov", "video/quicktime"},
    {"mkv", "video/x-matroska"},
    {"mp3", "audio/mpeg"},
    {"ogg", "audio/ogg"},
    {"oga", "audio/ogg"},
    {"wav", "audio/wav"},
    {"flac", "audio/flac"},
    {"m4a", "audio/mp4"},
    {"pdf", "application/pdf"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
    {"txt", "text/plain; charset=utf-8"},
    {"md", "text/markdown; charset=utf-8"},
    {"markdown", "text/markdown; charset=utf-8"},
    {"csv", "text/csv; charset=utf-8"},
    {"tsv", "text/tab-separated-values; charset=utf-8"},
    {"xml", "application/xml; charset=utf-8"},
    {"yml", "text/yaml; charset=utf-8"},
    {"yaml", "text/yaml; charset=utf-8"},
    {"toml", "text/plain; charset=utf-8"},
    {"c", "text/plain; charset=utf-8"},
    {"h", "text/plain; charset=utf-8"},
    {"rs", "text/plain; charset=utf-8"},
    {"go", "text/plain; charset=utf-8"},
    {"py", "text/plain; charset=utf-8"},
    {"sh", "text/plain; charset=utf-8"},
    {"zsh", "text/plain; charset=utf-8"},
    {"nix", "text/plain; charset=utf-8"},
};

const char *mime_type(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot)
    return "application/octet-stream";
  const char *ext = dot + 1;
  for (size_t i = 0; i < sizeof MIME / sizeof MIME[0]; i++)
    if (strcasecmp(ext, MIME[i].ext) == 0)
      return MIME[i].type;
  return "application/octet-stream";
}

int path_has_ext(const char *path, const char *const *exts) {
  const char *dot = strrchr(path, '.');
  if (!dot)
    return 0;
  const char *ext = dot + 1;
  for (size_t i = 0; exts[i]; i++)
    if (strcasecmp(ext, exts[i]) == 0)
      return 1;
  return 0;
}
