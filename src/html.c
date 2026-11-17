/* html.c - a small, safe renderer for the raw HTML that READMEs embed.
 *
 * GitHub lets READMEs contain a sanitised subset of HTML (a centred logo, a
 * <details> block, <kbd> hints, and so on). gander does the same, offline: it
 * keeps a whitelist of tags and attributes, drops everything else (scripts,
 * event handlers, unknown tags), rewrites relative src/href to /raw/<path>,
 * and shows remote images as badge chips because the browser cannot fetch them
 * without the network. Not a general-purpose HTML parser.
 */
#include "gander.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Tags we pass through. Everything else is dropped (its text content stays). */
static int tag_ok(const char *name, size_t n) {
  static const char *ok[] = {
      "a",        "b",     "strong",   "i",      "em",      "u",
      "s",        "del",   "ins",      "sub",    "sup",     "kbd",
      "code",     "pre",   "span",     "div",    "p",       "br",
      "hr",       "img",   "picture",  "source", "figure",  "figcaption",
      "center",   "details", "summary", "table", "thead",   "tbody",
      "tfoot",    "tr",    "th",       "td",     "caption", "ul",
      "ol",       "li",    "blockquote", "h1",   "h2",      "h3",
      "h4",       "h5",    "h6",       NULL};
  for (int i = 0; ok[i]; i++)
    if (strlen(ok[i]) == n && strncasecmp(ok[i], name, n) == 0)
      return 1;
  return 0;
}

/* Attributes we keep. Anything else (class, style, on*, id, ...) is dropped. */
static int attr_ok(const char *name) {
  static const char *ok[] = {"align",  "alt",   "title",  "width",
                             "height", "src",   "href",   "colspan",
                             "rowspan", "loading", "target", "start",
                             "type",   "value", NULL};
  for (int i = 0; ok[i]; i++)
    if (strcasecmp(ok[i], name) == 0)
      return 1;
  return 0;
}

static int is_void_tag(const char *name, size_t n) {
  static const char *voids[] = {"br", "hr", "img", "source", NULL};
  for (int i = 0; voids[i]; i++)
    if (strlen(voids[i]) == n && strncasecmp(voids[i], name, n) == 0)
      return 1;
  return 0;
}

static int is_alnum(char c) { return isalnum((unsigned char)c) != 0; }
static int is_alpha(char c) { return isalpha((unsigned char)c) != 0; }

size_t html_tag_len(const char *s, size_t n) {
  if (n < 3 || s[0] != '<')
    return 0;
  size_t i = 1;
  if (s[i] == '!') {
    /* comment or declaration */
    if (n >= 4 && s[2] == '-' && s[3] == '-') {
      for (size_t e = 4; e + 2 < n; e++)
        if (s[e] == '-' && s[e + 1] == '-' && s[e + 2] == '>')
          return e + 3;
      return 0;
    }
    for (size_t e = 2; e < n; e++)
      if (s[e] == '>')
        return e + 1;
    return 0;
  }
  if (s[i] == '/')
    i++;
  if (i >= n || !is_alpha(s[i]))
    return 0;
  size_t ns = i;
  while (i < n && (is_alnum(s[i]) || s[i] == '-'))
    i++;
  if (i >= n)
    return 0;
  char c = s[i];
  if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' || c == '/'))
    return 0;
  /* find the closing '>', respecting quoted attribute values */
  int q = 0;
  for (size_t e = ns; e < n; e++) {
    char cc = s[e];
    if (q) {
      if (cc == q)
        q = 0;
    } else if (cc == '"' || cc == '\'') {
      q = cc;
    } else if (cc == '>') {
      return e + 1;
    }
  }
  return 0;
}

int html_block_start(const char *line, size_t len) {
  size_t i = 0;
  while (i < len && (line[i] == ' ' || line[i] == '\t'))
    i++;
  if (i >= len || line[i] != '<')
    return 0;
  return html_tag_len(line + i, len - i) != 0;
}

