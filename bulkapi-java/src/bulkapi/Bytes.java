package bulkapi;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

/** Byte-level helpers over ByteBuffers (absolute access; positions are never relied on). */
final class Bytes {
    static final byte[] HEX = ascii("0123456789abcdef");

    static byte[] ascii(String s) { return s.getBytes(StandardCharsets.ISO_8859_1); }

    static int copy(byte[] src, byte[] dst, int at) {
        System.arraycopy(src, 0, dst, at, src.length);
        return at + src.length;
    }

    static int indexOf(ByteBuffer b, int from, int to, int ch) {
        for (int i = from; i < to; i++) if (b.get(i) == ch) return i;
        return -1;
    }

    /** First index of pat in [from, to), or -1. */
    static int indexOf(ByteBuffer b, int from, int to, byte[] pat) {
        int last = to - pat.length;
        outer:
        for (int i = from; i <= last; i++) {
            if (b.get(i) != pat[0]) continue;
            for (int j = 1; j < pat.length; j++) if (b.get(i + j) != pat[j]) continue outer;
            return i;
        }
        return -1;
    }

    static boolean eq(ByteBuffer b, int off, int len, byte[] s) {
        if (len != s.length) return false;
        for (int i = 0; i < len; i++) if (b.get(off + i) != s[i]) return false;
        return true;
    }

    /** Case-insensitive prefix compare against an all-lowercase pattern. */
    static boolean startsWithIgnoreCase(ByteBuffer b, int off, int len, byte[] lower) {
        if (len < lower.length) return false;
        for (int i = 0; i < lower.length; i++) {
            int x = b.get(off + i);
            if (x >= 'A' && x <= 'Z') x += 32;
            if (x != lower[i]) return false;
        }
        return true;
    }

    static byte[] copyOut(ByteBuffer b, int off, int len) {
        byte[] a = new byte[len];
        b.limit(b.capacity()).position(off);
        b.get(a);
        return a;
    }
}
