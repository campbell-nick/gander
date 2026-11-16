/* md.c - a small, practical Markdown -> HTML renderer.
 *
 * It is deliberately a subset: headings, paragraphs, fenced code, block
 * quotes, (nested) lists, task lists, GFM tables, horizontal rules, and the
 * usual inline spans. Relative links and images are rewritten to /raw/<path>
 * so they resolve against the local repository. Not full CommonMark.
 */
#include "gander.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------- helpers --- */

static void esc(struct strbuf *o, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    switch (s[i]) {
    case '&':
      sb_puts(o, "&amp;");
      break;
    case '<':
      sb_puts(o, "&lt;");
      break;
    case '>':
      sb_puts(o, "&gt;");
      break;
    case '"':
      sb_puts(o, "&quot;");
      break;
    default:
      sb_putc(o, s[i]);
    }
  }
}

static int is_blank(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r')
      return 0;
  return 1;
}

static size_t leading_ws(const char *s, size_t n) {
  size_t i = 0;
  while (i < n && (s[i] == ' ' || s[i] == '\t'))
    i++;
  return i;
}

static void trim(const char **s, size_t *n) {
  while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t' ||
                (*s)[*n - 1] == '\r'))
    (*n)--;
}

static int has_scheme(const char *u) {
  const char *p = u;
  if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
    return 0;
  p++;
  while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
         (*p >= '0' && *p <= '9') || *p == '+' || *p == '.' || *p == '-')
    p++;
  return *p == ':';
}

/* Collapse "." and ".." segments in a relative path. */
static void normalize(const char *in, char *out, size_t outsz) {
  const char *parts[256];
  size_t plen[256];
  int np = 0;
  const char *p = in;
  while (*p) {
    while (*p == '/')
      p++;
    if (!*p)
      break;
    const char *start = p;
    while (*p && *p != '/')
      p++;
    size_t len = (size_t)(p - start);
    if (len == 1 && start[0] == '.')
      continue;
    if (len == 2 && start[0] == '.' && start[1] == '.') {
      if (np > 0)
        np--;
      continue;
    }
    if (np < 256) {
      parts[np] = start;
      plen[np] = len;
      np++;
    }
  }
  size_t o = 0;
  for (int i = 0; i < np; i++) {
    if (i && o < outsz - 1)
      out[o++] = '/';
    for (size_t j = 0; j < plen[i] && o < outsz - 1; j++)
      out[o++] = parts[i][j];
  }
  out[o] = '\0';
}

/* Turn a link/image target into something the browser can fetch. */
static char *resolve_url(const char *url, const char *base) {
  if (url[0] == '#' || url[0] == '/' || has_scheme(url))
    return xstrdup(url);
  char joined[4096];
  if (base[0])
    snprintf(joined, sizeof joined, "%s/%s", base, url);
  else
    snprintf(joined, sizeof joined, "%s", url);
  char norm[4096];
  normalize(joined, norm, sizeof norm);
  char *res = xmalloc(strlen(norm) + 8);
  sprintf(res, "/raw/%s", norm);
  return res;
}

static void put_attr_url(struct strbuf *o, const char *url, const char *base) {
  char *r = resolve_url(url, base);
  esc(o, r, strlen(r));
  free(r);
}

/* -------------------------------------------------------------- inline --- */

static void inline_md(struct strbuf *o, const char *s, size_t n,
                      const char *base);

static size_t find_seq(const char *s, size_t n, const char *seq, size_t sl) {
  if (sl == 0 || sl > n)
    return (size_t)-1;
  for (size_t i = 0; i + sl <= n; i++)
    if (memcmp(s + i, seq, sl) == 0)
      return i;
  return (size_t)-1;
}

