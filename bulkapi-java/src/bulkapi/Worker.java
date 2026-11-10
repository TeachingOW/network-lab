package bulkapi;

import java.nio.ByteBuffer;

/** Per-thread state: counters and the scratch request the handlers read. */
final class Worker {
    final int id;
    long req, conns;  // read without synchronisation by /stats; slightly stale values are fine

    // current request
    ByteBuffer in;
    int method, bodyOff, bodyLen;
    final int[] pOff = new int[Router.MAX_SEGS], pLen = new int[Router.MAX_SEGS];
    int status;
    byte[] ctype;

    // scratch
    final int[] segOff = new int[Router.MAX_SEGS], segLen = new int[Router.MAX_SEGS];
    long num;              // result of Json.parseU64
    int valOff, valLen;    // result of Json.parseItem
    boolean esc;
    byte[] tmp = new byte[256];

    Worker(int id) { this.id = id; }
}
