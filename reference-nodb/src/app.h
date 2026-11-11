/* app.h: bulkapi's eight endpoints on the reference's request/response plumbing (reference-nodb).
 * One request in, one complete HTTP response out, written straight into the batch arena. Every
 * error response is a constant prepared at startup. The store is in memory (store.h). */
#ifndef APP_H
#define APP_H

#include <time.h>

#include "http.h"
#include "json.h"
#include "store.h"

#define HDR_ROOM 128      /* space kept in front of a body for its headers */
#define RESP_MAX (64 * 1024)

struct app {
  struct store st;
  int64_t now;      /* seconds, refreshed per batch */
  int64_t start;
  uint64_t n_req, n_err, n_conns;
  uint8_t *scratch; /* unescaped value of the bulk-insert line being read */
};

struct resp {
  uint8_t *p;
  unsigned n;
  uint8_t close;  /* close the connection after sending */
  uint8_t db;     /* unused here: nothing can roll back */
  uint8_t held;   /* unused here: there is no write gate */
};

/* constant responses */
#define STATIC_RESP 256
static uint8_t R_404[STATIC_RESP], R_405[STATIC_RESP], R_400_ID[STATIC_RESP], R_413[STATIC_RESP], R_500[STATIC_RESP],
    R_400_BAD[STATIC_RESP], R_200_OK[STATIC_RESP];
static unsigned L_404, L_405, L_400_ID, L_413, L_500, L_400_BAD, L_200_OK;

static unsigned mk_resp(uint8_t *dst, const char *status, const char *body, int close) {
  return (unsigned)snprintf((char *)dst, STATIC_RESP,
                            "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n%s\r\n%s", status,
                            strlen(body), close ? "Connection: close\r\n" : "", body);
}

static int app_init(struct app *a) {
  memset(a, 0, sizeof *a);
  L_404 = mk_resp(R_404, "404 Not Found", "{\"error\":\"not found\"}", 0);
  L_405 = mk_resp(R_405, "405 Method Not Allowed", "{\"error\":\"method not allowed\"}", 0);
  L_400_ID = mk_resp(R_400_ID, "400 Bad Request", "{\"error\":\"bad id\"}", 0);
  L_413 = mk_resp(R_413, "413 Payload Too Large", "{\"error\":\"response too large\"}", 0);
  L_500 = mk_resp(R_500, "500 Internal Server Error", "{\"error\":\"internal error\"}", 0);
  L_400_BAD = mk_resp(R_400_BAD, "400 Bad Request", "{\"error\":\"bad request\"}", 1);
  L_200_OK = mk_resp(R_200_OK, "200 OK", "{\"ok\":true}", 0);
  a->start = a->now = (int64_t)time(NULL);
  a->scratch = malloc(HTTP_MAX_BODY);
  if (!a->scratch) return -1;
  return store_init(&a->st);
}

/* A constant response; `close` appends nothing here, so a closing connection frames its own. */
static inline void resp_static(struct resp *out, uint8_t *free, const uint8_t *r, unsigned n) {
  memcpy(free, r, n);
  out->p = free;
  out->n = n;
}

/* Puts the status line and headers in front of a body that was written at free+HDR_ROOM. */
static void resp_frame(struct resp *out, uint8_t *free, const char *status, unsigned status_len, const char *ctype,
                       unsigned ctype_len, unsigned body_n, int close) {
  char hdr[HDR_ROOM];
  unsigned h = 0;
  memcpy(hdr + h, "HTTP/1.1 ", 9); h += 9;
  memcpy(hdr + h, status, status_len); h += status_len;
  memcpy(hdr + h, "\r\nContent-Type: ", 16); h += 16;
  memcpy(hdr + h, ctype, ctype_len); h += ctype_len;
  memcpy(hdr + h, "\r\nContent-Length: ", 18); h += 18;
  h += u64_to_str(hdr + h, body_n);
  if (close) { memcpy(hdr + h, "\r\nConnection: close", 19); h += 19; }
  memcpy(hdr + h, "\r\n\r\n", 4); h += 4;
  out->p = free + HDR_ROOM - h;
  memcpy(out->p, hdr, h);
  out->n = h + body_n;
}
#define FRAME_JSON(out, free, body_n) resp_frame(out, free, "200 OK", 6, "application/json", 16, body_n, (out)->close)

