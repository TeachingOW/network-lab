package bulkapi;

import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.SocketChannel;

/** One connection: a direct input buffer the kernel reads into, and a direct output buffer. */
final class Conn {
    static final int INIT = 16 * 1024;
    static final int BIG = 256 * 1024;  // buffers above this are dropped when idle

    ByteBuffer in = ByteBuffer.allocateDirect(INIT);
    int inLen, needTotal;
    final Out out = new Out();
    int outOff;
    boolean wantClose, sent100;

    // backend state
    SocketChannel ch;        // epoll, sockets
    SelectionKey key;        // epoll
    boolean writing;         // epoll: waiting for the socket to become writable
    int fd = -1, id;         // uring
    private long inAddr, outAddr;
    private ByteBuffer inAddrOf, outAddrOf;

    void reset() {
        inLen = needTotal = outOff = 0;
        wantClose = sent100 = writing = false;
        out.clear();
        if (in.capacity() > BIG) in = ByteBuffer.allocateDirect(INIT);
    }

    /** Makes room for the next read: the rest of a known body, or at least 4 KB. */
    void ensureRoom() {
        int want = Math.max(needTotal, inLen + 4096);
        if (want <= in.capacity()) return;
        int ncap = needTotal >= inLen + 4096 ? needTotal : Math.max(want, in.capacity() * 2);
        ByteBuffer nb = ByteBuffer.allocateDirect(ncap);
        in.limit(inLen).position(0);
        nb.put(in);
        in = nb;
    }

    /** Drops the first pos bytes of input. */
    void consume(int pos) {
        in.limit(inLen).position(pos);
        in.compact();
        inLen -= pos;
    }

    long inAddress() {
        if (inAddrOf != in) { inAddr = Native.address(in); inAddrOf = in; }
        return inAddr;
    }
    long outAddress() {
        if (outAddrOf != out.b) { outAddr = Native.address(out.b); outAddrOf = out.b; }
        return outAddr;
    }
}
