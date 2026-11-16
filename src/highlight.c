/* highlight.c - a small, generic source highlighter that emits HTML spans.
 *
 * This is deliberately not a full parser. It tokenises comments, strings,
 * numbers, keywords, preprocessor lines and function calls well enough to
 * look like GitHub's code view, and it knows the comment/string rules and
 * keyword sets for a handful of common languages. Anything unknown falls
 * back to a C-like tokeniser.
 */
#include "gander.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* --------------------------------------------------------------- escaping - */

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

static void span(struct strbuf *o, const char *cls, const char *s, size_t n) {
  if (n == 0)
    return;
  sb_printf(o, "<span class=\"%s\">", cls);
  esc(o, s, n);
  sb_puts(o, "</span>");
}

/* --------------------------------------------------------------- keywords - */

static const char *const KW_C[] = {
    "auto",     "break",    "case",     "char",   "const",  "continue",
    "default",  "do",       "double",   "else",   "enum",   "extern",
    "float",    "for",      "goto",     "if",     "inline", "int",
    "long",     "register", "restrict", "return", "short",  "signed",
    "sizeof",   "static",   "struct",   "switch", "typedef", "union",
    "unsigned", "void",     "volatile", "while",  "bool",   "true",
    "false",    "NULL",     "size_t",   NULL};

static const char *const KW_CPP[] = {
    "class",      "namespace", "template", "typename", "public",
    "private",    "protected", "virtual",  "new",      "delete",
    "this",       "using",     "try",      "catch",    "throw",
    "constexpr",  "nullptr",   "override", "final",    "friend",
    "operator",   "explicit",  "mutable",  "static_cast", "dynamic_cast",
    "const_cast", "reinterpret_cast", NULL};

static const char *const KW_JS[] = {
    "break",      "case",     "catch",   "class",    "const",   "continue",
    "debugger",   "default",  "delete",  "do",       "else",    "export",
    "extends",    "finally",  "for",     "function", "if",      "import",
    "in",         "instanceof", "let",   "new",      "return",  "super",
    "switch",     "this",     "throw",   "try",      "typeof",  "var",
    "void",       "while",    "with",    "yield",    "async",   "await",
    "of",         "static",   "get",     "set",      "true",    "false",
    "null",       "undefined", NULL};

static const char *const KW_TS[] = {
    "interface", "type",   "enum",     "implements", "declare",
    "readonly",  "abstract", "as",     "any",        "string",
    "number",    "boolean", "never",   "unknown",    "keyof",
    "infer",     "is",      "namespace", "module",   "public",
    "private",   "protected", "override", NULL};

static const char *const KW_PY[] = {
    "False", "None",   "True",   "and",    "as",     "assert", "async",
    "await", "break",  "class",  "continue", "def",  "del",    "elif",
    "else",  "except", "finally", "for",   "from",   "global", "if",
    "import", "in",    "is",     "lambda", "nonlocal", "not",  "or",
    "pass",  "raise",  "return", "try",    "while",  "with",   "yield",
    "self",  NULL};

static const char *const KW_RUST[] = {
    "as",    "async",  "await",  "break", "const",  "continue", "crate",
    "dyn",   "else",   "enum",   "extern", "false", "fn",       "for",
    "if",    "impl",   "in",     "let",   "loop",   "match",    "mod",
    "move",  "mut",    "pub",    "ref",   "return", "self",     "Self",
    "static", "struct", "super", "trait", "true",   "type",     "unsafe",
    "use",   "where",  "while",  NULL};

static const char *const KW_GO[] = {
    "break",   "case",  "chan",  "const",  "continue", "default",
    "defer",   "else",  "fallthrough", "for", "func", "go",     "goto",
    "if",      "import", "interface", "map", "package", "range", "return",
    "select",  "struct", "switch", "type",  "var",     "true",  "false",
    "nil",     NULL};

static const char *const KW_SH[] = {
    "if",     "then",   "else",  "elif",   "fi",     "for",    "while",
    "do",     "done",   "case",  "esac",   "function", "in",   "select",
    "until",  "return", "break", "continue", "local", "export", "readonly",
    "declare", "typeset", "unset", "shift", "source", "alias",  "echo",
    "set",    "trap",   "exit",  NULL};

static const char *const KW_MAKE[] = {
    "ifeq", "ifneq", "ifdef", "ifndef", "else", "endif", "include",
    "define", "endef", "export", "unexport", "override", NULL};

static const char *const KW_NIX[] = {
    "let", "in", "with", "rec", "inherit", "import", "if", "then", "else",
    "assert", "true", "false", "null", NULL};

/* --------------------------------------------------------------- langdef - */