/* Only digits and an optional trailing '%' - enough for width/height/span. */
static int num_ok(const char *v) {
  size_t i = 0;
  if (!v[0])
    return 0;
  for (; v[i]; i++)
    if (!isdigit((unsigned char)v[i]) && v[i] != '%')
      return 0;
  return 1;
}

static int align_ok(const char *v) {
  static const char *ok[] = {"left", "right", "center", "justify",
                             "top",  "middle", "bottom", NULL};
  for (int i = 0; ok[i]; i++)
    if (strcasecmp(ok[i], v) == 0)
      return 1;
  return 0;
}

struct attr {
  char name[32];
  char value[4096];
  int has_value;
};

/* Sanitise one tag (t[0]=='<', t[n-1]=='>') into o. */
static void render_tag(struct strbuf *o, const char *t, size_t n,
                       const char *base) {
  size_t i = 1;
  int closing = 0;
  if (i < n && t[i] == '/') {
    closing = 1;
    i++;
  }
  size_t ns = i;
  while (i < n && (is_alnum(t[i]) || t[i] == '-'))
    i++;
  size_t nl = i - ns;
  if (nl == 0 || !tag_ok(t + ns, nl))
    return; /* unknown tag: drop the tag, keep its text content */

  char name[32];
  size_t cp = nl < sizeof name - 1 ? nl : sizeof name - 1;
  for (size_t k = 0; k < cp; k++)
    name[k] = (char)tolower((unsigned char)t[ns + k]);
  name[cp] = '\0';

  if (closing) {
    if (is_void_tag(name, cp))
      return;
    sb_puts(o, "</");
    sb_puts(o, name);
    sb_putc(o, '>');
    return;
  }

  /* gather attributes */
  struct attr attrs[32];
  int na = 0;
  size_t p = ns + nl;
  while (p < n) {
    while (p < n && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' ||
                     t[p] == '\r'))
      p++;
    if (p >= n || t[p] == '>' || t[p] == '/')
      break;
    size_t as = p;
    while (p < n && t[p] != '=' && t[p] != '>' && t[p] != '/' && t[p] != ' ' &&
           t[p] != '\t' && t[p] != '\n' && t[p] != '\r')
      p++;
    size_t al = p - as;
    if (al == 0) {
      p++;
      continue;
    }
    while (p < n && (t[p] == ' ' || t[p] == '\t'))
      p++;
    int has_value = 0;
    char val[4096];
    size_t vl = 0;
    val[0] = '\0';
    if (p < n && t[p] == '=') {
      p++;
      while (p < n && (t[p] == ' ' || t[p] == '\t'))
        p++;
      if (p < n && (t[p] == '"' || t[p] == '\'')) {
        char qc = t[p++];
        while (p < n && t[p] != qc) {
          if (vl < sizeof val - 1)
            val[vl++] = t[p];
          p++;
        }
        if (p < n)
          p++;
      } else {
        while (p < n && t[p] != ' ' && t[p] != '\t' && t[p] != '\n' &&
               t[p] != '\r' && t[p] != '>') {
          if (vl < sizeof val - 1)
            val[vl++] = t[p];
          p++;
        }
      }
      val[vl] = '\0';
      has_value = 1;
    }
    if (na < 32 && al < sizeof attrs[0].name) {
      for (size_t k = 0; k < al; k++)
        attrs[na].name[k] = (char)tolower((unsigned char)t[as + k]);
      attrs[na].name[al] = '\0';
      snprintf(attrs[na].value, sizeof attrs[na].value, "%s", val);
      attrs[na].has_value = has_value;
      na++;
    }
  }

  /* self-closing? */
  int selfclose = 0;
  {
    size_t e = n - 1; /* '>' */
    while (e > 0 && (t[e - 1] == ' ' || t[e - 1] == '\t'))
      e--;
    if (e > 0 && t[e - 1] == '/')
      selfclose = 1;
  }

  /* find the bits we treat specially */
  const char *src = NULL, *alt = NULL;
  for (int k = 0; k < na; k++) {
    if (strcmp(attrs[k].name, "src") == 0 && attrs[k].has_value)
      src = attrs[k].value;
    else if (strcmp(attrs[k].name, "alt") == 0 && attrs[k].has_value)
      alt = attrs[k].value;
  }

  /* an <img> whose src is remote cannot load offline: show a badge chip */
  if (strcmp(name, "img") == 0 && src && url_is_remote(src, strlen(src))) {
    sb_puts(o, "<span class=\"badge\" title=\"");
    sb_put_html_escaped(o, src, strlen(src));
    sb_puts(o, "\">");
    if (alt && alt[0])
      sb_put_html_escaped(o, alt, strlen(alt));
    else
      sb_puts(o, "image");
    sb_puts(o, "</span>");
    return;
  }

  sb_putc(o, '<');
  sb_puts(o, name);
  for (int k = 0; k < na; k++) {
    const char *an = attrs[k].name;
    if (!attr_ok(an))
      continue;

    if (strcmp(an, "src") == 0 || strcmp(an, "href") == 0) {
      if (!attrs[k].has_value || !attrs[k].value[0])
        continue;
      if (strcmp(an, "href") == 0 && url_has_scheme(attrs[k].value) &&
          !url_is_remote(attrs[k].value, strlen(attrs[k].value)) &&
          strncasecmp(attrs[k].value, "mailto:", 7) != 0)
        continue; /* reject javascript:, data:, file: and friends */
      char *u = url_to_local(attrs[k].value, base);
      sb_putc(o, ' ');
      sb_puts(o, an);
      sb_puts(o, "=\"");
      sb_put_html_escaped(o, u, strlen(u));
      sb_putc(o, '"');
      free(u);
      continue;
    }
    if (strcmp(an, "width") == 0 || strcmp(an, "height") == 0 ||
        strcmp(an, "colspan") == 0 || strcmp(an, "rowspan") == 0) {
      if (!attrs[k].has_value || !num_ok(attrs[k].value))
        continue;
      sb_putc(o, ' ');
      sb_puts(o, an);
      sb_puts(o, "=\"");
      sb_put_html_escaped(o, attrs[k].value, strlen(attrs[k].value));
      sb_putc(o, '"');
      continue;
    }
    if (strcmp(an, "align") == 0) {
      if (!attrs[k].has_value || !align_ok(attrs[k].value))
        continue;
      sb_puts(o, " align=\"");
      sb_put_html_escaped(o, attrs[k].value, strlen(attrs[k].value));
      sb_putc(o, '"');
      continue;
    }
    if (strcmp(an, "target") == 0) {
      if (!attrs[k].has_value || strcmp(attrs[k].value, "_blank") != 0)
        continue;
      sb_puts(o, " target=\"_blank\" rel=\"noopener noreferrer\"");
      continue;
    }
    if (strcmp(an, "loading") == 0) {
      if (!attrs[k].has_value ||
          (strcasecmp(attrs[k].value, "lazy") != 0 &&
           strcasecmp(attrs[k].value, "eager") != 0))
        continue;
      sb_puts(o, " loading=\"");
      sb_put_html_escaped(o, attrs[k].value, strlen(attrs[k].value));
      sb_putc(o, '"');
      continue;
    }
    /* alt, title, start, type, value: escaped verbatim */
    sb_putc(o, ' ');
    sb_puts(o, an);
    if (attrs[k].has_value) {
      sb_puts(o, "=\"");
      sb_put_html_escaped(o, attrs[k].value, strlen(attrs[k].value));
      sb_putc(o, '"');
    }
  }
  (void)selfclose;
  sb_putc(o, '>');
}

void html_to_html(struct strbuf *out, const char *text, size_t len,
                  const char *base) {
  size_t i = 0;
  while (i < len) {
    if (text[i] == '<') {
      size_t tl = html_tag_len(text + i, len - i);
      if (tl) {
        render_tag(out, text + i, tl, base);
        i += tl;
        continue;
      }
    }
    size_t j = i;
    while (j < len && text[j] != '<')
      j++;
    if (j == i)
      j = i + 1; /* a lone '<' that does not start a tag */
    sb_put_html_escaped(out, text + i, j - i);
    i = j;
  }
}
