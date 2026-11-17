/* repo.c - derive GitHub-style repository metadata, entirely from disk.
 *
 * Nothing here touches the network. Cheap facts are read straight from the
 * filesystem (.git presence, LICENSE, CI workflow files, README); history
 * (branch, status, log, tags) comes from the local `git` CLI when it is
 * available. All git output is captured with sane caps.
 */
#include "gander.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>

/* ------------------------------------------------------------- shell bits - */

static void shell_quote(const char *s, char *out, size_t outsz) {
  size_t o = 0;
  if (o + 1 < outsz)
    out[o++] = '\'';
  for (; *s && o + 5 < outsz; s++) {
    if (*s == '\'') {
      memcpy(out + o, "'\\''", 4);
      o += 4;
    } else {
      out[o++] = *s;
    }
  }
  if (o + 1 < outsz)
    out[o++] = '\'';
  out[o] = '\0';
}

/* Run a command, capture stdout, strip trailing newlines. 1 on exit 0. */
static int run_capture(const char *cmd, char *out, size_t outsz) {
  out[0] = '\0';
  FILE *p = popen(cmd, "r");
  if (!p)
    return 0;
  size_t n = fread(out, 1, outsz - 1, p);
  out[n] = '\0';
  int status = pclose(p);
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' ||
                   out[n - 1] == '\t'))
    out[--n] = '\0';
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* --------------------------------------------------------------- helpers -- */

static int file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static void strip_md_inline(const char *in, char *out, size_t outsz) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < outsz; i++) {
    char c = in[i];
    if (c == '!' && in[i + 1] == '[') {
      /* image: skip to the alt text */
      i++;
      continue;
    }
    if (c == '[') {
      /* link: keep the text, drop the target */
      size_t j = i + 1;
      while (in[j] && in[j] != ']' && o + 1 < outsz)
        out[o++] = in[j++];
      while (in[j] && in[j] != ')')
        j++;
      i = j;
      continue;
    }
    if (c == '*' || c == '`' || c == '_' || c == '~')
      continue;
    out[o++] = c;
  }
  out[o] = '\0';
}

static void find_readme(const char *root, char *out, size_t outsz) {
  static const char *names[] = {"README.md",  "README.markdown", "README.rst",
                                "README.txt", "README",          "readme.md",
                                "Readme.md"};
  out[0] = '\0';
  char path[4200];
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", root, names[i]);
    if (file_exists(path)) {
      snprintf(out, outsz, "%s", names[i]);
      return;
    }
  }
}

static void read_readme_desc(const char *root, const char *name, char *out,
                             size_t outsz) {
  if (!name[0])
    return;
  char path[4200];
  snprintf(path, sizeof path, "%s/%s", root, name);

  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char line[1024];
  struct strbuf para;
  sb_init(&para);
  int started = 0;
  while (fgets(line, sizeof line, f)) {
    /* strip trailing newline */
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = '\0';
    /* skip leading blanks and headings */
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '\0') {
      if (started)
        break;
      continue;
    }
    /* Skip anything before the prose: headings, quotes, tables, fences, raw
     * HTML (e.g. a centred logo) and image/badge lines. */
    if (!started && (*p == '#' || *p == '=' || *p == '>' || *p == '|' ||
                     *p == '<' || strncmp(p, "```", 3) == 0 ||
                     (*p == '!' && p[1] == '[') ||
                     (*p == '[' && p[1] == '!' && p[2] == '[')))
      continue;
    if (!started) {
      started = 1;
    } else {
      sb_putc(&para, ' ');
    }
    sb_puts(&para, p);
    if (para.len > 380)
      break;
  }
  fclose(f);
  if (para.buf)
    strip_md_inline(para.buf, out, outsz);
  sb_free(&para);
}

