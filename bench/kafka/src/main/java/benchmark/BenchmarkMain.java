package benchmark;

import java.io.*;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

public class BenchmarkMain {

    public static void main(String[] args) throws Exception {
        String bootstrapServers = getArg(args, "--bootstrap-server", "localhost:9092");
        String topic = getArg(args, "--topic", "perf-test");
        String mode = getArg(args, "--mode", "producer"); // "producer" or "both"
        int threads = Integer.parseInt(getArg(args, "--threads", "4"));
        int opsPerSec = Integer.parseInt(getArg(args, "--ops", "5000"));
        int recordSize = Integer.parseInt(getArg(args, "--record-size", "1024"));
        int durationSec = Integer.parseInt(getArg(args, "--duration", "300"));
        String outputDir = getArg(args, "--output-dir", ".");
        int reportIntervalSec = Integer.parseInt(getArg(args, "--report-interval", "5"));

        long durationMs = durationSec * 1000L;
        int maxRecords = opsPerSec * durationSec * 2;

        System.out.println("=== Kafka Benchmark ===");
        System.out.printf("Mode: %s | Threads: %d | Target OP/s: %d%n", mode, threads, opsPerSec);
        System.out.printf("Record Size: %d bytes | Duration: %d sec%n", recordSize, durationSec);
        System.out.printf("Topic: %s | Bootstrap: %s%n", topic, bootstrapServers);
        System.out.println("=======================");

        AtomicBoolean running = new AtomicBoolean(true);
        AtomicLong totalSent = new AtomicLong(0);
        AtomicLong totalConsumed = new AtomicLong(0);
        LatencyRecorder recorder = new LatencyRecorder(maxRecords);

        boolean withConsumer = mode.equals("both");
        // Counts producers only; the consumer stops when running is cleared.
        CountDownLatch latch = new CountDownLatch(threads);
        ExecutorService executor = Executors.newFixedThreadPool(threads + 2);

        if (withConsumer) {
            executor.submit(new BenchmarkConsumer(bootstrapServers, topic, running, totalConsumed,
                new CountDownLatch(1)));
            Thread.sleep(2000); // let the consumer join the group
            System.out.println("[Consumer] Started");
        }

        executor.submit(() -> {
            try {
                int lastIdx = 0;
                long startTime = System.currentTimeMillis();
                long lastSent = 0;
                long lastConsumed = 0;

                while (running.get()) {
                    Thread.sleep(reportIntervalSec * 1000L);
                    if (!running.get()) break;

                    long elapsed = (System.currentTimeMillis() - startTime) / 1000;
                    long currentSent = totalSent.get();
                    long currentConsumed = totalConsumed.get();
                    long intervalSent = currentSent - lastSent;
                    long intervalConsumed = currentConsumed - lastConsumed;

                    int currentIdx = recorder.getCount();
                    LatencyRecorder.Stats windowStats = recorder.calculateWindow(lastIdx);

                    double actualOps = (double) intervalSent / reportIntervalSec;
                    double throughputMB = actualOps * recordSize / (1024.0 * 1024.0);

                    System.out.printf("[%3ds] sent=%d (%.0f op/s, %.1f MB/s) | %s",
                        elapsed, intervalSent, actualOps, throughputMB, windowStats);
                    if (withConsumer) {
                        System.out.printf(" | consumed=%d (%.0f op/s)", intervalConsumed,
                            (double) intervalConsumed / reportIntervalSec);
                    }
                    System.out.println();

                    lastIdx = currentIdx;
                    lastSent = currentSent;
                    lastConsumed = currentConsumed;
                }
            } catch (InterruptedException ignored) {}
        });

        long startTime = System.currentTimeMillis();
        for (int i = 0; i < threads; i++) {
            executor.submit(new BenchmarkProducer(i, bootstrapServers, topic,
                opsPerSec, threads, recordSize, durationMs, recorder, running, totalSent, latch));
        }
        System.out.printf("[Producer] Started %d threads%n", threads);

        latch.await();
        running.set(false);
        long totalTime = System.currentTimeMillis() - startTime;

        Thread.sleep(500); // let the reporter finish
        executor.shutdownNow();

        LatencyRecorder.Stats finalStats = recorder.calculate();
        long sent = totalSent.get();
        double avgOps = (double) sent / (totalTime / 1000.0);
        double avgMB = avgOps * recordSize / (1024.0 * 1024.0);

        System.out.println();
        System.out.println("=== Final Results ===");
        System.out.printf("Total sent: %d records in %.1f sec%n", sent, totalTime / 1000.0);
        System.out.printf("Throughput: %.0f op/s (%.1f MB/s)%n", avgOps, avgMB);
        System.out.printf("Latency: %s%n", finalStats);
        if (withConsumer) {
            System.out.printf("Total consumed: %d records%n", totalConsumed.get());
        }

        String scenario = withConsumer ? "B" : "A";
        String timestamp = new SimpleDateFormat("yyyyMMdd_HHmmss").format(new Date());
        String csvFile = String.format("%s/result_%s_%dKB_%dops_%s.csv",
            outputDir, scenario, recordSize / 1024, opsPerSec, timestamp);

        try (PrintWriter pw = new PrintWriter(new FileWriter(csvFile))) {
            pw.println("scenario,record_size_bytes,target_ops,actual_ops,throughput_mb_s,"
                + "duration_sec,total_records,avg_ms,p50_ms,p90_ms,p99_ms,max_ms,consumed");
            pw.printf("%s,%d,%d,%.0f,%.1f,%d,%d,%.1f,%d,%d,%d,%d,%d%n",
                scenario, recordSize, opsPerSec, avgOps, avgMB,
                totalTime / 1000, sent,
                finalStats.avg, finalStats.p50, finalStats.p90, finalStats.p99, finalStats.max,
                totalConsumed.get());
        }
        System.out.printf("Results saved: %s%n", csvFile);
    }

    private static String getArg(String[] args, String name, String defaultVal) {
        for (int i = 0; i < args.length - 1; i++) {
            if (args[i].equals(name)) return args[i + 1];
        }
        return defaultVal;
    }
}