struct langdef {
  const char *const *kw;
  int hash_comment;  /* '#' begins a line comment (shell, python, make) */
  int slash_comment; /* C-family comments: slash-slash and slash-star */
  int dash_comment;  /* '--' line comment (sql, lua, haskell) */
  int preproc;       /* '#' at the start of a line is a preprocessor */
  int triple;        /* triple-quoted multi-line strings (python) */
  int markup;        /* handled by the HTML tokeniser */
};

static const struct langdef LD_C = {KW_C, 0, 1, 0, 1, 0, 0};
static const struct langdef LD_CPP = {KW_CPP, 0, 1, 0, 1, 0, 0};
static const struct langdef LD_JS = {KW_JS, 0, 1, 0, 0, 0, 0};
static const struct langdef LD_TS = {KW_TS, 0, 1, 0, 0, 0, 0};
static const struct langdef LD_PY = {KW_PY, 1, 0, 0, 0, 1, 0};
static const struct langdef LD_RUST = {KW_RUST, 0, 1, 0, 0, 0, 0};
static const struct langdef LD_GO = {KW_GO, 0, 1, 0, 0, 0, 0};
static const struct langdef LD_SH = {KW_SH, 1, 0, 0, 0, 0, 0};
static const struct langdef LD_MAKE = {KW_MAKE, 1, 0, 0, 0, 0, 0};
static const struct langdef LD_NIX = {KW_NIX, 1, 1, 0, 0, 0, 0};
static const struct langdef LD_GENERIC = {KW_C, 0, 1, 0, 0, 0, 0};
static const struct langdef LD_JSON = {NULL, 0, 0, 0, 0, 0, 0};
static const struct langdef LD_YAML = {NULL, 1, 0, 0, 0, 0, 0};
static const struct langdef LD_TOML = {NULL, 1, 0, 0, 0, 0, 0};
static const struct langdef LD_CSS = {NULL, 0, 1, 0, 0, 0, 0};
static const struct langdef LD_HTML = {NULL, 0, 0, 0, 0, 0, 1};
static const struct langdef LD_SQL = {NULL, 0, 0, 1, 0, 0, 0};

static int is_kw(const struct langdef *ld, const char *s, size_t n) {
  if (!ld->kw)
    return 0;
  for (size_t i = 0; ld->kw[i]; i++)
    if (strlen(ld->kw[i]) == n && strncmp(ld->kw[i], s, n) == 0)
      return 1;
  return 0;
}

/* --------------------------------------------------------------- detect --- */

static const struct langdef *pick(const char *path, const char **name) {
  const char *dot = strrchr(path, '.');
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  const char *ext = dot && (!base || dot > base) ? dot + 1 : "";

  struct {
    const char *ext;
    const struct langdef *ld;
    const char *name;
  } T[] = {
      {"c", &LD_C, "c"},           {"h", &LD_C, "c"},
      {"cc", &LD_CPP, "cpp"},      {"cpp", &LD_CPP, "cpp"},
      {"cxx", &LD_CPP, "cpp"},     {"hpp", &LD_CPP, "cpp"},
      {"hh", &LD_CPP, "cpp"},      {"hxx", &LD_CPP, "cpp"},
      {"js", &LD_JS, "javascript"}, {"mjs", &LD_JS, "javascript"},
      {"cjs", &LD_JS, "javascript"}, {"jsx", &LD_JS, "javascript"},
      {"ts", &LD_TS, "typescript"}, {"tsx", &LD_TS, "typescript"},
      {"py", &LD_PY, "python"},    {"rs", &LD_RUST, "rust"},
      {"go", &LD_GO, "go"},        {"sh", &LD_SH, "shell"},
      {"bash", &LD_SH, "shell"},   {"zsh", &LD_SH, "shell"},
      {"fish", &LD_SH, "shell"},   {"mk", &LD_MAKE, "make"},
      {"nix", &LD_NIX, "nix"},     {"json", &LD_JSON, "json"},
      {"yml", &LD_YAML, "yaml"},   {"yaml", &LD_YAML, "yaml"},
      {"toml", &LD_TOML, "toml"},  {"css", &LD_CSS, "css"},
      {"html", &LD_HTML, "html"},  {"htm", &LD_HTML, "html"},
      {"xml", &LD_HTML, "xml"},    {"svg", &LD_HTML, "xml"},
      {"sql", &LD_SQL, "sql"},
  };

  for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
    if (strcasecmp(ext, T[i].ext) == 0) {
      *name = T[i].name;
      return T[i].ld;
    }

  if (strcmp(base, "Makefile") == 0 || strcmp(base, "makefile") == 0) {
    *name = "make";
    return &LD_MAKE;
  }
  if (strcmp(base, "Dockerfile") == 0) {
    *name = "dockerfile";
    return &LD_SH;
  }
  if (strcmp(base, "LICENSE") == 0 || strcmp(base, "README") == 0) {
    *name = "text";
    return &LD_GENERIC;
  }
  *name = "text";
  return &LD_GENERIC;
}