static void detect_license(const char *root, char *out, size_t outsz) {
  const char *names[] = {"LICENSE", "LICENSE.md", "LICENSE.txt", "COPYING",
                         "LICENSE-MIT", "license"};
  char path[4200] = "";
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", root, names[i]);
    if (file_exists(path))
      break;
    path[0] = '\0';
  }
  if (!path[0])
    return;
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char buf[4096];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = '\0';
  fclose(f);

  struct {
    const char *key;
    const char *name;
  } K[] = {{"MIT License", "MIT"},       {"Apache License", "Apache-2.0"},
           {"GNU GENERAL PUBLIC LICENSE", "GPL"}, {"GNU LESSER", "LGPL"},
           {"BSD 3-Clause", "BSD-3-Clause"}, {"BSD 2-Clause", "BSD-2-Clause"},
           {"ISC License", "ISC"},       {"Mozilla Public License", "MPL-2.0"},
           {"The Unlicense", "Unlicense"}};
  for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
    if (strstr(buf, K[i].key)) {
      snprintf(out, outsz, "%s", K[i].name);
      return;
    }
  if (strstr(buf, "MIT"))
    snprintf(out, outsz, "MIT");
}

static void detect_ci(const char *root, struct repoinfo *ri) {
  char dir[4200];
  snprintf(dir, sizeof dir, "%s/.github/workflows", root);
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d)) && ri->ci_count < 8) {
    const char *dot = strrchr(e->d_name, '.');
    if (!dot || (strcasecmp(dot, ".yml") != 0 && strcasecmp(dot, ".yaml") != 0))
      continue;
    /* trim the extension for a tidy name */
    size_t n = (size_t)(dot - e->d_name);
    if (n >= sizeof ri->ci_names[0])
      n = sizeof ri->ci_names[0] - 1;
    memcpy(ri->ci_names[ri->ci_count], e->d_name, n);
    ri->ci_names[ri->ci_count][n] = '\0';
    ri->ci_count++;
  }
  closedir(d);
}

/* ------------------------------------------------------------- languages -- */

static const struct {
  const char *ext;
  const char *name;
  const char *color;
} LANGS[] = {
    {"c", "C", "#555555"},          {"h", "C", "#555555"},
    {"cc", "C++", "#f34b7d"},       {"cpp", "C++", "#f34b7d"},
    {"cxx", "C++", "#f34b7d"},      {"hpp", "C++", "#f34b7d"},
    {"rs", "Rust", "#dea584"},      {"go", "Go", "#00ADD8"},
    {"py", "Python", "#3572A5"},    {"js", "JavaScript", "#f1e05a"},
    {"mjs", "JavaScript", "#f1e05a"}, {"cjs", "JavaScript", "#f1e05a"},
    {"ts", "TypeScript", "#2b7489"}, {"tsx", "TypeScript", "#2b7489"},
    {"sh", "Shell", "#89e051"},     {"bash", "Shell", "#89e051"},
    {"zsh", "Shell", "#89e051"},    {"fish", "Shell", "#89e051"},
    {"md", "Markdown", "#083fa1"},  {"markdown", "Markdown", "#083fa1"},
    {"html", "HTML", "#e34c26"},    {"htm", "HTML", "#e34c26"},
    {"css", "CSS", "#563d7c"},      {"scss", "SCSS", "#c6538c"},
    {"nix", "Nix", "#7e7eff"},      {"json", "JSON", "#292929"},
    {"yml", "YAML", "#cb171e"},     {"yaml", "YAML", "#cb171e"},
    {"toml", "TOML", "#9c4221"},    {"lua", "Lua", "#000080"},
    {"rb", "Ruby", "#701516"},      {"java", "Java", "#b07219"},
    {"lisp", "Lisp", "#3fb68b"},    {"el", "Emacs Lisp", "#c065db"},
    {"vim", "Vim script", "#199f4b"}, {"sql", "SQL", "#e38c00"},
    {"tex", "TeX", "#3D6117"},      {"r", "R", "#198CE7"},
};

static const char *lang_of(const char *path, const char **color) {
  const char *dot = strrchr(path, '.');
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  if (!dot || dot < base) {
    if (strcmp(base, "Makefile") == 0 || strcmp(base, "makefile") == 0) {
      *color = "#427819";
      return "Makefile";
    }
    if (strcmp(base, "Dockerfile") == 0) {
      *color = "#384d54";
      return "Dockerfile";
    }
    if (strcmp(base, "CMakeLists.txt") == 0) {
      *color = "#DA3434";
      return "CMake";
    }
    return NULL;
  }
  const char *ext = dot + 1;
  for (size_t i = 0; i < sizeof LANGS / sizeof LANGS[0]; i++)
    if (strcasecmp(ext, LANGS[i].ext) == 0) {
      *color = LANGS[i].color;
      return LANGS[i].name;
    }
  return NULL;
}

