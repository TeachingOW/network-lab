#include "http.h"

#include <strings.h>

#include <cstdlib>
#include <cstring>

#include "json.h"

static constexpr size_t kMaxHeader = 16 * 1024;

static const char* status_text(int s) {
    switch (s) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 204: return "204 No Content";
        case 400: return "400 Bad Request";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 413: return "413 Payload Too Large";
        case 431: return "431 Request Header Fields Too Large";
        case 501: return "501 Not Implemented";
        default: return "500 Internal Server Error";
    }
}

static void serialize(std::string& o, const Res& r, bool close_after) {
    o.reserve(o.size() + r.body.size() + 160);
    o += "HTTP/1.1 "; o += status_text(r.status);
    o += "\r\nContent-Type: "; o += r.ctype;
    o += "\r\nContent-Length: "; append_u64(o, r.body.size());
    o += close_after ? "\r\nConnection: close\r\n\r\n" : "\r\n\r\n";
    o += r.body;
}

static void fail(Conn& c, Worker& w, int status, const char* msg) {
    w.res.status = 200; w.res.ctype = "application/json"; w.res.body.clear();
    err(w.res, status, msg);
    serialize(c.out, w.res, true);
    c.want_close = true;
}

static inline bool ci_eq(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n) == 0; }

static Method parse_method(std::string_view m) {
    if (m == "GET") return M_GET;
    if (m == "POST") return M_POST;
    if (m == "PUT") return M_PUT;
    if (m == "DELETE") return M_DELETE;
    if (m == "PATCH") return M_PATCH;
    if (m == "HEAD") return M_HEAD;
    return M_UNKNOWN;
}

void process(Conn& c, Worker& w) {
    size_t pos = 0;
    u64 nreq = 0;
    c.need_total = 0;
    while (pos < c.in_len && !c.want_close) {
        const char* base = c.in + pos;
        size_t avail = c.in_len - pos;
        const char* he = (const char*)memmem(base, avail, "\r\n\r\n", 4);
        if (!he) {
            if (avail > kMaxHeader) fail(c, w, 431, "headers too large");
            break;
        }
        size_t hlen = (he - base) + 4;
        const char* le = (const char*)memchr(base, '\r', hlen);  // end of request line
        const char* sp1 = (const char*)memchr(base, ' ', le - base);
        const char* sp2 = sp1 ? (const char*)memchr(sp1 + 1, ' ', le - sp1 - 1) : nullptr;
        if (!sp2 || (size_t)(le - sp2 - 1) != 8 || memcmp(sp2 + 1, "HTTP/1.", 7) != 0) {
            fail(c, w, 400, "bad request line");
            break;
        }
        bool keep_alive = sp2[8] == '1';
        size_t cl = 0;
        bool has_te = false, expect100 = false, bad = false;
        for (const char* p = le + 2; p < he + 2;) {
            const char* nl = (const char*)memchr(p, '\n', he + 2 - p);
            const char* e = nl[-1] == '\r' ? nl - 1 : nl;
            const char* colon = (const char*)memchr(p, ':', e - p);
            if (colon) {
                size_t nlen = colon - p;
                const char* v = colon + 1;
                while (v < e && (*v == ' ' || *v == '\t')) v++;
                size_t vlen = e - v;
                if (nlen == 14 && ci_eq(p, "content-length", 14)) {
                    u64 x = 0;
                    if (!vlen || vlen > 12) bad = true;
                    for (size_t i = 0; i < vlen && !bad; i++) {
                        if (v[i] < '0' || v[i] > '9') bad = true; else x = x * 10 + (v[i] - '0');
                    }
                    cl = x;
                } else if (nlen == 10 && ci_eq(p, "connection", 10)) {
                    if (vlen >= 5 && ci_eq(v, "close", 5)) keep_alive = false;
                    else if (vlen >= 10 && ci_eq(v, "keep-alive", 10)) keep_alive = true;
                } else if (nlen == 17 && ci_eq(p, "transfer-encoding", 17)) {
                    has_te = true;
                } else if (nlen == 6 && ci_eq(p, "expect", 6)) {
                    expect100 = vlen >= 12 && ci_eq(v, "100-continue", 12);
                }
            }
            p = nl + 1;
        }
        if (bad) { fail(c, w, 400, "bad content-length"); break; }
        if (has_te) { fail(c, w, 501, "chunked bodies unsupported; send Content-Length"); break; }

        std::string_view target(sp1 + 1, sp2 - sp1 - 1);
        std::string_view path = target, query;
        if (size_t qm = target.find('?'); qm != std::string_view::npos) {
            path = target.substr(0, qm);
            query = target.substr(qm + 1);
        }
        Params params;
        int ridx = route_match(parse_method(std::string_view(base, sp1 - base)), path, params);
        if (ridx < 0) {
            w.res.status = 200; w.res.ctype = "application/json"; w.res.body.clear();
            err(w.res, ridx == -2 ? 405 : 404, ridx == -2 ? "method not allowed" : "not found");
            bool close_after = !keep_alive || cl > 0;  // don't buffer a body we won't use
            serialize(c.out, w.res, close_after);
            if (close_after) c.want_close = true;
            pos += hlen;
            nreq++;
            continue;
        }
        if (cl > kRouteMaxBody[ridx]) { fail(c, w, 413, "body too large"); break; }
        size_t total = hlen + cl;
        if (avail < total) {
            c.need_total = total;
            if (expect100 && !c.sent100) { c.out += "HTTP/1.1 100 Continue\r\n\r\n"; c.sent100 = true; }
            break;
        }
        c.sent100 = false;

        Req req{parse_method(std::string_view(base, sp1 - base)), path, query,
                std::string_view(base + hlen, cl), params};
        Res& res = w.res;
        res.status = 200; res.ctype = "application/json"; res.body.clear();
        try { kRouteHandler[ridx](req, res); }
        catch (...) { res.body.clear(); err(res, 500, "internal error"); }
        serialize(c.out, res, !keep_alive);
        if (!keep_alive) c.want_close = true;
        if (res.body.capacity() > (1u << 20)) std::string().swap(res.body);
        pos += total;
        nreq++;
    }
    if (nreq) w.st->req.store(w.st->req.load(std::memory_order_relaxed) + nreq, std::memory_order_relaxed);
    if (c.want_close) { c.in_len = 0; return; }
    if (pos) {
        c.in_len -= pos;
        if (c.in_len) memmove(c.in, c.in + pos, c.in_len);
        if (c.need_total) c.need_total -= pos;
    }
    if (c.in_len == 0 && c.in_cap > kBigBuf) {
        free(c.in);
        c.in = (char*)malloc(kInitBuf);
        c.in_cap = kInitBuf;
    }
}