/* A decimal unsigned 64-bit number and nothing else (leading zeros and 0 are fine, as in bulkapi). */
static int parse_u64(const char *s, size_t n, uint64_t *out) {
  if (n == 0 || n > 20) return 0;
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return 0;
    uint64_t d = (uint64_t)(s[i] - '0');
    if (v > (UINT64_MAX - d) / 10) return 0;
    v = v * 10 + d;
  }
  *out = v;
  return 1;
}

static void item_json(struct ob *o, uint64_t id, const struct slot *v) {
  ob_lit(o, "{\"id\":");
  ob_u64(o, id);
  if (!v) { ob_lit(o, ",\"v\":null}"); return; }
  ob_lit(o, ",\"v\":");
  ob_json_str(o, v->p, v->n);
  ob_char(o, '}');
}

/* The next run of digits in [*p, end): returns 1 with its value, 0 at the end of the input. */
static int next_id(const char **p, const char *end, uint64_t *id) {
  const char *s = *p;
  for (;;) {
    while (s < end && (*s < '0' || *s > '9')) s++;
    if (s >= end) { *p = s; return 0; }
    const char *b = s;
    while (s < end && *s >= '0' && *s <= '9') s++;
    if (parse_u64(b, (size_t)(s - b), id)) { *p = s; return 1; }
  }
}

enum { P_NONE, P_HEALTH, P_STATS, P_ITEM, P_BULK, P_BULK_GET, P_BULK_DELETE };