static void add_lang(struct repoinfo *ri, const char *name, const char *color,
                     long bytes) {
  for (int i = 0; i < ri->nlangs; i++)
    if (strcmp(ri->langs[i].name, name) == 0) {
      ri->langs[i].bytes += bytes;
      return;
    }
  if (ri->nlangs >= 8)
    return;
  struct langent *e = &ri->langs[ri->nlangs++];
  snprintf(e->name, sizeof e->name, "%s", name);
  snprintf(e->color, sizeof e->color, "%s", color);
  e->bytes = bytes;
}

static void scan_languages(const char *root, struct repoinfo *ri) {
  struct filelist fl;
  filelist_init(&fl);
  walk_dir(root, "", &fl, 20000);

  ri->files = (long)fl.n;
  for (size_t i = 0; i < fl.n; i++) {
    char full[4400];
    snprintf(full, sizeof full, "%s/%s", root, fl.v[i]);
    struct stat st;
    long sz = (stat(full, &st) == 0) ? (long)st.st_size : 0;
    ri->total_bytes += sz;
    const char *color = NULL;
    const char *name = lang_of(fl.v[i], &color);
    if (!name)
      continue;
    add_lang(ri, name, color, sz);
  }
  filelist_free(&fl);

  /* sort by bytes, descending */
  for (int i = 0; i < ri->nlangs; i++)
    for (int j = i + 1; j < ri->nlangs; j++)
      if (ri->langs[j].bytes > ri->langs[i].bytes) {
        struct langent t = ri->langs[i];
        ri->langs[i] = ri->langs[j];
        ri->langs[j] = t;
      }
}

/* -------------------------------------------------------------- gather ---- */

void repo_info(const char *root, struct repoinfo *ri) {
  memset(ri, 0, sizeof *ri);

  char q[4400];
  shell_quote(root, q, sizeof q);

  char gitdir[4400];
  snprintf(gitdir, sizeof gitdir, "%s/.git", root);
  ri->is_git = file_exists(gitdir);

  scan_languages(root, ri);
  detect_license(root, ri->license, sizeof ri->license);
  detect_ci(root, ri);
  find_readme(root, ri->readme, sizeof ri->readme);
  read_readme_desc(root, ri->readme, ri->description, sizeof ri->description);

  if (!ri->is_git)
    return;

  char cmd[5000];
  char buf[65536];

  /* branch: symbolic-ref gives the branch; detached HEAD falls back to hash */
  snprintf(cmd, sizeof cmd, "git -C %s symbolic-ref --short HEAD 2>/dev/null", q);
  if (!run_capture(cmd, ri->branch, sizeof ri->branch) || !ri->branch[0]) {
    snprintf(cmd, sizeof cmd, "git -C %s rev-parse --short HEAD 2>/dev/null", q);
    if (run_capture(cmd, ri->branch, sizeof ri->branch))
      snprintf(ri->branch + strlen(ri->branch),
               sizeof ri->branch - strlen(ri->branch), " (detached)");
  }

  snprintf(cmd, sizeof cmd, "git -C %s config --get remote.origin.url 2>/dev/null", q);
  run_capture(cmd, ri->remote, sizeof ri->remote);

  /* status: count changed entries */
  snprintf(cmd, sizeof cmd, "git -C %s status --porcelain 2>/dev/null", q);
  if (run_capture(cmd, buf, sizeof buf)) {
    for (char *p = buf; *p;) {
      if (*p == '\n')
        ri->changes++;
      p++;
    }
    if (buf[0])
      ri->changes++;
    ri->dirty = ri->changes > 0;
  }

  /* last commit: hash, subject, author, relative date */
  snprintf(cmd, sizeof cmd,
           "git -C %s log -1 --format=%%h%%x1f%%s%%x1f%%an%%x1f%%ar 2>/dev/null", q);
  if (run_capture(cmd, buf, sizeof buf)) {
    char *us = buf;
    char *p1 = strchr(us, '\x1f');
    if (p1) {
      *p1 = '\0';
      snprintf(ri->last_hash, sizeof ri->last_hash, "%.15s", us);
      char *p2 = strchr(p1 + 1, '\x1f');
      if (p2) {
        *p2 = '\0';
        snprintf(ri->last_subject, sizeof ri->last_subject, "%.299s", p1 + 1);
        char *p3 = strchr(p2 + 1, '\x1f');
        if (p3) {
          *p3 = '\0';
          snprintf(ri->last_author, sizeof ri->last_author, "%.127s", p2 + 1);
          snprintf(ri->last_rel, sizeof ri->last_rel, "%.63s", p3 + 1);
        }
      }
    }
  }

  snprintf(cmd, sizeof cmd, "git -C %s rev-list --count HEAD 2>/dev/null", q);
  char cnt[64];
  if (run_capture(cmd, cnt, sizeof cnt))
    ri->commits = atol(cnt);

  /* tags: latest by version sort, plus a count */
  snprintf(cmd, sizeof cmd, "git -C %s tag --sort=-v:refname 2>/dev/null", q);
  if (run_capture(cmd, buf, sizeof buf) && buf[0]) {
    for (char *p = buf; *p; p++)
      if (*p == '\n')
        ri->tag_count++;
    ri->tag_count++; /* lines = newlines + 1 */
    char *nl = strchr(buf, '\n');
    if (nl)
      *nl = '\0';
    snprintf(ri->latest_tag, sizeof ri->latest_tag, "%.63s", buf);
  }
}

