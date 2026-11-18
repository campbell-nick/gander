/* config.c - the optional gander configuration file.
 *
 * Settings are read from the first of these that exists:
 *
 *   $GANDER_CONFIG                    an explicit file (or --config)
 *   $XDG_CONFIG_HOME/gander/config
 *   $HOME/.config/gander/config
 *
 * A stylesheet next to the config file (style.css) is layered over the
 * bundled theme, so a small override file is enough to re-skin gander.
 *
 * The file is plain `key = value` lines; blank lines and lines starting with
 * '#' or ';' are ignored, and a value may be quoted. Recognised keys:
 *
 *   port        integer      0 picks a free port          (default 0)
 *   host        address      interface to bind            (127.0.0.1)
 *   open        true/false   launch a browser on start    (true)
 *   app         true/false   open a chromeless app window (false)
 *   webview     true/false   use the embedded webview     (false)
 *   watch       commit|all|off   what live reload reacts to (commit)
 *   web-root    directory    where the UI assets live
 *   token       string       require this token to open the UI
 *   style       path         a stylesheet to layer over the bundled one
 *
 * A [paths] section names repositories, so `gander Nexus` opens the one you
 * configured (matched case-insensitively, with a leading ~ expanded to $HOME):
 *
 *   [paths]
 *   Nexus = ~/Projects/Nexus
 *
 * Command-line options override anything set here. */
#include "gander.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s))
    s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1]))
    *--e = '\0';
  return s;
}

static int parse_bool(const char *v, int *out) {
  if (strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 ||
      strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0) {
    *out = 1;
    return 1;
  }
  if (strcasecmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
      strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0) {
    *out = 0;
    return 1;
  }
  return 0;
}

