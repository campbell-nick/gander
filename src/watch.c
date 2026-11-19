/* watch.c - recursive live-reload watcher with debounce.
 *
 * Two backends, chosen at build time (see the Makefile's WATCH knob):
 *
 *   inotify   Linux (or -DGANDER_WATCH_INOTIFY). Events come from the kernel;
 *             watch_fd() is pollable and watch_drain() reads them.
 *   polling   everywhere else (or -DGANDER_WATCH_POLL). A periodic scan of the
 *             tree, diffed against the previous snapshot; watch_fd() is -1 and
 *             watch_poll() drives it from the server's loop.
 *
 * Both expose the same API, so the server does not care which is in use.
 * Everything here is local; nothing touches the network.
 */
#include "gander.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(GANDER_WATCH_INOTIFY)
/* inotify forced by the build */
#elif defined(GANDER_WATCH_POLL)
/* polling forced by the build */
#elif defined(__linux__)
#define GANDER_WATCH_INOTIFY 1
#else
#define GANDER_WATCH_POLL 1
#endif

#define DEBOUNCE_MS 120
#define MAX_CHANGED 64

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* In commit mode only history/ref changes matter: a commit, a tag, a branch
 * switch or a fetch. Everything else under .git - most importantly the index,
 * which `git status` rewrites - is ignored, so a status poll can never feed
 * back into another refresh. */
static int commit_path(const char *rel) {
  if (strncmp(rel, ".git/", 5) != 0)
    return 0;
  const char *p = rel + 5;
  if (strncmp(p, "refs/", 5) == 0 || strncmp(p, "logs/", 5) == 0)
    return 1;
  static const char *files[] = {"HEAD",         "packed-refs", "ORIG_HEAD",
                                "MERGE_HEAD",   "FETCH_HEAD",  "CHERRY_PICK_HEAD",
                                "REVERT_HEAD",  "COMMIT_EDITMSG", "TAG_EDITMSG",
                                NULL};
  for (int i = 0; files[i]; i++)
    if (strcmp(p, files[i]) == 0)
      return 1;
  return 0;
}

#ifdef GANDER_WATCH_INOTIFY

#include <dirent.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_WATCHES 8192

struct wmap {
  int wd;
  char *rel;
};

struct watcher {
  int fd;
  int scope;
  char *root;
  struct wmap *maps;
  size_t nmaps, cmaps;
  char **changed;
  size_t nchanged, cchanged;
  long long last_ms;
  int dirty;
};

static const char *map_rel(struct watcher *w, int wd) {
  for (size_t i = 0; i < w->nmaps; i++)
    if (w->maps[i].wd == wd)
      return w->maps[i].rel;
  return NULL;
}

static void map_put(struct watcher *w, int wd, const char *rel) {
  for (size_t i = 0; i < w->nmaps; i++)
    if (w->maps[i].wd == wd) {
      free(w->maps[i].rel);
      w->maps[i].rel = xstrdup(rel);
      return;
    }
  if (w->nmaps == w->cmaps) {
    w->cmaps = w->cmaps ? w->cmaps * 2 : 64;
    w->maps = xrealloc(w->maps, w->cmaps * sizeof(*w->maps));
  }
  w->maps[w->nmaps].wd = wd;
  w->maps[w->nmaps].rel = xstrdup(rel);
  w->nmaps++;
}

static void map_del(struct watcher *w, int wd) {
  for (size_t i = 0; i < w->nmaps; i++)
    if (w->maps[i].wd == wd) {
      free(w->maps[i].rel);
      w->maps[i] = w->maps[w->nmaps - 1];
      w->nmaps--;
      return;
    }
}

static void add_changed(struct watcher *w, const char *rel) {
  if (!rel || !rel[0])
    return;
  if (w->scope == WATCH_COMMIT && !commit_path(rel))
    return;
  for (size_t i = 0; i < w->nchanged; i++)
    if (strcmp(w->changed[i], rel) == 0)
      return;
  if (w->nchanged >= MAX_CHANGED)
    return;
  if (w->nchanged == w->cchanged) {
    w->cchanged = w->cchanged ? w->cchanged * 2 : 16;
    w->changed = xrealloc(w->changed, w->cchanged * sizeof(char *));
  }
  w->changed[w->nchanged++] = xstrdup(rel);
}

