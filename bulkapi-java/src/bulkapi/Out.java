package bulkapi;

import java.nio.ByteBuffer;

/** Growable direct output buffer. Responses are written once: body first, header filled in after. */
final class Out {
    static final int HDR = 160;       // room reserved in front of every body for its header
    static final int SMALL = 4096;    // bodies up to this size are moved down to close the gap
    private static final byte[] HTTP = Bytes.ascii("HTTP/1.1 ");
    private static final byte[] CTYPE = Bytes.ascii("\r\nContent-Type: ");
    private static final byte[] CLOSE = Bytes.ascii("\r\nConnection: close");
    private static final byte[] CLEN = Bytes.ascii("\r\nContent-Length: ");

    ByteBuffer b = ByteBuffer.allocateDirect(Conn.INIT);  // position = end of written data
    private final byte[] hdr = new byte[HDR];
    private final byte[] num = new byte[20];

    void ensure(int n) {
        if (b.remaining() >= n) return;
        ByteBuffer nb = ByteBuffer.allocateDirect(Math.max(b.capacity() * 2, b.position() + n));
        b.flip();
        nb.put(b);
        b = nb;
    }

    void put(byte x) { ensure(1); b.put(x); }
    void put(byte[] a) { ensure(a.length); b.put(a); }
    void put(byte[] a, int off, int len) { ensure(len); b.put(a, off, len); }

    /** Decimal digits of v read as an unsigned 64-bit number. */
    void putU64(long v) {
        int i = 20;
        if (v < 0) {  // top bit set: peel one digit with unsigned division, the rest fits in a long
            num[--i] = (byte) ('0' + Long.remainderUnsigned(v, 10));
            v = Long.divideUnsigned(v, 10);
        }
        do { num[--i] = (byte) ('0' + v % 10); v /= 10; } while (v != 0);
        put(num, i, 20 - i);
    }

    /** s with JSON string escaping applied; clean runs are copied in one go. */
    void putEscaped(byte[] s) {
        int run = 0;
        for (int i = 0; i < s.length; i++) {
            int ch = s[i] & 0xff;
            if (ch >= 0x20 && ch != '"' && ch != '\\') continue;
            put(s, run, i - run);
            run = i + 1;
            ensure(6);
            b.put((byte) '\\');
            switch (ch) {
                case '"': b.put((byte) '"'); break;
                case '\\': b.put((byte) '\\'); break;
                case '\n': b.put((byte) 'n'); break;
                case '\r': b.put((byte) 'r'); break;
                case '\t': b.put((byte) 't'); break;
                default:
                    b.put((byte) 'u').put((byte) '0').put((byte) '0')
                     .put(Bytes.HEX[ch >> 4]).put(Bytes.HEX[ch & 15]);
            }
        }
        put(s, run, s.length - run);
    }

    /** Starts a response: leaves room for the header and returns where it begins. */
    int begin() {
        ensure(HDR);
        int p = b.position();
        b.position(p + HDR);
        return p;
    }

    /** Drops whatever body was written since begin(start). */
    void resetBody(int start) { b.position(start + HDR); }

    /** Writes the header for the body that follows begin(start). */
    void finish(int start, int status, byte[] ctype, boolean close) {
        int bodyStart = start + HDR, end = b.position(), bodyLen = end - bodyStart;
        byte[] h = hdr;
        int n = 0;
        n = Bytes.copy(HTTP, h, n);
        n = Bytes.copy(Http.statusText(status), h, n);
        n = Bytes.copy(CTYPE, h, n);
        n = Bytes.copy(ctype, h, n);
        n = Bytes.copy(CLEN, h, n);
        int i = 20, v = bodyLen;
        do { num[--i] = (byte) ('0' + v % 10); v /= 10; } while (v != 0);
        int digits = 20 - i;
        boolean small = bodyLen <= SMALL;
        if (!small) {  // large body: keep it where it is and pad the header to HDR with spaces (legal OWS)
            for (int pad = HDR - n - digits - 4 - (close ? CLOSE.length : 0); pad > 0; pad--) h[n++] = ' ';
        }
        System.arraycopy(num, i, h, n, digits);
        n += digits;
        if (close) n = Bytes.copy(CLOSE, h, n);
        h[n++] = '\r'; h[n++] = '\n'; h[n++] = '\r'; h[n++] = '\n';
        b.position(start);
        b.put(h, 0, n);
        if (small) {   // small body: move it down against the header; the copy is cheaper than the padding
            ByteBuffer src = b.duplicate();
            src.limit(end).position(bodyStart);
            b.put(src);
        } else {
            b.position(end);
        }
    }

    void clear() {
        if (b.capacity() > (1 << 20)) b = ByteBuffer.allocateDirect(Conn.INIT);
        else b.clear();
    }
}