static void inline_md(struct strbuf *o, const char *s, size_t n,
                      const char *base) {
  size_t i = 0;
  while (i < n) {
    char c = s[i];

    /* backslash escape */
    if (c == '\\' && i + 1 < n) {
      esc(o, s + i + 1, 1);
      i += 2;
      continue;
    }

    /* code span */
    if (c == '`') {
      size_t run = 1;
      while (i + run < n && s[i + run] == '`')
        run++;
      size_t j = i + run, close = (size_t)-1;
      while (j + run <= n) {
        if (memcmp(s + j, s + i, run) == 0) {
          close = j;
          break;
        }
        j++;
      }
      if (close != (size_t)-1) {
        sb_puts(o, "<code>");
        esc(o, s + i + run, close - (i + run));
        sb_puts(o, "</code>");
        i = close + run;
        continue;
      }
    }

    /* image */
    if (c == '!' && i + 1 < n && s[i + 1] == '[') {
      size_t close = i + 2;
      while (close < n && s[close] != ']')
        close++;
      if (close < n && close + 1 < n && s[close + 1] == '(') {
        size_t end = close + 2;
        while (end < n && s[end] != ')')
          end++;
        if (end < n) {
          const char *alt = s + i + 2;
          size_t altlen = close - (i + 2);
          const char *url = s + close + 2;
          size_t urllen = end - (close + 2);
          /* strip an optional "title" */
          const char *sp = memchr(url, ' ', urllen);
          if (sp)
            urllen = (size_t)(sp - url);
          sb_puts(o, "<img src=\"");
          char *u = xstrndup(url, urllen);
          put_attr_url(o, u, base);
          free(u);
          sb_puts(o, "\" alt=\"");
          esc(o, alt, altlen);
          sb_puts(o, "\" loading=\"lazy\">");
          i = end + 1;
          continue;
        }
      }
    }

    /* link */
    if (c == '[') {
      size_t close = i + 1;
      while (close < n && s[close] != ']')
        close++;
      if (close < n && close + 1 < n && s[close + 1] == '(') {
        size_t end = close + 2;
        while (end < n && s[end] != ')')
          end++;
        if (end < n) {
          const char *text = s + i + 1;
          size_t textlen = close - (i + 1);
          const char *url = s + close + 2;
          size_t urllen = end - (close + 2);
          const char *sp = memchr(url, ' ', urllen);
          if (sp)
            urllen = (size_t)(sp - url);
          sb_puts(o, "<a href=\"");
          char *u = xstrndup(url, urllen);
          put_attr_url(o, u, base);
          free(u);
          sb_puts(o, "\">");
          inline_md(o, text, textlen, base);
          sb_puts(o, "</a>");
          i = end + 1;
          continue;
        }
      }
    }

    /* autolink */
    if (c == '<') {
      size_t close = i + 1;
      while (close < n && s[close] != '>' && s[close] != ' ' &&
             s[close] != '\n')
        close++;
      if (close < n && s[close] == '>') {
        const char *inner = s + i + 1;
        size_t ilen = close - (i + 1);
        int is_url = (ilen > 7 && strncasecmp(inner, "http://", 7) == 0) ||
                     (ilen > 8 && strncasecmp(inner, "https://", 8) == 0);
        int is_mail = memchr(inner, '@', ilen) != NULL &&
                      memchr(inner, ':', ilen) == NULL;
        if (is_url || is_mail) {
          char *u = xstrndup(inner, ilen);
          char *href = is_mail ? NULL : u;
          char mailbuf[4200];
          if (is_mail) {
            snprintf(mailbuf, sizeof mailbuf, "mailto:%s", u);
            href = mailbuf;
          }
          sb_puts(o, "<a href=\"");
          esc(o, href, strlen(href));
          sb_puts(o, "\">");
          esc(o, inner, ilen);
          sb_puts(o, "</a>");
          free(u);
          i = close + 1;
          continue;
        }
      }
    }

    /* strong / emphasis / strike */
    if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c) {
      size_t close = find_seq(s + i + 2, n - (i + 2), c == '*' ? "**" : "__", 2);
      if (close != (size_t)-1) {
        sb_puts(o, "<strong>");
        inline_md(o, s + i + 2, close, base);
        sb_puts(o, "</strong>");
        i = i + 2 + close + 2;
        continue;
      }
    }
    if (c == '*' || c == '_') {
      size_t close = find_seq(s + i + 1, n - (i + 1), c == '*' ? "*" : "_", 1);
      if (close != (size_t)-1 && close > 0) {
        sb_puts(o, "<em>");
        inline_md(o, s + i + 1, close, base);
        sb_puts(o, "</em>");
        i = i + 1 + close + 1;
        continue;
      }
    }
    if (c == '~' && i + 1 < n && s[i + 1] == '~') {
      size_t close = find_seq(s + i + 2, n - (i + 2), "~~", 2);
      if (close != (size_t)-1) {
        sb_puts(o, "<del>");
        inline_md(o, s + i + 2, close, base);
        sb_puts(o, "</del>");
        i = i + 2 + close + 2;
        continue;
      }
    }

    esc(o, s + i, 1);
    i++;
  }
}

/* --------------------------------------------------------------- blocks --- */

struct lines {
  char **v;
  size_t n;
  char *buf;
};