static void add_watch_rec(struct watcher *w, const char *rel) {
  if (w->nmaps >= MAX_WATCHES)
    return;

  char dir[4096];
  if (rel[0])
    snprintf(dir, sizeof dir, "%s/%s", w->root, rel);
  else
    snprintf(dir, sizeof dir, "%s", w->root);

  int wd = inotify_add_watch(w->fd, dir,
                             IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
                                 IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                                 IN_DELETE_SELF | IN_MOVE_SELF);
  if (wd >= 0)
    map_put(w, wd, rel);

  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    if (strcmp(e->d_name, ".git") == 0 || strcmp(e->d_name, "node_modules") == 0)
      continue;
    char child[4096];
    if (rel[0])
      snprintf(child, sizeof child, "%s/%s", rel, e->d_name);
    else
      snprintf(child, sizeof child, "%s", e->d_name);
    if (strlen(child) >= sizeof child - 1)
      continue;
    char full[4200];
    snprintf(full, sizeof full, "%s/%s", w->root, child);
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
      add_watch_rec(w, child);
  }
  closedir(d);
}

/* Watch the git metadata that changes on commit, stage, checkout or branch
 * switch, so the UI can refresh its git state. Descends through refs/ and
 * logs/ but never into objects/ (which is huge and irrelevant here). */
static void add_git_rec(struct watcher *w, const char *rel) {
  if (w->nmaps >= MAX_WATCHES)
    return;

  char dir[4096];
  snprintf(dir, sizeof dir, "%s/%s", w->root, rel);
  struct stat st;
  if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
    return;

  int wd = inotify_add_watch(w->fd, dir,
                             IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
                                 IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                                 IN_DELETE_SELF | IN_MOVE_SELF);
  if (wd >= 0)
    map_put(w, wd, rel);

  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    if (strcmp(e->d_name, "objects") == 0)
      continue;
    char child[4096];
    snprintf(child, sizeof child, "%s/%s", rel, e->d_name);
    if (strlen(child) >= sizeof child - 1)
      continue;
    char full[4200];
    snprintf(full, sizeof full, "%s/%s", w->root, child);
    struct stat cs;
    if (stat(full, &cs) == 0 && S_ISDIR(cs.st_mode))
      add_git_rec(w, child);
  }
  closedir(d);
}

static void add_git_watches(struct watcher *w) {
  char gitdir[4096];
  snprintf(gitdir, sizeof gitdir, "%s/.git", w->root);
  struct stat st;
  if (stat(gitdir, &st) == 0 && S_ISDIR(st.st_mode))
    add_git_rec(w, ".git");
}

/* Commit mode: watch only the refs and reflogs, plus the top of .git for
 * HEAD and packed-refs. Nothing under the working tree or the index is
 * watched, so editing files (and the index rewrites that follow a status
 * poll) never trigger a refresh. */
static void add_git_commit_watches(struct watcher *w) {
  char gitdir[4096];
  snprintf(gitdir, sizeof gitdir, "%s/.git", w->root);
  struct stat st;
  if (stat(gitdir, &st) != 0 || !S_ISDIR(st.st_mode))
    return;

  int wd = inotify_add_watch(w->fd, gitdir,
                             IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
                                 IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                                 IN_DELETE_SELF | IN_MOVE_SELF);
  if (wd >= 0)
    map_put(w, wd, ".git");
  add_git_rec(w, ".git/refs");
  add_git_rec(w, ".git/logs");
}

struct watcher *watch_new(const char *root, int scope) {
  int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd < 0)
    return NULL;
  struct watcher *w = xmalloc(sizeof *w);
  memset(w, 0, sizeof *w);
  w->fd = fd;
  w->scope = scope;
  w->root = xstrdup(root);
  if (scope == WATCH_COMMIT) {
    add_git_commit_watches(w);
  } else {
    add_watch_rec(w, "");
    add_git_watches(w);
  }
  return w;
}

void watch_free(struct watcher *w) {
  if (!w)
    return;
  for (size_t i = 0; i < w->nmaps; i++)
    free(w->maps[i].rel);
  free(w->maps);
  for (size_t i = 0; i < w->nchanged; i++)
    free(w->changed[i]);
  free(w->changed);
  free(w->root);
  close(w->fd);
  free(w);
}

int watch_fd(struct watcher *w) { return w ? w->fd : -1; }

