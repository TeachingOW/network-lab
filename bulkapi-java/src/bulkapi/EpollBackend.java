package bulkapi;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.StandardSocketOptions;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.util.ArrayList;

/**
 * epoll backend: java.nio's Selector, which is epoll on Linux. One selector and one
 * SO_REUSEPORT listener per worker thread, direct buffers so reads and writes go straight
 * between the socket and the buffer, and the allocation-free select(Consumer) form.
 * Write interest is only registered when a send would block.
 */
final class EpollBackend {
    private final Worker w;
    private final int port;
    private Selector sel;
    private ServerSocketChannel listener;
    private final ArrayList<Conn> pool = new ArrayList<>();

    EpollBackend(Worker w, int port) { this.w = w; this.port = port; }

    private void drop(Conn c) {
        c.key.cancel();
        try { c.ch.close(); } catch (IOException ignored) { }
        c.ch = null;
        c.key = null;
        pool.add(c);
    }

    /** Writes pending output. Returns true once everything has been sent. */
    private static boolean flush(Conn c) throws IOException {
        ByteBuffer b = c.out.b;
        int end = b.position();
        b.limit(end).position(c.outOff);
        try {
            while (b.hasRemaining()) if (c.ch.write(b) == 0) break;
            c.outOff = b.position();
        } finally {
            b.limit(b.capacity()).position(end);
        }
        if (c.outOff < end) return false;
        c.out.clear();
        c.outOff = 0;
        return true;
    }

    private void accept() throws IOException {
        for (SocketChannel ch; (ch = listener.accept()) != null; ) {
            ch.configureBlocking(false);
            ch.setOption(StandardSocketOptions.TCP_NODELAY, true);
            Conn c = pool.isEmpty() ? new Conn() : pool.remove(pool.size() - 1);
            c.reset();
            c.ch = ch;
            c.key = ch.register(sel, SelectionKey.OP_READ, c);
            w.conns++;
        }
    }

    private void onKey(SelectionKey key) {
        Conn c = (Conn) key.attachment();
        try {
            if (c == null) { accept(); return; }
            if (c.writing) {
                if (!flush(c)) return;
                if (c.wantClose) { drop(c); return; }
                c.writing = false;
                key.interestOps(SelectionKey.OP_READ);
                return;
            }
            c.ensureRoom();
            ByteBuffer in = c.in;
            in.limit(in.capacity()).position(c.inLen);
            int n = c.ch.read(in);
            if (n < 0) { drop(c); return; }
            if (n == 0) return;
            c.inLen += n;
            Http.process(c, w);
            if (c.out.b.position() == 0) { if (c.wantClose) drop(c); return; }
            if (flush(c)) { if (c.wantClose) drop(c); return; }
            c.writing = true;  // socket buffer full: stop reading until it drains
            key.interestOps(SelectionKey.OP_WRITE);
        } catch (IOException | java.nio.channels.CancelledKeyException e) {
            if (c != null && c.key != null) drop(c);
        }
    }

    void run() throws IOException {
        sel = Selector.open();
        listener = ServerSocketChannel.open();
        listener.setOption(StandardSocketOptions.SO_REUSEADDR, true);
        listener.setOption(StandardSocketOptions.SO_REUSEPORT, true);
        listener.bind(new InetSocketAddress(port), 4096);
        listener.configureBlocking(false);
        listener.register(sel, SelectionKey.OP_ACCEPT, null);
        System.err.println("worker " + w.id + ": epoll (java.nio selector)");
        for (;;) sel.select(this::onKey);
    }
}