static void split_lines(const char *text, size_t len, struct lines *L) {
  L->buf = xstrndup(text, len);
  L->v = NULL;
  L->n = 0;
  size_t cap = 0;
  char *p = L->buf;
  while (p) {
    if (L->n == cap) {
      cap = cap ? cap * 2 : 64;
      L->v = xrealloc(L->v, cap * sizeof(char *));
    }
    L->v[L->n++] = p;
    char *nl = strchr(p, '\n');
    if (!nl)
      break;
    *nl = '\0';
    p = nl + 1;
  }
}

static void free_lines(struct lines *L) {
  free(L->v);
  free(L->buf);
}

static int is_fence(const char *l, size_t n, size_t *indent, char *ch,
                    size_t *run) {
  size_t i = leading_ws(l, n);
  if (i + 2 >= n + 1 && n < 3)
    return 0;
  if (i >= n)
    return 0;
  char c = l[i];
  if (c != '`' && c != '~')
    return 0;
  size_t r = 0;
  while (i + r < n && l[i + r] == c)
    r++;
  if (r < 3)
    return 0;
  *indent = i;
  *ch = c;
  *run = r;
  return 1;
}

static int is_hr(const char *l, size_t n) {
  size_t i = leading_ws(l, n);
  if (i >= n)
    return 0;
  char c = l[i];
  if (c != '-' && c != '*' && c != '_')
    return 0;
  int count = 0;
  for (; i < n; i++) {
    if (l[i] == c)
      count++;
    else if (l[i] != ' ' && l[i] != '\t')
      return 0;
  }
  return count >= 3;
}

static int list_marker(const char *l, size_t n, size_t *indent, int *ordered,
                       size_t *content_off) {
  size_t i = leading_ws(l, n);
  if (i >= n)
    return 0;
  if ((l[i] == '-' || l[i] == '*' || l[i] == '+') && i + 1 < n &&
      (l[i + 1] == ' ' || l[i + 1] == '\t')) {
    *indent = i;
    *ordered = 0;
    *content_off = i + 2;
    return 1;
  }
  if (l[i] >= '0' && l[i] <= '9') {
    size_t j = i;
    while (j < n && l[j] >= '0' && l[j] <= '9')
      j++;
    if (j < n && (l[j] == '.' || l[j] == ')') && j + 1 < n &&
        (l[j + 1] == ' ' || l[j + 1] == '\t')) {
      *indent = i;
      *ordered = 1;
      *content_off = j + 2;
      return 1;
    }
  }
  return 0;
}

static int is_table_sep(const char *l, size_t n) {
  size_t i = leading_ws(l, n);
  int dashes = 0, pipes = 0;
  for (; i < n; i++) {
    if (l[i] == '-')
      dashes++;
    else if (l[i] == '|')
      pipes++;
    else if (l[i] == ':' || l[i] == ' ' || l[i] == '\t')
      ;
    else
      return 0;
  }
  return dashes >= 1 && pipes >= 1;
}

static void render_table(struct strbuf *o, struct lines *L, size_t *ip,
                         const char *base) {
  const char *hdr = L->v[*ip];
  size_t hn = strlen(hdr);
  (void)hn;

  /* split a row into cells, writing them into out */
  sb_puts(o, "<table><thead><tr>");
  const char *p = hdr;
  size_t len = strlen(p);
  size_t i = leading_ws(p, len);
  size_t start = i;
  for (; i <= len; i++) {
    if (i == len || p[i] == '|') {
      size_t cs = start, ce = i;
      while (cs < ce && (p[cs] == ' ' || p[cs] == '\t'))
        cs++;
      while (ce > cs && (p[ce - 1] == ' ' || p[ce - 1] == '\t'))
        ce--;
      if (!(start == leading_ws(p, len) && ce == cs)) { /* skip edge blanks */
        sb_puts(o, "<th>");
        inline_md(o, p + cs, ce - cs, base);
        sb_puts(o, "</th>");
      }
      start = i + 1;
    }
  }
  sb_puts(o, "</tr></thead><tbody>");
  (*ip) += 2; /* header + separator */
  while (*ip < L->n) {
    const char *row = L->v[*ip];
    size_t rlen = strlen(row);
    if (is_blank(row, rlen) || !memchr(row, '|', rlen))
      break;
    sb_puts(o, "<tr>");
    size_t j = leading_ws(row, rlen), s = j;
    for (; j <= rlen; j++) {
      if (j == rlen || row[j] == '|') {
        size_t cs = s, ce = j;
        while (cs < ce && (row[cs] == ' ' || row[cs] == '\t'))
          cs++;
        while (ce > cs && (row[ce - 1] == ' ' || row[ce - 1] == '\t'))
          ce--;
        if (!(s == leading_ws(row, rlen) && ce == cs)) {
          sb_puts(o, "<td>");
          inline_md(o, row + cs, ce - cs, base);
          sb_puts(o, "</td>");
        }
        s = j + 1;
      }
    }
    sb_puts(o, "</tr>");
    (*ip)++;
  }
  sb_puts(o, "</tbody></table>");
}

