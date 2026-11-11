/* store.h: the in-memory store that replaces SQLite in reference-nodb. An open-addressing hash
 * table from a 64-bit id to a heap copy of the value. One thread owns it, so there are no locks. */
#ifndef STORE_H
#define STORE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct slot {
  uint64_t key;
  uint8_t *p; /* NULL = empty */
  uint32_t n;
};
struct store {
  struct slot *t;
  size_t cap, count; /* cap is a power of two */
};

static inline size_t store_home(const struct store *s, uint64_t k) {
  return (size_t)((k * 0x9E3779B97F4A7C15ull) >> 20) & (s->cap - 1);
}

static int store_init(struct store *s) {
  s->cap = 1024;
  s->count = 0;
  s->t = calloc(s->cap, sizeof *s->t);
  return s->t ? 0 : -1;
}

static const struct slot *store_get(const struct store *s, uint64_t k) {
  for (size_t i = store_home(s, k);; i = (i + 1) & (s->cap - 1)) {
    if (!s->t[i].p) return NULL;
    if (s->t[i].key == k) return &s->t[i];
  }
}

static int store_grow(struct store *s) {
  struct slot *old = s->t;
  size_t ocap = s->cap;
  struct slot *t = calloc(ocap * 2, sizeof *t);
  if (!t) return -1;
  s->t = t;
  s->cap = ocap * 2;
  for (size_t j = 0; j < ocap; j++) {
    if (!old[j].p) continue;
    size_t i = store_home(s, old[j].key);
    while (t[i].p) i = (i + 1) & (s->cap - 1);
    t[i] = old[j];
  }
  free(old);
  return 0;
}

/* Stores a copy of v[0..n). Returns 0, or -1 when memory runs out. */
static int store_put(struct store *s, uint64_t k, const void *v, size_t n) {
  uint8_t *copy = malloc(n ? n : 1);
  if (!copy) return -1;
  memcpy(copy, v, n);
  size_t i = store_home(s, k);
  for (; s->t[i].p; i = (i + 1) & (s->cap - 1)) {
    if (s->t[i].key == k) {
      free(s->t[i].p);
      s->t[i].p = copy;
      s->t[i].n = (uint32_t)n;
      return 0;
    }
  }
  if ((s->count + 1) * 4 > s->cap * 3) {
    if (store_grow(s) < 0) { free(copy); return -1; }
    for (i = store_home(s, k); s->t[i].p; i = (i + 1) & (s->cap - 1)) {}
  }
  s->t[i].key = k;
  s->t[i].p = copy;
  s->t[i].n = (uint32_t)n;
  s->count++;
  return 0;
}

/* Returns 1 if the key was there. Backward-shift deletion keeps probe runs intact. */
static int store_del(struct store *s, uint64_t k) {
  size_t m = s->cap - 1, i = store_home(s, k);
  for (;; i = (i + 1) & m) {
    if (!s->t[i].p) return 0;
    if (s->t[i].key == k) break;
  }
  free(s->t[i].p);
  for (size_t j = i;;) {
    j = (j + 1) & m;
    if (!s->t[j].p) break;
    size_t h = store_home(s, s->t[j].key);
    int movable = j > i ? (h <= i || h > j) : (h <= i && h > j);
    if (movable) { s->t[i] = s->t[j]; i = j; }
  }
  s->t[i].p = NULL;
  s->count--;
  return 1;
}

#endif
