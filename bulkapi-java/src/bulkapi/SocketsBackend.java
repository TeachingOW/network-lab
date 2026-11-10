package bulkapi;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.StandardSocketOptions;
import java.nio.ByteBuffer;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Plain blocking sockets: one thread per connection, taken from a cached pool so threads are
 * reused across connections. Each thread blocks in read(), handles every request that arrived,
 * and writes the replies in one blocking write. Buffers are direct and pooled with their
 * connection object. The simplest of the three; it costs a thread per open connection.
 */
final class SocketsBackend {
    private final int port;
    private final ConcurrentLinkedQueue<Conn> pool = new ConcurrentLinkedQueue<>();
    private final ThreadLocal<Worker> worker = ThreadLocal.withInitial(Server::newWorker);

    SocketsBackend(int port) { this.port = port; }

    private void serve(SocketChannel ch) {
        Worker w = worker.get();
        Conn c = pool.poll();
        if (c == null) c = new Conn();
        c.reset();
        w.conns++;
        try {
            ch.setOption(StandardSocketOptions.TCP_NODELAY, true);
            while (!c.wantClose) {
                c.ensureRoom();
                ByteBuffer in = c.in;
                in.limit(in.capacity()).position(c.inLen);
                int n = ch.read(in);
                if (n < 0) break;
                c.inLen += n;
                Http.process(c, w);
                ByteBuffer b = c.out.b;
                if (b.position() == 0) continue;
                b.flip();
                while (b.hasRemaining()) ch.write(b);
                c.out.clear();
            }
        } catch (IOException ignored) {
        } finally {
            try { ch.close(); } catch (IOException ignored) { }
            pool.add(c);
        }
    }

    void run() throws IOException {
        ServerSocketChannel listener = ServerSocketChannel.open();
        listener.setOption(StandardSocketOptions.SO_REUSEADDR, true);
        listener.bind(new InetSocketAddress(port), 4096);
        ExecutorService threads = Executors.newCachedThreadPool(r -> {
            Thread t = new Thread(r, "conn");
            t.setDaemon(true);
            return t;
        });
        System.err.println("sockets: blocking, one thread per connection");
        for (;;) {
            SocketChannel ch = listener.accept();
            threads.execute(() -> serve(ch));
        }
    }
}