static int is_regular(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

void config_init(struct gander_config *cfg) {
  memset(cfg, 0, sizeof *cfg);
  cfg->port = 0;
  cfg->do_open = 1;
  cfg->do_app = 0;
  cfg->do_webview = 0;
  cfg->watch_scope = WATCH_COMMIT;
  snprintf(cfg->host, sizeof cfg->host, "127.0.0.1");
}

static void apply_kv(struct gander_config *cfg, const char *path, int lineno,
                     const char *key, const char *val) {
  int b;
  if (strcmp(key, "port") == 0) {
    cfg->port = atoi(val);
  } else if (strcmp(key, "host") == 0) {
    snprintf(cfg->host, sizeof cfg->host, "%s", val);
  } else if (strcmp(key, "open") == 0) {
    if (parse_bool(val, &b))
      cfg->do_open = b;
    else
      fprintf(stderr, "gander: %s:%d: open expects true/false\n", path, lineno);
  } else if (strcmp(key, "app") == 0) {
    if (parse_bool(val, &b))
      cfg->do_app = b;
    else
      fprintf(stderr, "gander: %s:%d: app expects true/false\n", path, lineno);
  } else if (strcmp(key, "webview") == 0) {
    if (parse_bool(val, &b))
      cfg->do_webview = b;
    else
      fprintf(stderr, "gander: %s:%d: webview expects true/false\n", path,
              lineno);
  } else if (strcmp(key, "watch") == 0) {
    if (strcmp(val, "off") == 0 || strcmp(val, "none") == 0)
      cfg->watch_scope = WATCH_OFF;
    else if (strcmp(val, "all") == 0 || strcmp(val, "files") == 0)
      cfg->watch_scope = WATCH_ALL;
    else if (strcmp(val, "commit") == 0 || strcmp(val, "git") == 0)
      cfg->watch_scope = WATCH_COMMIT;
    else
      fprintf(stderr, "gander: %s:%d: watch expects commit/all/off\n", path,
              lineno);
  } else if (strcmp(key, "web-root") == 0) {
    snprintf(cfg->web_root, sizeof cfg->web_root, "%s", val);
  } else if (strcmp(key, "token") == 0) {
    snprintf(cfg->token, sizeof cfg->token, "%s", val);
  } else if (strcmp(key, "style") == 0) {
    snprintf(cfg->css, sizeof cfg->css, "%s", val);
  } else {
    fprintf(stderr, "gander: %s:%d: unknown key '%s' (ignored)\n", path,
            lineno, key);
  }
}

/* Expand a leading ~ or ~/ to $HOME; anything else is copied verbatim. */
static void expand_tilde(const char *in, char *out, size_t outsz) {
  if (in[0] == '~' && (in[1] == '\0' || in[1] == '/')) {
    const char *home = getenv("HOME");
    if (home && home[0]) {
      snprintf(out, outsz, "%s%s", home, in + 1);
      return;
    }
  }
  snprintf(out, outsz, "%s", in);
}

/* Record one `name = path` line from the [paths] section. A later definition
 * of the same name wins, so the last one in the file is the one used. */
static void config_add_path(struct gander_config *cfg, const char *name,
                            const char *val, const char *file, int lineno) {
  if (!name[0]) {
    fprintf(stderr, "gander: %s:%d: empty path name (ignored)\n", file, lineno);
    return;
  }
  for (int i = 0; i < cfg->npaths; i++) {
    if (strcasecmp(cfg->paths[i].name, name) == 0) {
      expand_tilde(val, cfg->paths[i].path, sizeof cfg->paths[i].path);
      return;
    }
  }
  if (cfg->npaths >= GANDER_MAX_PATHS) {
    fprintf(stderr, "gander: %s:%d: too many paths (max %d)\n", file, lineno,
            GANDER_MAX_PATHS);
    return;
  }
  struct gander_path *p = &cfg->paths[cfg->npaths++];
  snprintf(p->name, sizeof p->name, "%s", name);
  expand_tilde(val, p->path, sizeof p->path);
}

const char *config_lookup_path(const struct gander_config *cfg,
                               const char *name) {
  for (int i = 0; i < cfg->npaths; i++)
    if (strcasecmp(cfg->paths[i].name, name) == 0)
      return cfg->paths[i].path;
  return NULL;
}

void config_load(struct gander_config *cfg, const char *explicit_path) {
  char path[4096] = "";
  if (explicit_path && explicit_path[0]) {
    snprintf(path, sizeof path, "%s", explicit_path);
  } else {
    const char *env = getenv("GANDER_CONFIG");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (env && env[0])
      snprintf(path, sizeof path, "%s", env);
    else if (xdg && xdg[0])
      snprintf(path, sizeof path, "%s/gander/config", xdg);
    else if (home && home[0])
      snprintf(path, sizeof path, "%s/.config/gander/config", home);
  }
  if (!path[0])
    return;

  /* A style.css beside the config file is layered over the bundled theme. */
  char dir[4096];
  snprintf(dir, sizeof dir, "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash)
    *slash = '\0';
  else
    snprintf(dir, sizeof dir, ".");
  int n = snprintf(cfg->css, sizeof cfg->css, "%s/style.css", dir);
  if (n < 0 || (size_t)n >= sizeof cfg->css || !is_regular(cfg->css))
    cfg->css[0] = '\0';

  FILE *f = fopen(path, "r");
  if (!f)
    return; /* a missing config file is not an error */
  snprintf(cfg->path, sizeof cfg->path, "%s", path);

  char section[64] = "";
  char line[8192];
  int lineno = 0;
  while (fgets(line, sizeof line, f)) {
    lineno++;
    char *s = trim(line);
    if (!*s || *s == '#' || *s == ';')
      continue;

    /* A [section] header switches the context for the keys that follow. */
    if (*s == '[') {
      char *close = strchr(s, ']');
      if (!close) {
        fprintf(stderr, "gander: %s:%d: malformed section header\n", path,
                lineno);
        continue;
      }
      *close = '\0';
      snprintf(section, sizeof section, "%s", trim(s + 1));
      continue;
    }

    char *eq = strchr(s, '=');
    if (!eq) {
      fprintf(stderr, "gander: %s:%d: expected key = value\n", path, lineno);
      continue;
    }
    *eq = '\0';
    char *key = trim(s);
    char *val = trim(eq + 1);
    /* Drop a trailing comment that follows whitespace. */
    for (char *p = val; *p; p++) {
      if ((*p == '#' || *p == ';') && (p == val || isspace((unsigned char)p[-1]))) {
        *p = '\0';
        val = trim(val);
        break;
      }
    }
    size_t vl = strlen(val);
    if (vl >= 2 && ((val[0] == '"' && val[vl - 1] == '"') ||
                    (val[0] == '\'' && val[vl - 1] == '\''))) {
      val[vl - 1] = '\0';
      val++;
    }
    if (strcasecmp(section, "paths") == 0)
      config_add_path(cfg, key, val, path, lineno);
    else if (section[0])
      fprintf(stderr, "gander: %s:%d: unknown section '[%s]' (ignored)\n",
              path, lineno, section);
    else
      apply_kv(cfg, path, lineno, key, val);
  }
  fclose(f);
}
