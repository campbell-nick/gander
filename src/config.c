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
 *   watch       commit|all|off   what live reload reacts to (commit)
 *   ci-status   true/false   opt in to the network gh call (false)
 *   web-root    directory    where the UI assets live
 *   token       string       require this token to open the UI
 *   style       path         a stylesheet to layer over the bundled one
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
  cfg->watch_scope = WATCH_COMMIT;
  cfg->ci_status = 0;
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
  } else if (strcmp(key, "ci-status") == 0) {
    if (parse_bool(val, &b))
      cfg->ci_status = b;
    else
      fprintf(stderr, "gander: %s:%d: ci-status expects true/false\n", path,
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

  char line[8192];
  int lineno = 0;
  while (fgets(line, sizeof line, f)) {
    lineno++;
    char *s = trim(line);
    if (!*s || *s == '#' || *s == ';')
      continue;
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
    apply_kv(cfg, path, lineno, key, val);
  }
  fclose(f);
}