static void render_list(struct strbuf *o, struct lines *L, size_t *ip,
                        const char *base) {
  size_t indent_stack[32];
  int type_stack[32];
  int depth = 0;
  int li_open = 0;

  size_t ind, coff;
  int ord;
  list_marker(L->v[*ip], strlen(L->v[*ip]), &ind, &ord, &coff);
  sb_puts(o, ord ? "<ol>" : "<ul>");
  indent_stack[0] = ind;
  type_stack[0] = ord;

  while (*ip < L->n) {
    const char *l = L->v[*ip];
    size_t n = strlen(l);
    if (!list_marker(l, n, &ind, &ord, &coff))
      break;

    if (ind > indent_stack[depth]) {
      sb_puts(o, ord ? "<ol>" : "<ul>");
      depth++;
      indent_stack[depth] = ind;
      type_stack[depth] = ord;
      li_open = 0;
    } else {
      while (depth > 0 && ind < indent_stack[depth]) {
        if (li_open) {
          sb_puts(o, "</li>");
          li_open = 0;
        }
        sb_puts(o, type_stack[depth] ? "</ol>" : "</ul>");
        depth--;
      }
      if (li_open) {
        sb_puts(o, "</li>");
        li_open = 0;
      }
      if (ind == indent_stack[depth] && ord != type_stack[depth]) {
        sb_puts(o, type_stack[depth] ? "</ol>" : "</ul>");
        sb_puts(o, ord ? "<ol>" : "<ul>");
        type_stack[depth] = ord;
      }
    }

    const char *content = l + coff;
    size_t clen = n - coff;
    trim(&content, &clen);

    sb_puts(o, "<li>");
    /* task list? */
    if (clen >= 3 && content[0] == '[' && content[2] == ']' &&
        (content[1] == ' ' || content[1] == 'x' || content[1] == 'X')) {
      int done = content[1] != ' ';
      sb_puts(o, done ? "<input type=\"checkbox\" disabled checked> "
                      : "<input type=\"checkbox\" disabled> ");
      content += 3;
      clen -= 3;
      while (clen && *content == ' ') {
        content++;
        clen--;
      }
    }
    inline_md(o, content, clen, base);
    li_open = 1;
    (*ip)++;

    /* continuation lines: more-indented, not a new item, not blank */
    while (*ip < L->n) {
      const char *c = L->v[*ip];
      size_t cn = strlen(c);
      size_t ci;
      int co;
      size_t cc;
      if (is_blank(c, cn) || list_marker(c, cn, &ci, &co, &cc))
        break;
      if (leading_ws(c, cn) <= indent_stack[depth])
        break;
      sb_putc(o, ' ');
      const char *ct = c + leading_ws(c, cn);
      inline_md(o, ct, cn - leading_ws(c, cn), base);
      (*ip)++;
    }
  }
  if (li_open)
    sb_puts(o, "</li>");
  while (depth > 0) {
    sb_puts(o, type_stack[depth] ? "</ol>" : "</ul>");
    depth--;
  }
  sb_puts(o, type_stack[0] ? "</ol>" : "</ul>");
}