/* --------------------------------------------------------------- JSON ----- */

void repo_info_json(struct strbuf *out, const struct repoinfo *ri) {
  sb_puts(out, "{\"is_git\":");
  sb_puts(out, ri->is_git ? "true" : "false");

  sb_puts(out, ",\"branch\":");
  sb_put_json_string(out, ri->branch);
  sb_puts(out, ",\"remote\":");
  sb_put_json_string(out, ri->remote);
  sb_puts(out, ",\"description\":");
  sb_put_json_string(out, ri->description);
  sb_puts(out, ",\"readme\":");
  sb_put_json_string(out, ri->readme);
  sb_puts(out, ",\"license\":");
  sb_put_json_string(out, ri->license);

  sb_printf(out, ",\"dirty\":%s", ri->dirty ? "true" : "false");
  sb_printf(out, ",\"changes\":%d", ri->changes);
  sb_printf(out, ",\"commits\":%ld", ri->commits);
  sb_printf(out, ",\"files\":%ld", ri->files);
  sb_printf(out, ",\"total_bytes\":%ld", ri->total_bytes);

  sb_puts(out, ",\"last\":{\"hash\":");
  sb_put_json_string(out, ri->last_hash);
  sb_puts(out, ",\"subject\":");
  sb_put_json_string(out, ri->last_subject);
  sb_puts(out, ",\"author\":");
  sb_put_json_string(out, ri->last_author);
  sb_puts(out, ",\"when\":");
  sb_put_json_string(out, ri->last_rel);
  sb_puts(out, "}");

  sb_puts(out, ",\"latest_tag\":");
  sb_put_json_string(out, ri->latest_tag);
  sb_printf(out, ",\"tag_count\":%d", ri->tag_count);

  sb_puts(out, ",\"ci\":[");
  for (int i = 0; i < ri->ci_count; i++) {
    if (i)
      sb_putc(out, ',');
    sb_put_json_string(out, ri->ci_names[i]);
  }
  sb_puts(out, "]");

  sb_puts(out, ",\"languages\":[");
  for (int i = 0; i < ri->nlangs; i++) {
    if (i)
      sb_putc(out, ',');
    sb_puts(out, "{\"name\":");
    sb_put_json_string(out, ri->langs[i].name);
    sb_puts(out, ",\"color\":");
    sb_put_json_string(out, ri->langs[i].color);
    sb_printf(out, ",\"bytes\":%ld}", ri->langs[i].bytes);
  }
  sb_puts(out, "]}");
}

/* ---------------------------------------------------------------- status -- */

/* Capture a command's stdout into a growable buffer (trailing newlines
 * trimmed). Returns 1 on exit status 0. */
