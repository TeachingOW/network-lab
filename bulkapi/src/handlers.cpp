// Route handlers. One `void h_<name>(const Req&, Res&)` per handler named in
// api.spec; a spec route without a handler here fails at link time.

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

static void item_json(std::string& o, u64 id, const std::string* v) {
    o += "{\"id\":"; append_u64(o, id);
    if (v) { o += ",\"v\":\""; json_escape(o, *v); o += "\"}"; }
    else o += ",\"v\":null}";
}

void h_get_item(const Req& q, Res& r) {
    u64 id;
    if (!parse_u64(q.params.v[0], id)) return err(r, 400, "bad id");
    Shard& s = shard_for(id);
    Guard g(s);
    auto it = s.m.find(id);
    if (it == s.m.end()) return err(r, 404, "not found");
    item_json(r.body, id, &it->second);
}

void h_put_item(const Req& q, Res& r) {
    u64 id;
    if (!parse_u64(q.params.v[0], id)) return err(r, 400, "bad id");
    {
        Shard& s = shard_for(id);
        Guard g(s);
        s.m[id].assign(q.body.data(), q.body.size());  // body stored raw
    }
    r.body += "{\"ok\":true}";
}

void h_delete_item(const Req& q, Res& r) {
    u64 id;
    if (!parse_u64(q.params.v[0], id)) return err(r, 400, "bad id");
    size_t n;
    {
        Shard& s = shard_for(id);
        Guard g(s);
        n = s.m.erase(id);
    }
    if (!n) return err(r, 404, "not found");
    r.body += "{\"ok\":true}";
}

void h_bulk_insert(const Req& q, Res& r) {
    u64 ok = 0, bad = 0;
    std::string tmp;
    for_each_line(q.body, [&](std::string_view line) {
        u64 id; std::string_view v; bool esc;
        if (!parse_item(line, id, v, esc)) { bad++; return; }
        if (esc) { tmp.clear(); json_unescape(tmp, v); }
        Shard& s = shard_for(id);
        Guard g(s);
        std::string& slot = s.m[id];
        if (esc) slot.assign(tmp); else slot.assign(v.data(), v.size());
        ok++;
    });
    r.body += "{\"inserted\":"; append_u64(r.body, ok);
    r.body += ",\"errors\":"; append_u64(r.body, bad);
    r.body += "}";
}

void h_bulk_get(const Req& q, Res& r) {
    r.ctype = "application/x-ndjson";
    for_each_id(q.body, [&](u64 id) {
        Shard& s = shard_for(id);
        Guard g(s);
        auto it = s.m.find(id);
        item_json(r.body, id, it == s.m.end() ? nullptr : &it->second);
        r.body += '\n';
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