const char *highlight_lang_name(const char *path) {
  const char *name;
  pick(path, &name);
  return name;
}

/* -------------------------------------------------------------- scanning -- */

static int ident_start(unsigned char c) {
  return isalpha(c) || c == '_' || c >= 0x80;
}
static int ident_char(unsigned char c) {
  return isalnum(c) || c == '_' || c >= 0x80;
}

static void number_tok(struct strbuf *o, const char *s, size_t n, size_t *i) {
  size_t j = *i;
  if (s[j] == '0' && j + 1 < n && (s[j + 1] == 'x' || s[j + 1] == 'X' ||
                                   s[j + 1] == 'b' || s[j + 1] == 'B' ||
                                   s[j + 1] == 'o' || s[j + 1] == 'O'))
    j += 2;
  while (j < n) {
    unsigned char c = (unsigned char)s[j];
    if (isalnum(c) || c == '.' || c == '_')
      j++;
    else if ((c == '+' || c == '-') && j > *i &&
             (s[j - 1] == 'e' || s[j - 1] == 'E'))
      j++;
    else
      break;
  }
  span(o, "tok-num", s + *i, j - *i);
  *i = j;
}

/* Emit a C-like token stream. */
static void scan_generic(struct strbuf *o, const char *s, size_t n,
                         const struct langdef *ld) {
  size_t i = 0;
  int line_start = 1;

  while (i < n) {
    unsigned char c = (unsigned char)s[i];

    if (c == '\n') {
      sb_putc(o, '\n');
      i++;
      line_start = 1;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      sb_putc(o, (char)c);
      i++;
      continue;
    }

    /* preprocessor line */
    if (ld->preproc && c == '#' && line_start) {
      size_t j = i;
      while (j < n && s[j] != '\n')
        j++;
      span(o, "tok-pre", s + i, j - i);
      i = j;
      line_start = 0;
      continue;
    }

    /* line comments */
    if (ld->hash_comment && c == '#') {
      size_t j = i;
      while (j < n && s[j] != '\n')
        j++;
      span(o, "tok-com", s + i, j - i);
      i = j;
      line_start = 0;
      continue;
    }
    if (ld->dash_comment && c == '-' && i + 1 < n && s[i + 1] == '-') {
      size_t j = i;
      while (j < n && s[j] != '\n')
        j++;
      span(o, "tok-com", s + i, j - i);
      i = j;
      line_start = 0;
      continue;
    }
    if (ld->slash_comment && c == '/' && i + 1 < n && s[i + 1] == '/') {
      size_t j = i;
      while (j < n && s[j] != '\n')
        j++;
      span(o, "tok-com", s + i, j - i);
      i = j;
      line_start = 0;
      continue;
    }
    if (ld->slash_comment && c == '/' && i + 1 < n && s[i + 1] == '*') {
      size_t j = i + 2;
      while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/'))
        j++;
      j = (j + 1 < n) ? j + 2 : n;
      span(o, "tok-com", s + i, j - i);
      i = j;
      line_start = 0;
      continue;
    }

    /* strings */
    if (c == '"' || c == '\'') {
      if (ld->triple && i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
        size_t j = i + 3;
        while (j + 2 < n && !(s[j] == c && s[j + 1] == c && s[j + 2] == c))
          j++;
        j = (j + 2 < n) ? j + 3 : n;
        span(o, "tok-str", s + i, j - i);
        i = j;
        line_start = 0;
        continue;
      }
      size_t j = i + 1;
      while (j < n) {
        if (s[j] == '\\' && j + 1 < n) {
          j += 2;
          continue;
        }
        if (s[j] == c)
          break;
        if (s[j] == '\n')
          break; /* don't swallow the rest of the file */
        j++;
      }
      if (j < n && s[j] == c)
        j++;
      span(o, "tok-str", s + i, j - i);
      i = j;
      line_start = 0;
      continue;
    }

    /* numbers */
    if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
      number_tok(o, s, n, &i);
      line_start = 0;
      continue;
    }

    /* identifiers */
    if (ident_start(c)) {
      size_t j = i;
      while (j < n && ident_char((unsigned char)s[j]))
        j++;
      size_t len = j - i;
      if (is_kw(ld, s + i, len)) {
        span(o, "tok-kw", s + i, len);
      } else {
        /* function call? identifier followed by optional space then '(' */
        size_t k = j;
        while (k < n && (s[k] == ' ' || s[k] == '\t'))
          k++;
        int call = (k < n && s[k] == '(');
        int allcaps = 1;
        for (size_t t = i; t < j; t++)
          if (!(isupper((unsigned char)s[t]) || s[t] == '_' || isdigit((unsigned char)s[t]))) {
            allcaps = 0;
            break;
          }
        if (call)
          span(o, "tok-fn", s + i, len);
        else if (allcaps && len > 1)
          span(o, "tok-const", s + i, len);
        else
          esc(o, s + i, len);
      }
      i = j;
      line_start = 0;
      continue;
    }

    /* anything else: punctuation / operators */
    esc(o, s + i, 1);
    i++;
    line_start = 0;
  }
}

