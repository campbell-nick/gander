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

static void read_readme_desc(const char *root, char *out, size_t outsz) {
  const char *names[] = {"README.md", "README.markdown", "README", "readme.md"};
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
    if (!started && (*p == '#' || *p == '=' || *p == '>' || *p == '|' ||
                     strncmp(p, "```", 3) == 0))
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
  read_readme_desc(root, ri->description, sizeof ri->description);

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
