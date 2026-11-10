package bulkapi;

import java.io.File;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * bulkapi in Java: the same API and api.spec as the C++ server, with three I/O backends.
 *
 *   java -cp build/classes -Djava.library.path=build bulkapi.Server
 *        [-p port] [-t threads] [--backend uring|epoll|sockets] [--spec path/to/api.spec]
 */
public final class Server {
    static final CopyOnWriteArrayList<Worker> workers = new CopyOnWriteArrayList<>();

    static Worker newWorker() {
        Worker w = new Worker(workers.size());
        workers.add(w);
        return w;
    }

    private static void usage() {
        System.err.println("usage: bulkapi.Server [-p port] [-t threads] [--backend uring|epoll|sockets] [--spec api.spec]");
        System.exit(2);
    }

    public static void main(String[] args) throws Exception {
        int port = 8080, threads = Runtime.getRuntime().availableProcessors();
        String backend = "uring", spec = null;
        for (int i = 0; i < args.length; i++) {
            String a = args[i];
            if (i + 1 >= args.length) usage();
            String v = args[++i];
            if (a.equals("-p") || a.equals("--port")) port = Integer.parseInt(v);
            else if (a.equals("-t") || a.equals("--threads")) threads = Integer.parseInt(v);
            else if (a.equals("--backend")) backend = v;
            else if (a.equals("--spec")) spec = v;
            else usage();
        }
        if (!backend.equals("uring") && !backend.equals("epoll") && !backend.equals("sockets")) usage();
        if (spec == null) spec = new File("api.spec").exists() ? "api.spec" : "../bulkapi/api.spec";
        try {
            Router.load(spec);
        } catch (IllegalArgumentException e) {
            System.err.println("api.spec: " + e.getMessage());
            System.exit(1);
        }
        threads = Math.max(1, Math.min(threads, 256));
        final int p = port;
        if (backend.equals("sockets")) {
            System.err.println("bulkapi-java listening on :" + port + " (" + Router.routes.length + " routes)");
            new SocketsBackend(p).run();
            return;
        }
        System.err.println("bulkapi-java listening on :" + port + " with " + threads + " workers (" + Router.routes.length + " routes)");
        final boolean uring = backend.equals("uring");
        Thread[] ts = new Thread[threads];
        for (int i = 0; i < threads; i++) {
            Worker w = newWorker();
            ts[i] = new Thread(() -> {
                try {
                    if (uring) new UringBackend(w, p).run();
                    else new EpollBackend(w, p).run();
                } catch (Throwable e) {
                    e.printStackTrace();
                    System.exit(1);
                }
            }, "worker-" + i);
            ts[i].start();
        }
        for (Thread t : ts) t.join();
    }
}
