/* watch.c - recursive inotify watcher with debounce, for live reload.
 *
 * Linux uses inotify. Elsewhere watching is simply unavailable and
 * watch_new() returns NULL; the server keeps working, just without live
 * reload. Everything here is local; nothing touches the network.
 */
#include "gander.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
#include <dirent.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEBOUNCE_MS 120
#define MAX_WATCHES 8192
#define MAX_CHANGED 64

struct wmap {
  int wd;
  char *rel;
};

struct watcher {
  int fd;
  char *root;
  struct wmap *maps;
  size_t nmaps, cmaps;
  char **changed;
  size_t nchanged, cchanged;
  long long last_ms;
  int dirty;
};

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

struct watcher *watch_new(const char *root) {
  int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd < 0)
    return NULL;
  struct watcher *w = xmalloc(sizeof *w);
  memset(w, 0, sizeof *w);
  w->fd = fd;
  w->root = xstrdup(root);
  add_watch_rec(w, "");
  add_git_watches(w);
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

int watch_take(struct watcher *w, char ***paths, size_t *n) {
  if (!w || !w->dirty)
    return 0;
  if (now_ms() - w->last_ms < DEBOUNCE_MS)
    return 0;
  *paths = w->changed;
  *n = w->nchanged;
  w->changed = NULL;
  w->nchanged = w->cchanged = 0;
  w->dirty = 0;
  return 1;
}

#else /* not Linux: no live reload */

struct watcher {
  int unused;
};

struct watcher *watch_new(const char *root) {
  (void)root;
  return NULL;
}
void watch_free(struct watcher *w) { (void)w; }
int watch_fd(struct watcher *w) {
  (void)w;
  return -1;
}
void watch_drain(struct watcher *w) { (void)w; }
int watch_take(struct watcher *w, char ***paths, size_t *n) {
  (void)w;
  (void)paths;
  (void)n;
  return 0;
}

#endif
