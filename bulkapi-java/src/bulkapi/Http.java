package bulkapi;

import java.nio.ByteBuffer;

/** HTTP/1.1 request framing, routing and response writing. */
final class Http {
    private static final int MAX_HEADER = 16 * 1024;
    private static final byte[] CRLFCRLF = Bytes.ascii("\r\n\r\n"), HTTP1 = Bytes.ascii("HTTP/1.");
    private static final byte[] CONTENT_LENGTH = Bytes.ascii("content-length"), CONNECTION = Bytes.ascii("connection"),
            TRANSFER_ENCODING = Bytes.ascii("transfer-encoding"), EXPECT = Bytes.ascii("expect"),
            CLOSE = Bytes.ascii("close"), KEEP_ALIVE = Bytes.ascii("keep-alive"), CONTINUE = Bytes.ascii("100-continue");
    private static final byte[] R_CONTINUE = Bytes.ascii("HTTP/1.1 100 Continue\r\n\r\n");
    private static final byte[] E_HEADERS = Bytes.ascii("headers too large"), E_LINE = Bytes.ascii("bad request line"),
            E_CL = Bytes.ascii("bad content-length"), E_TE = Bytes.ascii("chunked bodies unsupported; send Content-Length"),
            E_405 = Bytes.ascii("method not allowed"), E_404 = Bytes.ascii("not found"), E_413 = Bytes.ascii("body too large"),
            E_500 = Bytes.ascii("internal error");
    private static final byte[] S200 = Bytes.ascii("200 OK"), S201 = Bytes.ascii("201 Created"), S204 = Bytes.ascii("204 No Content"),
            S400 = Bytes.ascii("400 Bad Request"), S404 = Bytes.ascii("404 Not Found"), S405 = Bytes.ascii("405 Method Not Allowed"),
            S413 = Bytes.ascii("413 Payload Too Large"), S431 = Bytes.ascii("431 Request Header Fields Too Large"),
            S501 = Bytes.ascii("501 Not Implemented"), S500 = Bytes.ascii("500 Internal Server Error");

    static byte[] statusText(int s) {
        switch (s) {
            case 200: return S200;
            case 201: return S201;
            case 204: return S204;
            case 400: return S400;
            case 404: return S404;
            case 405: return S405;
            case 413: return S413;
            case 431: return S431;
            case 501: return S501;
            default: return S500;
        }
    }

    private static void fail(Conn c, Worker w, int status, byte[] msg) {
        int start = c.out.begin();
        Handlers.err(w, c.out, status, msg);
        c.out.finish(start, status, Handlers.JSON, true);
        c.wantClose = true;
    }