/* A very small HTML/XML tokeniser: tags, attribute names and strings. */
static void scan_markup(struct strbuf *o, const char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    if (s[i] == '<') {
      /* comment? */
      if (i + 3 < n && strncmp(s + i, "<!--", 4) == 0) {
        size_t j = i + 4;
        while (j + 2 < n && strncmp(s + j, "-->", 3) != 0)
          j++;
        j = (j + 2 < n) ? j + 3 : n;
        span(o, "tok-com", s + i, j - i);
        i = j;
        continue;
      }
      size_t j = i;
      size_t tag_start = (i + 1 < n && s[i + 1] == '/') ? i + 2 : i + 1;
      sb_putc(o, '<');
      if (tag_start > i + 1)
        sb_putc(o, '/');
      size_t k = tag_start;
      while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '-' || s[k] == ':'))
        k++;
      span(o, "tok-kw", s + tag_start, k - tag_start);
      j = k;
      while (j < n && s[j] != '>') {
        if (s[j] == '"' || s[j] == '\'') {
          char q = s[j];
          size_t e = j + 1;
          while (e < n && s[e] != q)
            e++;
          if (e < n)
            e++;
          span(o, "tok-str", s + j, e - j);
          j = e;
        } else if (isalpha((unsigned char)s[j]) || s[j] == '-' || s[j] == '_') {
          size_t e = j;
          while (e < n && (isalnum((unsigned char)s[e]) || s[e] == '-' || s[e] == '_' || s[e] == ':'))
            e++;
          span(o, "tok-fn", s + j, e - j);
          j = e;
        } else {
          esc(o, s + j, 1);
          j++;
        }
      }
      if (j < n && s[j] == '>') {
        sb_putc(o, '>');
        j++;
      }
      i = j;
      continue;
    }
    esc(o, s + i, 1);
    i++;
  }
}

void highlight_to_html(struct strbuf *out, const char *text, size_t len,
                       const char *path) {
  const char *name;
  const struct langdef *ld = pick(path, &name);
  (void)name;
  if (ld->markup)
    scan_markup(out, text, len);
  else
    scan_generic(out, text, len, ld);
}

/* Map a fence language name (e.g. "python") to a known extension, or NULL. */
static const char *ext_for_lang(const char *lang) {
  static const struct {
    const char *lang;
    const char *ext;
  } M[] = {
      {"c", "c"},         {"cpp", "cpp"},   {"c++", "cpp"},
      {"cxx", "cpp"},     {"js", "js"},     {"javascript", "js"},
      {"jsx", "js"},      {"ts", "ts"},     {"typescript", "ts"},
      {"tsx", "ts"},      {"python", "py"}, {"py", "py"},
      {"rust", "rs"},     {"rs", "rs"},     {"go", "go"},
      {"sh", "sh"},       {"bash", "sh"},   {"shell", "sh"},
      {"zsh", "sh"},      {"json", "json"}, {"yaml", "yml"},
      {"yml", "yml"},     {"toml", "toml"}, {"make", "mk"},
      {"makefile", "mk"}, {"css", "css"},   {"scss", "css"},
      {"html", "html"},   {"xml", "html"},  {"svg", "html"},
      {"sql", "sql"},     {"nix", "nix"},
  };
  for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
    if (strcasecmp(lang, M[i].lang) == 0)
      return M[i].ext;
  return NULL;
}

void highlight_lang_to_html(struct strbuf *out, const char *text, size_t len,
                            const char *lang) {
  const char *ext = ext_for_lang(lang);
  if (!ext) {
    esc(out, text, len);
    return;
  }
  char fake[64];
  snprintf(fake, sizeof fake, "code.%s", ext);
  highlight_to_html(out, text, len, fake);
}
