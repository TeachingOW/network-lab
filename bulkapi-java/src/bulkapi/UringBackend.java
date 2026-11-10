package bulkapi;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;

/**
 * io_uring backend. liburing sets the ring up; after that Java writes submissions and reads
 * completions directly in the rings' shared memory through direct ByteBuffers, so the only JNI
 * call on the hot path is io_uring_enter, once per loop turn. Multishot accept, one recv or send
 * in flight per connection (same algorithm as the C++ uring backend).
 */
final class UringBackend {
    private static final VarHandle INT = MethodHandles.byteBufferViewVarHandle(int[].class, ByteOrder.nativeOrder());
    private static final int OP_ACCEPT = 0, OP_RECV = 1, OP_SEND = 2, OP_BITS = 3;
    private static final int[] K = Native.constants();

    private final Worker w;
    private final int port;
    private int ringFd, sqMask, sqEntries, cqMask, localTail, pending;
    private ByteBuffer sqHead, sqTail, sqes, cqHead, cqTail, cqes;
    private final ArrayList<Conn> conns = new ArrayList<>();   // index = connection id
    private final ArrayList<Conn> free = new ArrayList<>();

    UringBackend(Worker w, int port) { this.w = w; this.port = port; }

    private static ByteBuffer map(long addr, long len) { return Native.wrap(addr, len).order(ByteOrder.nativeOrder()); }

    private void fatal(String what, int err) {
        System.err.println("worker " + w.id + ": " + what + " failed (errno " + -err + ")");
        System.exit(1);
    }

    /** Publishes queued SQEs; optionally waits for at least one completion. */
    private void enter(boolean wait) {
        INT.setRelease(sqTail, 0, localTail);
        for (;;) {
            int r = Native.enter(ringFd, pending, wait ? 1 : 0, wait ? K[Native.K_ENTER_GETEVENTS] : 0);
            if (r < 0) {
                if (r == -K[Native.K_EINTR]) continue;
                if (r == -K[Native.K_EAGAIN] || r == -K[Native.K_EBUSY]) return;  // CQ pressure: caller drains
                fatal("io_uring_enter", r);
            }
            pending -= r;
            if (pending > 0 && !wait) continue;
            return;
        }
    }

    /** Offset of a zeroed SQE in the submission array. */
    private int sqe() {
        while (localTail - (int) INT.getAcquire(sqHead, 0) >= sqEntries) enter(false);  // SQ full: flush
        int off = (localTail & sqMask) * K[Native.K_SQE_SIZE];
        localTail++;
        pending++;
        for (int i = 0; i < K[Native.K_SQE_SIZE]; i += 8) sqes.putLong(off + i, 0L);
        return off;
    }

    private void sqAccept(int lfd) {
        int e = sqe();
        sqes.put(e + K[Native.K_SQE_OPCODE], (byte) K[Native.K_OP_ACCEPT]);
        sqes.putInt(e + K[Native.K_SQE_FD], lfd);
        sqes.putInt(e + K[Native.K_SQE_OPFLAGS], K[Native.K_SOCK_CLOEXEC]);
        sqes.putShort(e + K[Native.K_SQE_IOPRIO], (short) K[Native.K_ACCEPT_MULTISHOT]);
        sqes.putLong(e + K[Native.K_SQE_USER_DATA], OP_ACCEPT);
    }

    private void sqRecv(Conn c) {
        c.ensureRoom();
        int e = sqe();
        sqes.put(e + K[Native.K_SQE_OPCODE], (byte) K[Native.K_OP_RECV]);
        sqes.putInt(e + K[Native.K_SQE_FD], c.fd);
        sqes.putLong(e + K[Native.K_SQE_ADDR], c.inAddress() + c.inLen);
        sqes.putInt(e + K[Native.K_SQE_LEN], c.in.capacity() - c.inLen);
        sqes.putLong(e + K[Native.K_SQE_USER_DATA], (long) c.id << OP_BITS | OP_RECV);
    }

