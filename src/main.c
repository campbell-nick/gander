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
          "  --no-open         do not launch a browser\n"
          "  -h, --help        show this help and exit\n"
          "  -V, --version     show the version and exit\n"
          "\n"
          "Live reload is on by default; edits to watched files refresh the UI.\n",
          GANDER_VERSION);
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
  const char *host = "127.0.0.1";
  const char *web_override = NULL;
  const char *token = NULL;
  int port = 0;
  int do_open = 1;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(stdout);
      return 0;
    } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
      printf("gander %s\n", GANDER_VERSION);
      return 0;
    } else if (strcmp(a, "--port") == 0 && i + 1 < argc) {
      port = atoi(argv[++i]);
    } else if (strncmp(a, "--port=", 7) == 0) {
      port = atoi(a + 7);
    } else if (strcmp(a, "--host") == 0 && i + 1 < argc) {
      host = argv[++i];
    } else if (strncmp(a, "--host=", 7) == 0) {
      host = a + 7;
    } else if (strcmp(a, "--web-root") == 0 && i + 1 < argc) {
      web_override = argv[++i];
    } else if (strncmp(a, "--web-root=", 11) == 0) {
      web_override = a + 11;
    } else if (strcmp(a, "--token") == 0 && i + 1 < argc) {
      token = argv[++i];
    } else if (strncmp(a, "--token=", 8) == 0) {
      token = a + 8;
    } else if (strcmp(a, "--no-open") == 0) {
      do_open = 0;
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
  find_web_root(web_override, web_root, sizeof web_root);

  return server_run(path, web_root, host, port, do_open, token);
}