/* `free` points at RESP_MAX bytes of arena. */
static void app_handle(struct app *a, const struct req *r, uint8_t *free, int writes_ok, struct resp *out) {
  (void)writes_ok;
  out->close = !r->keep_alive;
  out->db = 0;
  out->held = 0;
  a->n_req++;
  struct ob o;
  ob_init(&o, free + HDR_ROOM, RESP_MAX - HDR_ROOM);

  /* the path without its query string; one trailing slash is ignored */
  const char *path = r->path;
  unsigned plen = r->path_len;
  const char *q = memchr(path, '?', plen);
  if (q) plen = (unsigned)(q - path);
  if (plen > 1 && path[plen - 1] == '/') plen--;

  int what = P_NONE;
  const char *idp = NULL;
  unsigned idn = 0;
  if (plen == 7 && memcmp(path, "/health", 7) == 0) what = P_HEALTH;
  else if (plen == 6 && memcmp(path, "/stats", 6) == 0) what = P_STATS;
  else if (plen > 7 && memcmp(path, "/items/", 7) == 0) {
    idp = path + 7;
    idn = plen - 7;
    if (r->method == M_POST && idn == 4 && memcmp(idp, "bulk", 4) == 0) what = P_BULK;
    else if (idn == 8 && memcmp(idp, "bulk/get", 8) == 0) what = P_BULK_GET;
    else if (idn == 11 && memcmp(idp, "bulk/delete", 11) == 0) what = P_BULK_DELETE;
    else if (!memchr(idp, '/', idn)) what = P_ITEM;
  }

  switch (what) {
  case P_HEALTH:
    if (r->method != M_GET) break;
    if (out->close) { ob_lit(&o, "{\"ok\":true}"); FRAME_JSON(out, free, (unsigned)o.n); }
    else resp_static(out, free, R_200_OK, L_200_OK);
    return;

  case P_STATS:
    if (r->method != M_GET) break;
    ob_lit(&o, "{\"requests\":"); ob_u64(&o, a->n_req - 1);
    ob_lit(&o, ",\"connections\":"); ob_u64(&o, a->n_conns);
    ob_lit(&o, ",\"items\":"); ob_u64(&o, a->st.count);
    ob_char(&o, '}');
    FRAME_JSON(out, free, (unsigned)o.n);
    return;

  case P_ITEM: {
    if (r->method != M_GET && r->method != M_PUT && r->method != M_DELETE) break;
    uint64_t id;
    if (!parse_u64(idp, idn, &id)) { resp_static(out, free, R_400_ID, L_400_ID); return; }
    if (r->method == M_GET) {
      const struct slot *v = store_get(&a->st, id);
      if (!v) { resp_static(out, free, R_404, L_404); return; }
      item_json(&o, id, v);
      if (o.err) { resp_static(out, free, R_413, L_413); return; }
      FRAME_JSON(out, free, (unsigned)o.n);
    } else if (r->method == M_PUT) {
      if (store_put(&a->st, id, r->body, r->body_len) < 0) goto err500; /* body stored raw */
      ob_lit(&o, "{\"ok\":true}");
      FRAME_JSON(out, free, (unsigned)o.n);
    } else {
      if (!store_del(&a->st, id)) { resp_static(out, free, R_404, L_404); return; }
      ob_lit(&o, "{\"ok\":true}");
      FRAME_JSON(out, free, (unsigned)o.n);
    }
    return;
  }

  case P_BULK: { /* one {"id":N,"v":"text"} per line; each line is validated as JSON */
    uint64_t ok = 0, bad = 0;
    const char *p = r->body, *end = r->body + r->body_len;
    while (p < end) {
      const char *nl = memchr(p, '\n', (size_t)(end - p));
      const char *le = nl ? nl : end, *e = le;
      if (e > p && e[-1] == '\r') e--;
      if (e > p) {
        struct jwant w[2] = {{"id", 2, {0}}, {"v", 1, {0}}};
        uint64_t id;
        long n;
        if (json_doc(p, (size_t)(e - p), w, 2) != J_OBJECT || w[0].v.type != J_NUMBER || w[1].v.type != J_STRING ||
            !parse_u64(w[0].v.p, w[0].v.n, &id) ||
            (n = json_unescape(w[1].v.p, w[1].v.n, a->scratch, HTTP_MAX_BODY)) < 0) {
          bad++;
        } else {
          if (store_put(&a->st, id, a->scratch, (size_t)n) < 0) goto err500;
          ok++;
        }
      }
      p = le + 1;
    }
    ob_lit(&o, "{\"inserted\":"); ob_u64(&o, ok);
    ob_lit(&o, ",\"errors\":"); ob_u64(&o, bad);
    ob_char(&o, '}');
    FRAME_JSON(out, free, (unsigned)o.n);
    return;
  }

  case P_BULK_GET: {
    if (r->method != M_POST) break;
    const char *p = r->body, *end = r->body + r->body_len;
    uint64_t id;
    while (next_id(&p, end, &id)) {
      item_json(&o, id, store_get(&a->st, id));
      ob_char(&o, '\n');
    }
    if (o.err) { resp_static(out, free, R_413, L_413); return; } /* a response is at most RESP_MAX */
    resp_frame(out, free, "200 OK", 6, "application/x-ndjson", 20, (unsigned)o.n, out->close);
    return;
  }

  case P_BULK_DELETE: {
    if (r->method != M_POST) break;
    const char *p = r->body, *end = r->body + r->body_len;
    uint64_t id, n = 0;
    while (next_id(&p, end, &id)) n += (uint64_t)store_del(&a->st, id);
    ob_lit(&o, "{\"deleted\":"); ob_u64(&o, n);
    ob_char(&o, '}');
    FRAME_JSON(out, free, (unsigned)o.n);
    return;
  }

  default:
    resp_static(out, free, R_404, L_404);
    return;
  }
  resp_static(out, free, R_405, L_405); /* the path exists, the method does not */
  return;

err500:
  a->n_err++;
  resp_static(out, free, R_500, L_500);
}

#endif