static int run_capture_buf(const char *cmd, struct strbuf *out) {
  FILE *p = popen(cmd, "r");
  if (!p)
    return 0;
  char buf[8192];
  size_t got;
  while ((got = fread(buf, 1, sizeof buf, p)) > 0)
    sb_putn(out, buf, got);
  int status = pclose(p);
  while (out->len > 0 && (out->buf[out->len - 1] == '\n' ||
                          out->buf[out->len - 1] == '\r'))
    out->buf[--out->len] = '\0';
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void normalize_status(const char *xy, char *out, size_t outsz) {
  char x = xy[0], y = xy[1];
  const char *s = "modified";
  if (x == '?' && y == '?')
    s = "untracked";
  else if (x == '!' && y == '!')
    s = "ignored";
  else if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') ||
           (x == 'D' && y == 'D'))
    s = "conflicted";
  else if (x == 'A' || y == 'A')
    s = "added";
  else if (x == 'D' || y == 'D')
    s = "deleted";
  else if (x == 'R' || y == 'R')
    s = "renamed";
  else if (x == 'C' || y == 'C')
    s = "copied";
  else if (x == 'T' || y == 'T')
    s = "typechange";
  snprintf(out, outsz, "%s", s);
}

void statuslist_free(struct statuslist *sl) {
  free(sl->v);
  sl->v = NULL;
  sl->n = sl->cap = 0;
}

static struct statusent *statuslist_add(struct statuslist *sl) {
  if (sl->n == sl->cap) {
    sl->cap = sl->cap ? sl->cap * 2 : 32;
    sl->v = xrealloc(sl->v, sl->cap * sizeof(*sl->v));
  }
  struct statusent *e = &sl->v[sl->n++];
  memset(e, 0, sizeof *e);
  return e;
}

void repo_status(const char *root, struct statuslist *sl) {
  memset(sl, 0, sizeof *sl);

  char gitdir[4400];
  snprintf(gitdir, sizeof gitdir, "%s/.git", root);
  if (!file_exists(gitdir))
    return;

  char q[4400];
  shell_quote(root, q, sizeof q);
  char cmd[5000];
  snprintf(cmd, sizeof cmd,
           "git -C %s status --porcelain=v1 -z --untracked-files=normal "
           "2>/dev/null",
           q);

  struct strbuf out;
  sb_init(&out);
  if (!run_capture_buf(cmd, &out)) {
    sb_free(&out);
    return;
  }

  /* Records are NUL-separated "XY PATH"; a rename/copy is followed by a
   * second NUL-terminated record holding the original path, which we skip. */
  size_t i = 0;
  while (i < out.len) {
    size_t end = i;
    while (end < out.len && out.buf[end] != '\0')
      end++;
    size_t rlen = end - i;
    if (rlen >= 3) {
      const char *rec = out.buf + i;
      struct statusent *e = statuslist_add(sl);
      snprintf(e->code, sizeof e->code, "%c%c", rec[0], rec[1]);
      normalize_status(e->code, e->status, sizeof e->status);
      size_t plen = rlen - 3;
      if (plen >= sizeof e->path)
        plen = sizeof e->path - 1;
      memcpy(e->path, rec + 3, plen);
      e->path[plen] = '\0';
      if ((rec[0] == 'R' || rec[1] == 'R' || rec[0] == 'C' || rec[1] == 'C') &&
          end + 1 < out.len) {
        size_t o = end + 1;
        while (o < out.len && out.buf[o] != '\0')
          o++;
        i = o + 1;
        continue;
      }
    }
    i = end + 1;
  }
  sb_free(&out);
}

void repo_status_json(struct strbuf *out, const struct statuslist *sl) {
  sb_puts(out, "[");
  for (size_t i = 0; i < sl->n; i++) {
    if (i)
      sb_putc(out, ',');
    sb_puts(out, "{\"path\":");
    sb_put_json_string(out, sl->v[i].path);
    sb_puts(out, ",\"code\":");
    sb_put_json_string(out, sl->v[i].code);
    sb_puts(out, ",\"status\":");
    sb_put_json_string(out, sl->v[i].status);
    sb_puts(out, "}");
  }
  sb_puts(out, "]");
}

/* ------------------------------------------------------------------ log --- */

