/* walk.c - collect the file tree of the previewed directory. */
#include "gander.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void filelist_init(struct filelist *fl) {
  fl->v = NULL;
  fl->n = 0;
  fl->cap = 0;
}

void filelist_free(struct filelist *fl) {
  for (size_t i = 0; i < fl->n; i++)
    free(fl->v[i]);
  free(fl->v);
  fl->v = NULL;
  fl->n = fl->cap = 0;
}

void filelist_add(struct filelist *fl, char *s) {
  if (fl->n == fl->cap) {
    fl->cap = fl->cap ? fl->cap * 2 : 64;
    fl->v = xrealloc(fl->v, fl->cap * sizeof(char *));
  }
  fl->v[fl->n++] = s;
}

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void filelist_sort(struct filelist *fl) {
  if (fl->n > 1)
    qsort(fl->v, fl->n, sizeof(char *), cmp_str);
}

/* Directories that are never interesting to browse. */
static int skip_dir(const char *name) {
  return strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0 ||
         strcmp(name, ".cache") == 0;
}

void walk_dir(const char *root, const char *rel, struct filelist *fl, int max) {
  if ((int)fl->n >= max)
    return;

  char dir[4096];
  if (rel[0])
    snprintf(dir, sizeof dir, "%s/%s", root, rel);
  else
    snprintf(dir, sizeof dir, "%s", root);

  DIR *d = opendir(dir);
  if (!d)
    return;

  struct dirent *e;
  while ((e = readdir(d))) {
    if ((int)fl->n >= max)
      break;
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;

    char child[4096];
    if (rel[0])
      snprintf(child, sizeof child, "%s/%s", rel, e->d_name);
    else
      snprintf(child, sizeof child, "%s", e->d_name);
    if (strlen(child) >= sizeof child - 1)
      continue;

    char full[4200];
    snprintf(full, sizeof full, "%s/%s", root, child);

    struct stat st;
    if (lstat(full, &st) != 0)
      continue;
    if (S_ISLNK(st.st_mode))
      continue; /* avoid symlink loops; also skips symlinked files */
    if (S_ISDIR(st.st_mode)) {
      if (skip_dir(e->d_name))
        continue;
      walk_dir(root, child, fl, max);
    } else if (S_ISREG(st.st_mode)) {
      filelist_add(fl, xstrdup(child));
    }
  }
  closedir(d);
}
