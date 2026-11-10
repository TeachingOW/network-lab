package bulkapi;

/**
 * Sharded in-memory map from unsigned 64-bit id to bytes. Each shard is an open-addressing
 * table over a long[] and a byte[][]: no boxing and no node per entry. Callers synchronise on
 * the shard. Stored arrays are never modified, so a value may be read after the lock is released.
 */
final class Store {
    static final int SHARDS = 256;
    private static final long MIX = 0x9E3779B97F4A7C15L;
    static final Shard[] shards = new Shard[SHARDS];
    static { for (int i = 0; i < SHARDS; i++) shards[i] = new Shard(); }

    static Shard shardFor(long id) { return shards[(int) ((id * MIX) >>> 56)]; }

    static final class Shard {
        private long[] keys = new long[16];
        private byte[][] vals = new byte[16][];  // null = empty slot
        int size;

        private int slot(long k, int mask) { return (int) ((k * MIX) >>> 20) & mask; }

        byte[] get(long k) {
            int m = keys.length - 1;
            for (int i = slot(k, m); vals[i] != null; i = (i + 1) & m) if (keys[i] == k) return vals[i];
            return null;
        }

        void put(long k, byte[] v) {
            int m = keys.length - 1, i = slot(k, m);
            for (; vals[i] != null; i = (i + 1) & m) if (keys[i] == k) { vals[i] = v; return; }
            if ((size + 1) * 4 > keys.length * 3) { grow(); put(k, v); return; }
            keys[i] = k;
            vals[i] = v;
            size++;
        }

        boolean remove(long k) {
            int m = keys.length - 1, i = slot(k, m);
            for (; ; i = (i + 1) & m) {
                if (vals[i] == null) return false;
                if (keys[i] == k) break;
            }
            // backward-shift deletion: pull later entries of the probe run into the hole
            for (int j = i; ; ) {
                j = (j + 1) & m;
                if (vals[j] == null) break;
                int h = slot(keys[j], m);
                boolean movable = j > i ? (h <= i || h > j) : (h <= i && h > j);
                if (movable) { keys[i] = keys[j]; vals[i] = vals[j]; i = j; }
            }
            vals[i] = null;
            size--;
            return true;
        }

        private void grow() {
            long[] ok = keys;
            byte[][] ov = vals;
            keys = new long[ok.length * 2];
            vals = new byte[ok.length * 2][];
            int m = keys.length - 1;
            for (int j = 0; j < ok.length; j++) {
                if (ov[j] == null) continue;
                int i = slot(ok[j], m);
                while (vals[i] != null) i = (i + 1) & m;
                keys[i] = ok[j];
                vals[i] = ov[j];
            }
        }
    }
}
