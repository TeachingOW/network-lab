package bulkapi;

import java.nio.ByteBuffer;

/** Minimal JSON / NDJSON helpers for the item payloads. Not a full JSON parser. */
final class Json {
    private static final byte[] ID = Bytes.ascii("\"id\""), V = Bytes.ascii("\"v\"");
    private static final long MAX_DIV10 = Long.divideUnsigned(-1L, 10);  // 2^64 / 10

    /** Parses [off, end) as an unsigned 64-bit decimal into w.num. False if empty, non-digit or overflow. */
    static boolean parseU64(ByteBuffer b, int off, int end, Worker w) {
        if (off >= end || end - off > 20) return false;
        long v = 0;
        for (int i = off; i < end; i++) {
            int d = b.get(i) - '0';
            if (d < 0 || d > 9) return false;
            if (Long.compareUnsigned(v, MAX_DIV10) > 0) return false;
            long nv = v * 10 + d;
            if (Long.compareUnsigned(nv, v * 10) < 0) return false;
            v = nv;
        }
        w.num = v;
        return true;
    }

    /** {"id":123,"v":"text"} -> w.num, w.valOff, w.valLen, w.esc (value contains backslash escapes). */
    static boolean parseItem(ByteBuffer b, int off, int end, Worker w) {
        int p = Bytes.indexOf(b, off, end, ID);
        if (p < 0) return false;
        p += 4;
        while (p < end && (b.get(p) == ' ' || b.get(p) == ':')) p++;
        int e = p;
        while (e < end && b.get(e) >= '0' && b.get(e) <= '9') e++;
        if (!parseU64(b, p, e, w)) return false;
        int q = Bytes.indexOf(b, e, end, V);
        if (q < 0) q = Bytes.indexOf(b, off, end, V);
        if (q < 0) return false;
        q += 3;
        while (q < end && (b.get(q) == ' ' || b.get(q) == ':')) q++;
        if (q >= end || b.get(q) != '"') return false;
        int start = ++q;
        boolean esc = false;
        while (q < end && b.get(q) != '"') {
            if (b.get(q) == '\\') { esc = true; q++; }
            q++;
        }
        if (q >= end) return false;
        w.valOff = start;
        w.valLen = q - start;
        w.esc = esc;
        return true;
    }

    private static int hex(int c) {
        if (c >= '0' && c <= '9') return c - '0';
        c |= 0x20;
        return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    }

    private static int hex4(ByteBuffer b, int i, int end) {
        if (i + 4 > end) return -1;
        int v = 0;
        for (int k = 0; k < 4; k++) {
            int h = hex(b.get(i + k));
            if (h < 0) return -1;
            v = v << 4 | h;
        }
        return v;
    }

    /** Decodes JSON string escapes, including \\uXXXX and surrogate pairs, to UTF-8 bytes. */
    static byte[] unescape(ByteBuffer b, int off, int len, Worker w) {
        if (w.tmp.length < len) w.tmp = new byte[len];  // the decoded form is never longer
        byte[] o = w.tmp;
        int n = 0, end = off + len;
        for (int i = off; i < end; i++) {
            byte ch = b.get(i);
            if (ch != '\\' || i + 1 >= end) { o[n++] = ch; continue; }
            byte e = b.get(++i);
            switch (e) {
                case 'n': o[n++] = '\n'; break;
                case 't': o[n++] = '\t'; break;
                case 'r': o[n++] = '\r'; break;
                case 'b': o[n++] = '\b'; break;
                case 'f': o[n++] = '\f'; break;
                case 'u': {
                    int cp = hex4(b, i + 1, end);
                    if (cp < 0) { o[n++] = 'u'; break; }  // malformed: keep the text
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < end && b.get(i + 1) == '\\' && b.get(i + 2) == 'u') {
                        int lo = hex4(b, i + 3, end);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                    }
                    if (cp < 0x80) o[n++] = (byte) cp;
                    else if (cp < 0x800) { o[n++] = (byte) (0xC0 | cp >> 6); o[n++] = (byte) (0x80 | (cp & 0x3F)); }
                    else if (cp < 0x10000) {
                        o[n++] = (byte) (0xE0 | cp >> 12); o[n++] = (byte) (0x80 | (cp >> 6 & 0x3F)); o[n++] = (byte) (0x80 | (cp & 0x3F));
                    } else {
                        o[n++] = (byte) (0xF0 | cp >> 18); o[n++] = (byte) (0x80 | (cp >> 12 & 0x3F));
                        o[n++] = (byte) (0x80 | (cp >> 6 & 0x3F)); o[n++] = (byte) (0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: o[n++] = e;  // \" \\ \/
            }
        }
        return java.util.Arrays.copyOf(o, n);
    }
}