    /**
     * Handles every complete request buffered in c.in and appends the responses to c.out
     * (pipelining-aware). Leaves a partial request in place and records in c.needTotal how
     * many bytes it needs.
     */
    static void process(Conn c, Worker w) {
        if (c.needTotal > 0 && c.inLen < c.needTotal) return;  // a known body is still arriving
        final ByteBuffer in = c.in;
        final Out out = c.out;
        final int inLen = c.inLen;
        int pos = 0;
        long nreq = 0;
        c.needTotal = 0;
        w.in = in;
        while (pos < inLen && !c.wantClose) {
            final int base = pos, avail = inLen - pos;
            int he = Bytes.indexOf(in, base, inLen, CRLFCRLF);
            if (he < 0) {
                if (avail > MAX_HEADER) fail(c, w, 431, E_HEADERS);
                break;
            }
            int hlen = he - base + 4;
            int le = Bytes.indexOf(in, base, base + hlen, '\r');  // end of the request line
            int sp1 = Bytes.indexOf(in, base, le, ' ');
            int sp2 = sp1 < 0 ? -1 : Bytes.indexOf(in, sp1 + 1, le, ' ');
            if (sp2 < 0 || le - sp2 - 1 != 8 || !Bytes.eq(in, sp2 + 1, 7, HTTP1)) {
                fail(c, w, 400, E_LINE);
                break;
            }
            boolean keepAlive = in.get(sp2 + 8) == '1';
            long cl = 0;
            boolean hasTe = false, expect100 = false, bad = false;
            for (int p = le + 2, hend = he + 2; p < hend; ) {
                int nl = Bytes.indexOf(in, p, hend, '\n');
                if (nl < 0) { bad = true; break; }
                int e = in.get(nl - 1) == '\r' ? nl - 1 : nl;
                int colon = Bytes.indexOf(in, p, e, ':');
                if (colon >= 0) {
                    int nlen = colon - p, v = colon + 1;
                    while (v < e && (in.get(v) == ' ' || in.get(v) == '\t')) v++;
                    while (e > v && (in.get(e - 1) == ' ' || in.get(e - 1) == '\t')) e--;
                    int vlen = e - v;
                    if (nlen == 14 && Bytes.startsWithIgnoreCase(in, p, nlen, CONTENT_LENGTH)) {
                        long x = 0;
                        if (vlen == 0 || vlen > 12) bad = true;
                        for (int i = 0; i < vlen && !bad; i++) {
                            int d = in.get(v + i) - '0';
                            if (d < 0 || d > 9) bad = true; else x = x * 10 + d;
                        }
                        cl = x;
                    } else if (nlen == 10 && Bytes.startsWithIgnoreCase(in, p, nlen, CONNECTION)) {
                        if (Bytes.startsWithIgnoreCase(in, v, vlen, CLOSE)) keepAlive = false;
                        else if (Bytes.startsWithIgnoreCase(in, v, vlen, KEEP_ALIVE)) keepAlive = true;
                    } else if (nlen == 17 && Bytes.startsWithIgnoreCase(in, p, nlen, TRANSFER_ENCODING)) {
                        hasTe = true;
                    } else if (nlen == 6 && Bytes.startsWithIgnoreCase(in, p, nlen, EXPECT)) {
                        expect100 = Bytes.startsWithIgnoreCase(in, v, vlen, CONTINUE);
                    }
                }
                p = nl + 1;
            }
            if (bad) { fail(c, w, 400, E_CL); break; }
            if (hasTe) { fail(c, w, 501, E_TE); break; }

            int pathEnd = Bytes.indexOf(in, sp1 + 1, sp2, '?');
            if (pathEnd < 0) pathEnd = sp2;
            w.method = Router.parseMethod(in, base, sp1 - base);
            int ridx = Router.match(w, in, sp1 + 1, pathEnd, w.method);
            if (ridx < 0) {
                boolean closeAfter = !keepAlive || cl > 0;  // don't buffer a body we won't use
                int start = out.begin();
                Handlers.err(w, out, ridx == -2 ? 405 : 404, ridx == -2 ? E_405 : E_404);
                out.finish(start, w.status, Handlers.JSON, closeAfter);
                if (closeAfter) c.wantClose = true;
                pos += hlen;
                nreq++;
                continue;
            }
            Router.Route route = Router.routes[ridx];
            if (cl > route.maxBody) { fail(c, w, 413, E_413); break; }
            int total = hlen + (int) cl;
            if (avail < total) {
                c.needTotal = total;
                if (expect100 && !c.sent100) { out.put(R_CONTINUE); c.sent100 = true; }
                break;
            }
            c.sent100 = false;

            w.bodyOff = base + hlen;
            w.bodyLen = (int) cl;
            w.status = 200;
            w.ctype = Handlers.JSON;
            int start = out.begin();
            try {
                route.handler.handle(w, out);
            } catch (RuntimeException ex) {
                out.resetBody(start);
                w.ctype = Handlers.JSON;
                Handlers.err(w, out, 500, E_500);
            }
            out.finish(start, w.status, w.ctype, !keepAlive);
            if (!keepAlive) c.wantClose = true;
            pos += total;
            nreq++;
        }
        w.req += nreq;
        if (c.wantClose) { c.inLen = 0; return; }
        if (pos > 0) {
            c.consume(pos);
            if (c.needTotal > 0) c.needTotal -= pos;
        }
        if (c.inLen == 0 && c.in.capacity() > Conn.BIG) c.in = ByteBuffer.allocateDirect(Conn.INIT);
    }
}