void watch_drain(struct watcher *w) {
  if (!w)
    return;
  char buf[65536] __attribute__((aligned(8)));
  for (;;) {
    ssize_t n = read(w->fd, buf, sizeof buf);
    if (n <= 0)
      break;
    for (char *p = buf; p < buf + n;) {
      struct inotify_event *ev = (struct inotify_event *)p;
      const char *rel_dir = map_rel(w, ev->wd);
      char rel[4096];
      if (rel_dir && ev->len > 0 && ev->name[0])
        snprintf(rel, sizeof rel, "%s%s%s", rel_dir, rel_dir[0] ? "/" : "",
                 ev->name);
      else
        snprintf(rel, sizeof rel, "%s", rel_dir ? rel_dir : "");

      if (ev->mask & IN_IGNORED) {
        map_del(w, ev->wd);
      } else if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) &&
                 (ev->mask & IN_ISDIR)) {
        add_watch_rec(w, rel);
        add_changed(w, rel);
      } else {
        add_changed(w, rel);
      }
      w->dirty = 1;
      w->last_ms = now_ms();
      p += sizeof(struct inotify_event) + ev->len;
    }
  }
}

/* inotify is event-driven through watch_fd(); nothing to poll. */
void watch_poll(struct watcher *w) { (void)w; }

int watch_take(struct watcher *w, char ***paths, size_t *n) {
  if (!w || !w->dirty)
    return 0;
  if (now_ms() - w->last_ms < DEBOUNCE_MS)
    return 0;
  if (w->nchanged == 0) {
    /* everything this round was filtered out (e.g. the index in commit
     * mode); drop it silently rather than waking every client. */
    w->dirty = 0;
    return 0;
  }
  *paths = w->changed;
  *n = w->nchanged;
  w->changed = NULL;
  w->nchanged = w->cchanged = 0;
  w->dirty = 0;
  return 1;
}

#else /* GANDER_WATCH_POLL: a portable scan-and-diff backend */

#include <dirent.h>
#include <sys/stat.h>

#define POLL_INTERVAL_MS 500
#define MAX_ENTRIES 100000

struct fentry {
  char *rel;
  long long stamp; /* mtime in nanoseconds */
  long long size;
};

struct watcher {
  int scope;
  char *root;
  struct fentry *snap;
  size_t nsnap, csnap;
  char **changed;
  size_t nchanged, cchanged;
  long long last_scan_ms;
  long long last_ms;
  int dirty;
};

static void add_changed(struct watcher *w, const char *rel) {
  if (!rel || !rel[0])
    return;
  if (w->scope == WATCH_COMMIT && !commit_path(rel))
    return;
  for (size_t i = 0; i < w->nchanged; i++)
    if (strcmp(w->changed[i], rel) == 0)
      return;
  if (w->nchanged >= MAX_CHANGED)
    return;
  if (w->nchanged == w->cchanged) {
    w->cchanged = w->cchanged ? w->cchanged * 2 : 16;
    w->changed = xrealloc(w->changed, w->cchanged * sizeof(char *));
  }
  w->changed[w->nchanged++] = xstrdup(rel);
}

static long long file_stamp(const struct stat *st) {
#ifdef __APPLE__
  return (long long)st->st_mtimespec.tv_sec * 1000000000LL +
         st->st_mtimespec.tv_nsec;
#else
  return (long long)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
#endif
}

static int cmp_entry(const void *a, const void *b) {
  return strcmp(((const struct fentry *)a)->rel,
                ((const struct fentry *)b)->rel);
}

/* Append every file under root/rel to the snapshot. In git mode the walk is
 * confined to .git and skips objects/ (huge and irrelevant here); in commit
 * scope it keeps only the history/ref paths commit_path() allows. */