void repo_log_json(const char *root, struct strbuf *out, int max) {
  sb_puts(out, "[");

  char gitdir[4400];
  snprintf(gitdir, sizeof gitdir, "%s/.git", root);
  if (!file_exists(gitdir)) {
    sb_puts(out, "]");
    return;
  }
  if (max < 1)
    max = 50;
  if (max > 200)
    max = 200;

  char q[4400];
  shell_quote(root, q, sizeof q);
  char cmd[5200];
  snprintf(cmd, sizeof cmd,
           "git -C %s log -n %d "
           "--format=%%h%%x1f%%an%%x1f%%ar%%x1f%%s%%x1e 2>/dev/null",
           q, max);

  struct strbuf raw;
  sb_init(&raw);
  if (!run_capture_buf(cmd, &raw)) {
    sb_free(&raw);
    sb_puts(out, "]");
    return;
  }

  size_t i = 0;
  int first = 1;
  while (i < raw.len) {
    while (i < raw.len && (raw.buf[i] == '\n' || raw.buf[i] == '\r'))
      i++;
    if (i >= raw.len)
      break;
    size_t end = i;
    while (end < raw.len && raw.buf[end] != '\x1e')
      end++;
    if (end > i) {
      char rec[1400];
      size_t rl = end - i;
      if (rl >= sizeof rec)
        rl = sizeof rec - 1;
      memcpy(rec, raw.buf + i, rl);
      rec[rl] = '\0';
      char *f[4] = {rec, NULL, NULL, NULL};
      int nf = 1;
      for (char *p = rec; *p && nf < 4; p++)
        if (*p == '\x1f') {
          *p = '\0';
          f[nf++] = p + 1;
        }
      if (nf == 4) {
        if (!first)
          sb_putc(out, ',');
        first = 0;
        sb_puts(out, "{\"hash\":");
        sb_put_json_string(out, f[0]);
        sb_puts(out, ",\"author\":");
        sb_put_json_string(out, f[1]);
        sb_puts(out, ",\"when\":");
        sb_put_json_string(out, f[2]);
        sb_puts(out, ",\"subject\":");
        sb_put_json_string(out, f[3]);
        sb_puts(out, "}");
      }
    }
    i = end + 1;
  }
  sb_free(&raw);
  sb_puts(out, "]");
}

/* ----------------------------------------------------------------- diff --- */

static void escape_range(struct strbuf *out, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch == '&')
      sb_puts(out, "&amp;");
    else if (ch == '<')
      sb_puts(out, "&lt;");
    else if (ch == '>')
      sb_puts(out, "&gt;");
    else
      sb_putc(out, (char)ch);
  }
}

static void diff_row(struct strbuf *out, const char *kind, long oldn, long newn,
                     const char *text, size_t len) {
  sb_printf(out, "<tr class=\"%s\"><td class=\"ln\">", kind);
  if (oldn > 0)
    sb_printf(out, "%ld", oldn);
  sb_puts(out, "</td><td class=\"ln\">");
  if (newn > 0)
    sb_printf(out, "%ld", newn);
  sb_puts(out, "</td><td class=\"code\">");
  escape_range(out, text, len);
  sb_puts(out, "</td></tr>\n");
}

/* Render a plain blob (an untracked file) as a series of added lines. */
static void diff_added_blob(struct strbuf *out, const char *text, size_t len) {
  long ln = 0;
  size_t i = 0;
  while (i < len) {
    size_t end = i;
    while (end < len && text[end] != '\n')
      end++;
    ln++;
    /* reconstruct "+line" */
    struct strbuf line;
    sb_init(&line);
    sb_putc(&line, '+');
    sb_putn(&line, text + i, end - i);
    diff_row(out, "add", 0, ln, line.buf, line.len);
    sb_free(&line);
    i = end + 1;
  }
  if (len == 0)
    (void)ln;
}

