package bulkapi;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Routes loaded from api.spec at startup (same grammar as the C++ build-time generator). */
final class Router {
    static final int MAX_SEGS = 8;
    static final String[] METHODS = {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD"};
    static final int M_UNKNOWN = METHODS.length;
    private static final byte[][] METHOD_BYTES = new byte[METHODS.length][];
    static { for (int i = 0; i < METHODS.length; i++) METHOD_BYTES[i] = Bytes.ascii(METHODS[i]); }

    static final class Route {
        int method, maxBody, nparams;
        byte[][] segs;  // null entry = :param
        Handlers.Handler handler;
        String text;
    }

    static Route[] routes;
    private static int[] order;  // literal segments before :param segments

    static int parseMethod(ByteBuffer b, int off, int len) {
        for (int i = 0; i < METHOD_BYTES.length; i++) if (Bytes.eq(b, off, len, METHOD_BYTES[i])) return i;
        return M_UNKNOWN;
    }

    private static int parseSize(String s, String where) {
        Matcher m = Pattern.compile("(\\d+)([kKmMgG]?)").matcher(s);
        if (!m.matches()) throw new IllegalArgumentException(where + ": bad size '" + s + "'");
        long mult = "kK".contains(m.group(2)) && !m.group(2).isEmpty() ? 1L << 10
                  : "mM".contains(m.group(2)) && !m.group(2).isEmpty() ? 1L << 20
                  : m.group(2).isEmpty() ? 1 : 1L << 30;
        long v = Long.parseLong(m.group(1)) * mult;
        if (v > (1L << 30)) throw new IllegalArgumentException(where + ": maxbody above 1g is not supported");
        return (int) v;
    }

    static void load(String path) throws IOException {
        Pattern line = Pattern.compile("([A-Z]+)\\s+(/\\S*)\\s*->\\s*([A-Za-z_]\\w*)((?:\\s+\\w+=\\S+)*)");
        Pattern lit = Pattern.compile("[A-Za-z0-9._~-]+"), param = Pattern.compile(":([A-Za-z_]\\w*)");
        List<Route> rs = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        int ln = 0;
        for (String raw : Files.readAllLines(Paths.get(path))) {
            ln++;
            int hash = raw.indexOf('#');
            String s = (hash < 0 ? raw : raw.substring(0, hash)).trim();
            if (s.isEmpty()) continue;
            String where = path + ":" + ln;
            Matcher m = line.matcher(s);
            if (!m.matches()) throw new IllegalArgumentException(where + ": cannot parse '" + s + "'");
            Route r = new Route();
            r.method = Arrays.asList(METHODS).indexOf(m.group(1));
            if (r.method < 0) throw new IllegalArgumentException(where + ": unknown method " + m.group(1));
            String p = m.group(2);
            String[] segs = p.equals("/") ? new String[0] : p.replaceAll("^/+|/+$", "").split("/");
            if (segs.length > MAX_SEGS) throw new IllegalArgumentException(where + ": more than " + MAX_SEGS + " segments");
            r.segs = new byte[segs.length][];
            StringBuilder shape = new StringBuilder(m.group(1));
            for (int i = 0; i < segs.length; i++) {
                if (param.matcher(segs[i]).matches()) { r.nparams++; shape.append("/*"); }
                else if (lit.matcher(segs[i]).matches()) { r.segs[i] = Bytes.ascii(segs[i]); shape.append('/').append(segs[i]); }
                else throw new IllegalArgumentException(where + ": bad segment '" + segs[i] + "'");
            }
            if (!seen.add(shape.toString())) throw new IllegalArgumentException(where + ": duplicate route " + m.group(1) + " " + p);
            r.maxBody = 1 << 20;
            for (String kv : m.group(4).trim().split("\\s+")) {
                if (kv.isEmpty()) continue;
                String[] a = kv.split("=", 2);
                if (!a[0].equals("maxbody")) throw new IllegalArgumentException(where + ": unknown option '" + a[0] + "'");
                r.maxBody = parseSize(a[1], where);
            }
            r.handler = Handlers.byName(m.group(3));
            if (r.handler == null) throw new IllegalArgumentException(where + ": no handler named '" + m.group(3) + "' in Handlers.java");
            r.text = m.group(1) + " " + p;
            rs.add(r);
        }
        if (rs.isEmpty()) throw new IllegalArgumentException(path + ": no routes");
        routes = rs.toArray(new Route[0]);
        Integer[] idx = new Integer[routes.length];
        for (int i = 0; i < idx.length; i++) idx[i] = i;
        Arrays.sort(idx, (a, b) -> {  // stable; compares the per-segment "is a param" flags
            byte[][] x = routes[a].segs, y = routes[b].segs;
            for (int i = 0; i < Math.min(x.length, y.length); i++) {
                int c = Boolean.compare(x[i] == null, y[i] == null);
                if (c != 0) return c;
            }
            return Integer.compare(x.length, y.length);
        });
        order = new int[idx.length];
        for (int i = 0; i < idx.length; i++) order[i] = idx[i];
    }

    /** Returns the route index, -1 = no such path, -2 = path exists but not for this method. */
    static int match(Worker w, ByteBuffer b, int off, int end, int method) {
        int[] so = w.segOff, sl = w.segLen;
        int n = 0, i = (off < end && b.get(off) == '/') ? off + 1 : off;
        while (i < end) {
            int j = Bytes.indexOf(b, i, end, '/');
            if (j < 0) j = end;
            if (n == MAX_SEGS) return -1;
            so[n] = i;
            sl[n++] = j - i;
            i = j + 1;
        }
        boolean pathHit = false;
        outer:
        for (int idx : order) {
            Route r = routes[idx];
            byte[][] segs = r.segs;
            if (segs.length != n) continue;
            for (int k = 0; k < n; k++) {
                if (segs[k] == null ? sl[k] == 0 : !Bytes.eq(b, so[k], sl[k], segs[k])) continue outer;
            }
            pathHit = true;
            if (r.method != method) continue;
            for (int k = 0, p = 0; k < n; k++) if (segs[k] == null) { w.pOff[p] = so[k]; w.pLen[p++] = sl[k]; }
            return idx;
        }
        return pathHit ? -2 : -1;
    }
}