    private void sqSend(Conn c) {
        int e = sqe();
        sqes.put(e + K[Native.K_SQE_OPCODE], (byte) K[Native.K_OP_SEND]);
        sqes.putInt(e + K[Native.K_SQE_FD], c.fd);
        sqes.putLong(e + K[Native.K_SQE_ADDR], c.outAddress() + c.outOff);
        sqes.putInt(e + K[Native.K_SQE_LEN], c.out.b.position() - c.outOff);
        sqes.putInt(e + K[Native.K_SQE_OPFLAGS], K[Native.K_MSG_NOSIGNAL]);
        sqes.putLong(e + K[Native.K_SQE_USER_DATA], (long) c.id << OP_BITS | OP_SEND);
    }

    private Conn connNew(int fd) {
        Conn c;
        if (!free.isEmpty()) c = free.remove(free.size() - 1);
        else { c = new Conn(); c.id = conns.size(); conns.add(c); }
        c.reset();
        c.fd = fd;
        return c;
    }

    private void connFree(Conn c) {
        Native.close(c.fd);
        c.fd = -1;
        free.add(c);
    }

    /** After a recv or send finished: decide the connection's next operation. */
    private void nextOp(Conn c) {
        if (c.outOff < c.out.b.position()) { sqSend(c); return; }
        c.out.clear();
        c.outOff = 0;
        if (c.wantClose) { connFree(c); return; }
        sqRecv(c);
    }

    void run() {
        long[] r = Native.ringSetup(4096, 32768);
        if (r.length == 1) fatal("io_uring setup (needs Linux 6.1+)", (int) r[0]);
        ringFd = (int) r[Native.R_FD];
        sqMask = (int) r[Native.R_SQ_MASK];
        sqEntries = (int) r[Native.R_SQ_ENTRIES];
        cqMask = (int) r[Native.R_CQ_MASK];
        sqHead = map(r[Native.R_SQ_HEAD], 4);
        sqTail = map(r[Native.R_SQ_TAIL], 4);
        cqHead = map(r[Native.R_CQ_HEAD], 4);
        cqTail = map(r[Native.R_CQ_TAIL], 4);
        sqes = map(r[Native.R_SQES], (long) sqEntries * K[Native.K_SQE_SIZE]);
        cqes = map(r[Native.R_CQES], r[Native.R_CQ_ENTRIES] * K[Native.K_CQE_SIZE]);
        localTail = sqTail.getInt(0);
        int lfd = Native.listen(port, 4096);
        if (lfd < 0) fatal("listen on port " + port, lfd);
        System.err.println("worker " + w.id + ": io_uring (liburing " + Native.liburingVersion() + ", single-issuer+defer-taskrun)");

        final int cqeSize = K[Native.K_CQE_SIZE], oUd = K[Native.K_CQE_USER_DATA], oRes = K[Native.K_CQE_RES],
                oFlags = K[Native.K_CQE_FLAGS], more = K[Native.K_CQE_F_MORE], eintr = -K[Native.K_EINTR], eagain = -K[Native.K_EAGAIN];
        sqAccept(lfd);
        for (;;) {
            enter(true);
            int head = cqHead.getInt(0);
            int tail = (int) INT.getAcquire(cqTail, 0);
            while (head != tail) {
                int off = (head & cqMask) * cqeSize;
                long ud = cqes.getLong(off + oUd);
                int res = cqes.getInt(off + oRes), flags = cqes.getInt(off + oFlags);
                head++;
                int op = (int) ud & ((1 << OP_BITS) - 1);
                if (op == OP_ACCEPT) {
                    if (res >= 0) {
                        Conn nc = connNew(res);
                        w.conns++;
                        sqRecv(nc);
                    }
                    if ((flags & more) == 0) sqAccept(lfd);
                    continue;
                }
                Conn c = conns.get((int) (ud >>> OP_BITS));
                if (op == OP_RECV) {
                    if (res == eintr || res == eagain) { sqRecv(c); continue; }
                    if (res <= 0) { connFree(c); continue; }
                    c.inLen += res;
                    Http.process(c, w);
                    nextOp(c);
                } else {  // OP_SEND
                    if (res < 0) { connFree(c); continue; }
                    c.outOff += res;
                    nextOp(c);
                }
            }
            INT.setRelease(cqHead, 0, head);
        }
    }
}
