// Route handlers. One `void h_<name>(const Req&, Res&)` per handler named in
// api.spec; a spec route without a handler here fails at link time.

#include <algorithm>
#include <charconv>
#include <cstring>

#include "json.h"
#include "store.h"
#include "types.h"

void h_health(const Req&, Res& r) { r.body += "{\"ok\":true}"; }

void h_stats(const Req&, Res& r) {
    u64 req = 0, conns = 0, items = 0;
    for (int i = 0; i < g_nworkers; i++) {
        req += g_stats[i].req.load(std::memory_order_relaxed);
        conns += g_stats[i].conns.load(std::memory_order_relaxed);
    }
    for (auto& s : g_shards) { Guard g(s); items += s.m.size(); }
    r.body += "{\"requests\":"; append_u64(r.body, req);
    r.body += ",\"connections\":"; append_u64(r.body, conns);
    r.body += ",\"items\":"; append_u64(r.body, items);
    r.body += "}";
}

// {"id":<id>,"v":"<escaped value>"} or {"id":<id>,"v":null}, then `tail` (may be empty).
// The fixed parts are assembled on the stack and appended in one call.
static void item_json(std::string& o, u64 id, const Slot* v, const char* tail = "", size_t tail_len = 0) {
    char b[48];
    memcpy(b, "{\"id\":", 6);
    char* p = std::to_chars(b + 6, b + 30, id).ptr;
    if (!v) {
        memcpy(p, ",\"v\":null}", 10);
        p += 10;
        memcpy(p, tail, tail_len);
        o.append(b, p + tail_len - b);
        return;
    }
    memcpy(p, ",\"v\":\"", 6);
    o.append(b, p + 6 - b);
    json_escape(o, std::string_view(v->data(), v->len));
    memcpy(b, "\"}", 2);
    memcpy(b + 2, tail, tail_len);
    o.append(b, 2 + tail_len);
}

void h_get_item(const Req& q, Res& r) {
    u64 id;
    if (!parse_u64(q.params.v[0], id)) return err(r, 400, "bad id");
    Shard& s = shard_for(id);
    Guard g(s);
    const Slot* v = s.m.find(id);
    if (!v) return err(r, 404, "not found");
    item_json(r.body, id, v);
}

void h_put_item(const Req& q, Res& r) {
    u64 id;
    if (!parse_u64(q.params.v[0], id)) return err(r, 400, "bad id");
    {
        Shard& s = shard_for(id);
        Guard g(s);
        s.m.put(id, q.body.data(), q.body.size());  // body stored raw
    }
    r.body += "{\"ok\":true}";
}

void h_delete_item(const Req& q, Res& r) {
    u64 id;
    if (!parse_u64(q.params.v[0], id)) return err(r, 400, "bad id");
    bool found;
    {
        Shard& s = shard_for(id);
        Guard g(s);
        found = s.m.erase(id);
    }
    if (!found) return err(r, 404, "not found");
    r.body += "{\"ok\":true}";
}

void h_bulk_insert(const Req& q, Res& r) {
    u64 ok = 0, bad = 0;
    std::string tmp;
    for_each_line(q.body, [&](std::string_view line) {
        u64 id; std::string_view v; bool esc;
        if (!parse_item(line, id, v, esc)) { bad++; return; }
        if (esc) { tmp.clear(); json_unescape(tmp, v); v = tmp; }
        Shard& s = shard_for(id);
        Guard g(s);
        s.m.put(id, v.data(), v.size());
        ok++;
    });
    r.body += "{\"inserted\":"; append_u64(r.body, ok);
    r.body += ",\"errors\":"; append_u64(r.body, bad);
    r.body += "}";
}

void h_bulk_get(const Req& q, Res& r) {
    r.ctype = kNdjson;
    // An id in the body takes about 7 bytes and its output line about 48: reserve once rather than
    // let the buffer double (and copy itself) several times. Capped; past that it grows as usual.
    r.body.reserve(r.body.size() + std::min<size_t>(q.body.size() * 8 + 64, 4u << 20));
    for_each_id(q.body, [&](u64 id) {
        Shard& s = shard_for(id);
        Guard g(s);
        item_json(r.body, id, s.m.find(id), "\n", 1);
    });
}

void h_bulk_delete(const Req& q, Res& r) {
    u64 n = 0;
    for_each_id(q.body, [&](u64 id) {
        Shard& s = shard_for(id);
        Guard g(s);
        n += s.m.erase(id);
    });
    r.body += "{\"deleted\":"; append_u64(r.body, n);
    r.body += "}";
}