void md_to_html(struct strbuf *o, const char *text, size_t len,
                const char *base) {
  struct lines L;
  split_lines(text, len, &L);
  size_t i = 0;
  while (i < L.n) {
    const char *l = L.v[i];
    size_t n = strlen(l);

    if (is_blank(l, n)) {
      i++;
      continue;
    }

    /* fenced code */
    size_t fi, run;
    char fch;
    if (is_fence(l, n, &fi, &fch, &run)) {
      const char *info = l + fi + run;
      size_t infolen = n - (fi + run);
      const char *il = info;
      size_t ill = infolen;
      trim(&il, &ill);
      while (ill && (*il == ' ' || *il == '\t')) {
        il++;
        ill--;
      }
      /* language is the first word */
      size_t langlen = 0;
      while (langlen < ill && il[langlen] != ' ' && il[langlen] != '\t')
        langlen++;
      sb_puts(o, "<pre><code");
      if (langlen) {
        sb_puts(o, " class=\"language-");
        esc(o, il, langlen);
        sb_puts(o, "\"");
      }
      sb_puts(o, ">");
      i++;
      while (i < L.n) {
        size_t cn = strlen(L.v[i]);
        size_t ci2, r2;
        char c2;
        if (is_fence(L.v[i], cn, &ci2, &c2, &r2) && c2 == fch && r2 >= run)
          break;
        esc(o, L.v[i], cn);
        sb_putc(o, '\n');
        i++;
      }
      if (i < L.n)
        i++; /* closing fence */
      sb_puts(o, "</code></pre>");
      continue;
    }

    /* ATX heading */
    if (l[0] == '#') {
      size_t h = 0;
      while (h < n && l[h] == '#')
        h++;
      if (h <= 6 && (h == n || l[h] == ' ' || l[h] == '\t')) {
        const char *t = l + h;
        size_t tn = n - h;
        trim(&t, &tn);
        while (tn && (*t == ' ' || *t == '\t')) {
          t++;
          tn--;
        }
        /* drop trailing #'s */
        while (tn && t[tn - 1] == '#')
          tn--;
        while (tn && (t[tn - 1] == ' ' || t[tn - 1] == '\t'))
          tn--;
        sb_printf(o, "<h%zu>", h);
        inline_md(o, t, tn, base);
        sb_printf(o, "</h%zu>", h);
        i++;
        continue;
      }
    }

    /* setext heading */
    if (i + 1 < L.n) {
      const char *nx = L.v[i + 1];
      size_t nn = strlen(nx);
      size_t ni = leading_ws(nx, nn);
      if (ni < nn && (nx[ni] == '=' || nx[ni] == '-')) {
        char c = nx[ni];
        size_t k = ni;
        int ok = 1;
        while (k < nn) {
          if (nx[k] != c && nx[k] != ' ' && nx[k] != '\t')
            ok = 0;
          k++;
        }
        if (ok && !is_hr(l, n)) {
          int hh = (c == '=') ? 1 : 2;
          sb_printf(o, "<h%d>", hh);
          const char *t = l;
          size_t tn = n;
          trim(&t, &tn);
          inline_md(o, t, tn, base);
          sb_printf(o, "</h%d>", hh);
          i += 2;
          continue;
        }
      }
    }

    /* horizontal rule */
    if (is_hr(l, n)) {
      sb_puts(o, "<hr>");
      i++;
      continue;
    }

    /* blockquote */
    if (l[0] == '>') {
      struct strbuf inner;
      sb_init(&inner);
      while (i < L.n) {
        const char *q = L.v[i];
        size_t qn = strlen(q);
        if (q[0] != '>')
          break;
        const char *t = q + 1;
        size_t tn = qn - 1;
        if (tn && *t == ' ')
          t++;
        tn = tn ? tn - (t - (q + 1)) : 0;
        sb_putn(&inner, t, tn);
        sb_putc(&inner, '\n');
        i++;
      }
      sb_puts(o, "<blockquote>");
      md_to_html(o, inner.buf ? inner.buf : "", inner.len, base);
      sb_puts(o, "</blockquote>");
      sb_free(&inner);
      continue;
    }

    /* table */
    if (i + 1 < L.n && strchr(l, '|') && is_table_sep(L.v[i + 1],
                                                       strlen(L.v[i + 1]))) {
      render_table(o, &L, &i, base);
      continue;
    }

    /* list */
    size_t li, lc;
    int lo;
    if (list_marker(l, n, &li, &lo, &lc)) {
      render_list(o, &L, &i, base);
      continue;
    }

    /* paragraph */
    sb_puts(o, "<p>");
    int first = 1;
    while (i < L.n) {
      const char *pl = L.v[i];
      size_t pn = strlen(pl);
      if (is_blank(pl, pn))
        break;
      if (!first)
        sb_putc(o, '\n');
      /* stop if this line starts a new block */
      if (!first) {
        size_t hi, ri;
        char hc;
        size_t mi, mo;
        int moo;
        if (pl[0] == '#' || pl[0] == '>' || is_hr(pl, pn) ||
            is_fence(pl, pn, &hi, &hc, &ri) ||
            list_marker(pl, pn, &mi, &moo, &mo))
          break;
      }
      inline_md(o, pl, pn, base);
      first = 0;
      i++;
    }
    sb_puts(o, "</p>");
  }
  free_lines(&L);
}