/* Render a unified diff, tracking old/new line numbers from the @@ headers. */
static void diff_unified(struct strbuf *out, const char *text, size_t len) {
  long oldn = 0, newn = 0;
  int in_hunk = 0;
  size_t i = 0;
  while (i < len) {
    size_t end = i;
    while (end < len && text[end] != '\n')
      end++;
    const char *line = text + i;
    size_t llen = end - i;

    if (llen >= 11 && strncmp(line, "diff --git ", 11) == 0) {
      in_hunk = 0;
      diff_row(out, "meta", 0, 0, line, llen);
    } else if (llen >= 2 && line[0] == '@' && line[1] == '@') {
      const char *p = line + 2;
      while (*p == ' ')
        p++;
      if (*p == '-') {
        oldn = atol(p + 1);
        while (*p && *p != ' ')
          p++;
      }
      while (*p == ' ')
        p++;
      if (*p == '+')
        newn = atol(p + 1);
      in_hunk = 1;
      diff_row(out, "hunk", 0, 0, line, llen);
    } else if (!in_hunk) {
      diff_row(out, "meta", 0, 0, line, llen);
    } else if (llen && line[0] == '+') {
      diff_row(out, "add", 0, newn++, line, llen);
    } else if (llen && line[0] == '-') {
      diff_row(out, "del", oldn++, 0, line, llen);
    } else if (llen && line[0] == '\\') {
      diff_row(out, "meta", 0, 0, line, llen);
    } else if (llen && line[0] == ' ') {
      diff_row(out, "ctx", oldn++, newn++, line, llen);
    } else {
      diff_row(out, "meta", 0, 0, line, llen);
    }

    if (end >= len)
      break;
    i = end + 1;
  }
}

int repo_diff_html(const char *root, const char *rel, struct strbuf *out) {
  char gitdir[4400];
  snprintf(gitdir, sizeof gitdir, "%s/.git", root);
  if (!file_exists(gitdir))
    return 0;

  char q[4400], qrel[4400];
  shell_quote(root, q, sizeof q);
  shell_quote(rel, qrel, sizeof qrel);

  /* Is this path untracked? (scoped to the one path, so --all stays cheap) */
  int untracked = 0;
  {
    char cmd[12000];
    snprintf(cmd, sizeof cmd,
             "git -C %s status --porcelain=v1 -z --untracked-files=all -- %s "
             "2>/dev/null",
             q, qrel);
    struct strbuf st;
    sb_init(&st);
    if (run_capture_buf(cmd, &st) && st.len >= 2 && st.buf[0] == '?' &&
        st.buf[1] == '?')
      untracked = 1;
    sb_free(&st);
  }

  struct strbuf diff;
  sb_init(&diff);
  int have = 0;

  if (untracked) {
    char full[4400];
    snprintf(full, sizeof full, "%s/%s", root, rel);
    FILE *f = fopen(full, "rb");
    if (f) {
      const size_t MAX = 512 * 1024;
      char buf[8192];
      size_t got;
      while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (diff.len + got > MAX) {
          sb_putn(&diff, buf, MAX - diff.len);
          break;
        }
        sb_putn(&diff, buf, got);
      }
      fclose(f);
      have = 1;
    }
  } else {
    char cmd[12000];
    snprintf(cmd, sizeof cmd,
             "git -C %s diff --no-color --no-ext-diff -U3 HEAD -- %s 2>/dev/null",
             q, qrel);
    if (run_capture_buf(cmd, &diff) && diff.len)
      have = 1;
    if (!have) { /* no HEAD yet, or an unborn branch */
      snprintf(cmd, sizeof cmd,
               "git -C %s diff --no-color --no-ext-diff -U3 --cached -- %s "
               "2>/dev/null",
               q, qrel);
      if (run_capture_buf(cmd, &diff) && diff.len)
        have = 1;
    }
  }

  if (!have) {
    sb_free(&diff);
    return 0;
  }

  /* Detect a binary diff notice so the UI can style it plainly. */
  int binary = 0;
  for (size_t i = 0; i + 7 <= diff.len; i++)
    if (strncmp(diff.buf + i, "Binary ", 7) == 0) {
      binary = 1;
      break;
    }

  sb_puts(out, "<div class=\"diff\"><table class=\"diff-table\">");
  if (binary) {
    diff_row(out, "meta", 0, 0, "Binary file - no textual diff.", 31);
  } else if (untracked) {
    diff_added_blob(out, diff.buf ? diff.buf : "", diff.len);
  } else {
    diff_unified(out, diff.buf ? diff.buf : "", diff.len);
  }
  sb_puts(out, "</table></div>");

  sb_free(&diff);
  return 1;
}
