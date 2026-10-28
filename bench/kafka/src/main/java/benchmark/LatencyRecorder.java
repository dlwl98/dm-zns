package benchmark;

import java.util.Arrays;
import java.util.concurrent.atomic.AtomicInteger;

/** Latencies go into a preallocated array, so recording does not allocate. */
public class LatencyRecorder {
    private final long[] latencies;
    private final AtomicInteger count = new AtomicInteger(0);
    private final int capacity;

    public LatencyRecorder(int capacity) {
        this.capacity = capacity;
        this.latencies = new long[capacity];
    }

    public void record(long latencyMs) {
        int idx = count.getAndIncrement();
        if (idx < capacity) {
            latencies[idx] = latencyMs;
        }
    }

    public int getCount() {
        return Math.min(count.get(), capacity);
    }

    public Stats calculate() {
        int n = getCount();
        if (n == 0) return new Stats(0, 0, 0, 0, 0, 0);

        long[] sorted = Arrays.copyOf(latencies, n);
        Arrays.sort(sorted);

        long sum = 0;
        for (int i = 0; i < n; i++) sum += sorted[i];

        return new Stats(
            n,
            (double) sum / n,
            sorted[percentileIndex(n, 50)],
            sorted[percentileIndex(n, 90)],
            sorted[percentileIndex(n, 99)],
            sorted[n - 1]
        );
    }

    public Stats calculateWindow(int startIdx) {
        int endIdx = getCount();
        int n = endIdx - startIdx;
        if (n <= 0) return new Stats(0, 0, 0, 0, 0, 0);

        long[] window = new long[n];
        System.arraycopy(latencies, startIdx, window, 0, n);
        Arrays.sort(window);

        long sum = 0;
        for (int i = 0; i < n; i++) sum += window[i];

        return new Stats(
            n,
            (double) sum / n,
            window[percentileIndex(n, 50)],
            window[percentileIndex(n, 90)],
            window[percentileIndex(n, 99)],
            window[n - 1]
        );
    }

    private static int percentileIndex(int n, int percentile) {
        return Math.min((int) Math.ceil(n * percentile / 100.0) - 1, n - 1);
    }

    public static class Stats {
        public final int count;
        public final double avg;
        public final long p50;
        public final long p90;
        public final long p99;
        public final long max;

        public Stats(int count, double avg, long p50, long p90, long p99, long max) {
            this.count = count;
            this.avg = avg;
            this.p50 = p50;
            this.p90 = p90;
            this.p99 = p99;
            this.max = max;
        }

        @Override
        public String toString() {
            return String.format("count=%d, avg=%.1f ms, p50=%d ms, p90=%d ms, p99=%d ms, max=%d ms",
                count, avg, p50, p90, p99, max);
        }

        public String toCsv() {
            return String.format("%d,%.1f,%d,%d,%d,%d", count, avg, p50, p90, p99, max);
        }
    }
}
