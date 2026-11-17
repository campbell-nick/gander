/* main.c - command line handling for gander. */
#include "gander.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void usage(FILE *out) {
  fprintf(out,
          "gander %s - preview a local folder or git repository, offline.\n"
          "\n"
          "Usage: gander [path] [options]\n"
          "\n"
          "Options:\n"
          "  --port <n>        port to listen on (default: pick a free one)\n"
          "  --host <addr>     address to bind (default: 127.0.0.1)\n"
          "  --web-root <dir>  directory holding the bundled UI assets\n"
          "  --token <str>     require this token to open the UI\n"
          "  --config <file>   read settings from this file instead of\n"
          "                    ~/.config/gander/config\n"
          "  --no-open         do not launch a browser\n"
          "  --watch <mode>    what live reload reacts to: commit (default),\n"
          "                    all (every file change) or off\n"
          "  --no-watch        disable live reload (same as --watch off)\n"
          "  -h, --help        show this help and exit\n"
          "  -V, --version     show the version and exit\n"
          "\n"
          "By default the UI refreshes only when you commit (or switch branch,\n"
          "tag or fetch); use --watch all to also refresh as you edit files.\n"
          "\n"
          "Settings can also live in ~/.config/gander/config (override with\n"
          "--config or $GANDER_CONFIG); a style.css beside it is layered over\n"
          "the bundled theme. Command-line options win.\n",
          GANDER_VERSION);
}

static int parse_watch(const char *v) {
  if (strcmp(v, "off") == 0 || strcmp(v, "none") == 0)
    return WATCH_OFF;
  if (strcmp(v, "all") == 0 || strcmp(v, "files") == 0)
    return WATCH_ALL;
  if (strcmp(v, "commit") == 0 || strcmp(v, "git") == 0)
    return WATCH_COMMIT;
  fprintf(stderr, "gander: unknown watch mode: %s (using commit)\n", v);
  return WATCH_COMMIT;
}

static int is_dir(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* Directory containing the running executable, or "" if unknown. */
static void exe_dir(char *out, size_t outsz) {
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) {
    out[0] = '\0';
    return;
  }
  buf[n] = '\0';
  char *slash = strrchr(buf, '/');
  if (slash)
    *slash = '\0';
  snprintf(out, outsz, "%s", buf);
}

static void find_web_root(const char *override, char *out, size_t outsz) {
  if (override && override[0]) {
    snprintf(out, outsz, "%s", override);
    return;
  }
  const char *env = getenv("GANDER_WEB");
  if (env && env[0]) {
    snprintf(out, outsz, "%s", env);
    return;
  }

  char ed[PATH_MAX];
  exe_dir(ed, sizeof ed);
  if (ed[0]) {
    char cand[PATH_MAX + 32];
    snprintf(cand, sizeof cand, "%s/web", ed);
    if (is_dir(cand)) {
      snprintf(out, outsz, "%s", cand);
      return;
    }
    /* installed layout: <prefix>/bin/gander + <prefix>/share/gander/web */
    snprintf(cand, sizeof cand, "%s/../share/gander/web", ed);
    if (is_dir(cand)) {
      snprintf(out, outsz, "%s", cand);
      return;
    }
  }
  if (is_dir("./web")) {
    snprintf(out, outsz, "./web");
    return;
  }
  snprintf(out, outsz, "./web");
}

int main(int argc, char **argv) {
  const char *path = ".";
  const char *config_path = NULL;

  /* First pass: locate --config so the file is read before the other options,
   * which then override whatever it set. */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
      config_path = argv[i + 1];
    else if (strncmp(argv[i], "--config=", 9) == 0)
      config_path = argv[i] + 9;
  }

  struct gander_config cfg;
  config_init(&cfg);
  config_load(&cfg, config_path);

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(stdout);
      return 0;
    } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
      printf("gander %s\n", GANDER_VERSION);
      return 0;
    } else if (strcmp(a, "--config") == 0 && i + 1 < argc) {
      i++; /* already applied above */
    } else if (strncmp(a, "--config=", 9) == 0) {
      /* already applied above */
    } else if (strcmp(a, "--port") == 0 && i + 1 < argc) {
      cfg.port = atoi(argv[++i]);
    } else if (strncmp(a, "--port=", 7) == 0) {
      cfg.port = atoi(a + 7);
    } else if (strcmp(a, "--host") == 0 && i + 1 < argc) {
      snprintf(cfg.host, sizeof cfg.host, "%s", argv[++i]);
    } else if (strncmp(a, "--host=", 7) == 0) {
      snprintf(cfg.host, sizeof cfg.host, "%s", a + 7);
    } else if (strcmp(a, "--web-root") == 0 && i + 1 < argc) {
      snprintf(cfg.web_root, sizeof cfg.web_root, "%s", argv[++i]);
    } else if (strncmp(a, "--web-root=", 11) == 0) {
      snprintf(cfg.web_root, sizeof cfg.web_root, "%s", a + 11);
    } else if (strcmp(a, "--token") == 0 && i + 1 < argc) {
      snprintf(cfg.token, sizeof cfg.token, "%s", argv[++i]);
    } else if (strncmp(a, "--token=", 8) == 0) {
      snprintf(cfg.token, sizeof cfg.token, "%s", a + 8);
    } else if (strcmp(a, "--no-open") == 0) {
      cfg.do_open = 0;
    } else if (strcmp(a, "--no-watch") == 0) {
      cfg.watch_scope = WATCH_OFF;
    } else if (strcmp(a, "--watch") == 0 && i + 1 < argc) {
      cfg.watch_scope = parse_watch(argv[++i]);
    } else if (strncmp(a, "--watch=", 8) == 0) {
      cfg.watch_scope = parse_watch(a + 8);
    } else if (a[0] == '-' && a[1] != '\0') {
      fprintf(stderr, "gander: unknown option: %s\n", a);
      usage(stderr);
      return 2;
    } else {
      path = a;
    }
  }

  if (!is_dir(path)) {
    if (is_file(path)) {
      fprintf(stderr, "gander: %s is a file, not a directory\n", path);
    } else {
      fprintf(stderr, "gander: no such directory: %s\n", path);
    }
    return 1;
  }

  char web_root[PATH_MAX + 64];
  find_web_root(cfg.web_root[0] ? cfg.web_root : NULL, web_root,
                sizeof web_root);

  return server_run(path, web_root, cfg.host, cfg.port, cfg.do_open,
                    cfg.token[0] ? cfg.token : NULL, cfg.watch_scope,
                    cfg.css[0] ? cfg.css : NULL);
}