static void scan_rec(struct watcher *w, const char *rel, int gitmode,
                     struct fentry **v, size_t *n, size_t *cap) {
  char dir[4096];
  if (rel[0])
    snprintf(dir, sizeof dir, "%s/%s", w->root, rel);
  else
    snprintf(dir, sizeof dir, "%s", w->root);

  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    if (gitmode) {
      if (strcmp(e->d_name, "objects") == 0)
        continue;
    } else if (strcmp(e->d_name, ".git") == 0 ||
               strcmp(e->d_name, "node_modules") == 0) {
      continue;
    }
    char child[4096];
    if (rel[0])
      snprintf(child, sizeof child, "%s/%s", rel, e->d_name);
    else
      snprintf(child, sizeof child, "%s", e->d_name);
    if (strlen(child) >= sizeof child - 1)
      continue;

    char full[4200];
    snprintf(full, sizeof full, "%s/%s", w->root, child);
    struct stat st;
    if (stat(full, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      scan_rec(w, child, gitmode, v, n, cap);
    } else if (S_ISREG(st.st_mode)) {
      if (gitmode && w->scope == WATCH_COMMIT && !commit_path(child))
        continue;
      if (*n >= MAX_ENTRIES)
        continue;
      if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 128;
        *v = xrealloc(*v, *cap * sizeof(**v));
      }
      (*v)[*n].rel = xstrdup(child);
      (*v)[*n].stamp = file_stamp(&st);
      (*v)[*n].size = (long long)st.st_size;
      (*n)++;
    }
  }
  closedir(d);
}

/* Take a fresh snapshot and diff it against the previous one. With report set,
 * added/removed/changed paths are queued (via add_changed) and the watcher is
 * marked dirty; without it the snapshot is just primed as a baseline. */
static void poll_scan(struct watcher *w, int report) {
  struct fentry *ns = NULL;
  size_t nns = 0, cns = 0;

  if (w->scope == WATCH_COMMIT) {
    scan_rec(w, ".git", 1, &ns, &nns, &cns);
  } else {
    scan_rec(w, "", 0, &ns, &nns, &cns);
    scan_rec(w, ".git", 1, &ns, &nns, &cns);
  }
  qsort(ns, nns, sizeof *ns, cmp_entry);

  size_t i = 0, j = 0;
  int any = 0;
  while (i < w->nsnap || j < nns) {
    int c;
    if (i >= w->nsnap)
      c = 1; /* new only */
    else if (j >= nns)
      c = -1; /* old only */
    else
      c = strcmp(w->snap[i].rel, ns[j].rel);
    if (c == 0) {
      if (w->snap[i].stamp != ns[j].stamp || w->snap[i].size != ns[j].size) {
        if (report)
          add_changed(w, ns[j].rel);
        any = 1;
      }
      i++;
      j++;
    } else if (c < 0) { /* removed */
      if (report)
        add_changed(w, w->snap[i].rel);
      any = 1;
      i++;
    } else { /* added */
      if (report)
        add_changed(w, ns[j].rel);
      any = 1;
      j++;
    }
  }

  for (size_t k = 0; k < w->nsnap; k++)
    free(w->snap[k].rel);
  free(w->snap);
  w->snap = ns;
  w->nsnap = nns;
  w->csnap = cns;

  if (report && any) {
    w->dirty = 1;
    w->last_ms = now_ms();
  }
  w->last_scan_ms = now_ms();
}

struct watcher *watch_new(const char *root, int scope) {
  struct watcher *w = xmalloc(sizeof *w);
  memset(w, 0, sizeof *w);
  w->scope = scope;
  w->root = xstrdup(root);
  poll_scan(w, 0); /* prime the baseline; report nothing yet */
  return w;
}

void watch_free(struct watcher *w) {
  if (!w)
    return;
  for (size_t i = 0; i < w->nsnap; i++)
    free(w->snap[i].rel);
  free(w->snap);
  for (size_t i = 0; i < w->nchanged; i++)
    free(w->changed[i]);
  free(w->changed);
  free(w->root);
  free(w);
}

int watch_fd(struct watcher *w) {
  (void)w;
  return -1;
}

void watch_drain(struct watcher *w) { (void)w; }

/* Scan for changes if the interval has elapsed. Called once per server turn,
 * so a busy loop cannot spin the scanner. */
void watch_poll(struct watcher *w) {
  if (!w)
    return;
  if (now_ms() - w->last_scan_ms < POLL_INTERVAL_MS)
    return;
  poll_scan(w, 1);
}

int watch_take(struct watcher *w, char ***paths, size_t *n) {
  if (!w || !w->dirty)
    return 0;
  if (now_ms() - w->last_ms < DEBOUNCE_MS)
    return 0;
  if (w->nchanged == 0) {
    w->dirty = 0;
    return 0;
  }
  *paths = w->changed;
  *n = w->nchanged;
  w->changed = NULL;
  w->nchanged = w->cchanged = 0;
  w->dirty = 0;
  return 1;
}

#endif

const char *watch_backend(void) {
#ifdef GANDER_WATCH_INOTIFY
  return "inotify";
#else
  return "poll";
#endif
}
