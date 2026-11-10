package bulkapi;

import java.nio.ByteBuffer;
import java.util.HashMap;
import java.util.Map;

/** Route handlers: one per handler name in api.spec. A spec route without one fails at startup. */
final class Handlers {
    interface Handler { void handle(Worker w, Out o); }

    static final byte[] JSON = Bytes.ascii("application/json"), NDJSON = Bytes.ascii("application/x-ndjson");
    private static final byte[] OK = Bytes.ascii("{\"ok\":true}");
    private static final byte[] ERR_A = Bytes.ascii("{\"error\":\""), ERR_B = Bytes.ascii("\"}");
    private static final byte[] BAD_ID = Bytes.ascii("bad id"), NOT_FOUND = Bytes.ascii("not found");
    private static final byte[] ID = Bytes.ascii("{\"id\":"), V = Bytes.ascii(",\"v\":\""), V_END = Bytes.ascii("\"}"),
            V_NULL = Bytes.ascii(",\"v\":null}");
    private static final byte[] S_REQ = Bytes.ascii("{\"requests\":"), S_CONN = Bytes.ascii(",\"connections\":"),
            S_ITEMS = Bytes.ascii(",\"items\":"), INSERTED = Bytes.ascii("{\"inserted\":"), ERRORS = Bytes.ascii(",\"errors\":"),
            DELETED = Bytes.ascii("{\"deleted\":");

    private static final Map<String, Handler> BY_NAME = new HashMap<>();
    static {
        BY_NAME.put("health", Handlers::health);
        BY_NAME.put("stats", Handlers::stats);
        BY_NAME.put("get_item", Handlers::getItem);
        BY_NAME.put("put_item", Handlers::putItem);
        BY_NAME.put("delete_item", Handlers::deleteItem);
        BY_NAME.put("bulk_insert", Handlers::bulkInsert);
        BY_NAME.put("bulk_get", Handlers::bulkGet);
        BY_NAME.put("bulk_delete", Handlers::bulkDelete);
    }
    static Handler byName(String name) { return BY_NAME.get(name); }

    static void err(Worker w, Out o, int status, byte[] msg) {
        w.status = status;
        o.put(ERR_A);
        o.put(msg);
        o.put(ERR_B);
    }

    private static void itemJson(Out o, long id, byte[] v) {
        o.put(ID);
        o.putU64(id);
        if (v == null) { o.put(V_NULL); return; }
        o.put(V);
        o.putEscaped(v);
        o.put(V_END);
    }

    static void health(Worker w, Out o) { o.put(OK); }

    static void stats(Worker w, Out o) {
        long req = 0, conns = 0, items = 0;
        for (Worker x : Server.workers) { req += x.req; conns += x.conns; }
        for (Store.Shard s : Store.shards) synchronized (s) { items += s.size; }
        o.put(S_REQ); o.putU64(req);
        o.put(S_CONN); o.putU64(conns);
        o.put(S_ITEMS); o.putU64(items);
        o.put((byte) '}');
    }

    static void getItem(Worker w, Out o) {
        if (!Json.parseU64(w.in, w.pOff[0], w.pOff[0] + w.pLen[0], w)) { err(w, o, 400, BAD_ID); return; }
        long id = w.num;
        Store.Shard s = Store.shardFor(id);
        byte[] v;
        synchronized (s) { v = s.get(id); }
        if (v == null) { err(w, o, 404, NOT_FOUND); return; }
        itemJson(o, id, v);
    }

    static void putItem(Worker w, Out o) {
        if (!Json.parseU64(w.in, w.pOff[0], w.pOff[0] + w.pLen[0], w)) { err(w, o, 400, BAD_ID); return; }
        long id = w.num;
        byte[] v = Bytes.copyOut(w.in, w.bodyOff, w.bodyLen);  // body stored raw
        Store.Shard s = Store.shardFor(id);
        synchronized (s) { s.put(id, v); }
        o.put(OK);
    }

    static void deleteItem(Worker w, Out o) {
        if (!Json.parseU64(w.in, w.pOff[0], w.pOff[0] + w.pLen[0], w)) { err(w, o, 400, BAD_ID); return; }
        long id = w.num;
        Store.Shard s = Store.shardFor(id);
        boolean removed;
        synchronized (s) { removed = s.remove(id); }
        if (!removed) { err(w, o, 404, NOT_FOUND); return; }
        o.put(OK);
    }

    static void bulkInsert(Worker w, Out o) {
        ByteBuffer b = w.in;
        long ok = 0, bad = 0;
        int p = w.bodyOff, end = p + w.bodyLen;
        while (p < end) {  // one item per non-empty line (LF or CRLF)
            int nl = Bytes.indexOf(b, p, end, '\n');
            int le = nl < 0 ? end : nl;
            int e = le > p && b.get(le - 1) == '\r' ? le - 1 : le;
            if (e > p) {
                if (!Json.parseItem(b, p, e, w)) bad++;
                else {
                    long id = w.num;
                    byte[] v = w.esc ? Json.unescape(b, w.valOff, w.valLen, w) : Bytes.copyOut(b, w.valOff, w.valLen);
                    Store.Shard s = Store.shardFor(id);
                    synchronized (s) { s.put(id, v); }
                    ok++;
                }
            }
            p = le + 1;
        }
        o.put(INSERTED); o.putU64(ok);
        o.put(ERRORS); o.putU64(bad);
        o.put((byte) '}');
    }

    /** Advances to the next run of digits in [p, end); returns its end or -1, with the id in w.num. */
    private static int nextId(ByteBuffer b, int p, int end, Worker w) {
        for (;;) {
            while (p < end && (b.get(p) < '0' || b.get(p) > '9')) p++;
            if (p >= end) return -1;
            int s = p;
            while (p < end && b.get(p) >= '0' && b.get(p) <= '9') p++;
            if (Json.parseU64(b, s, p, w)) return p;
        }
    }

    static void bulkGet(Worker w, Out o) {
        w.ctype = NDJSON;
        ByteBuffer b = w.in;
        int end = w.bodyOff + w.bodyLen;
        for (int p = nextId(b, w.bodyOff, end, w); p >= 0; p = nextId(b, p, end, w)) {
            long id = w.num;
            Store.Shard s = Store.shardFor(id);
            byte[] v;
            synchronized (s) { v = s.get(id); }
            itemJson(o, id, v);
            o.put((byte) '\n');
        }
    }

    static void bulkDelete(Worker w, Out o) {
        ByteBuffer b = w.in;
        long n = 0;
        int end = w.bodyOff + w.bodyLen;
        for (int p = nextId(b, w.bodyOff, end, w); p >= 0; p = nextId(b, p, end, w)) {
            long id = w.num;
            Store.Shard s = Store.shardFor(id);
            synchronized (s) { if (s.remove(id)) n++; }
        }
        o.put(DELETED); o.putU64(n);
        o.put((byte) '}');
    }
}
