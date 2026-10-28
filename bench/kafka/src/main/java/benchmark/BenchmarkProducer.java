package benchmark;

import org.apache.kafka.clients.producer.*;
import org.apache.kafka.common.serialization.ByteArraySerializer;
import org.apache.kafka.common.serialization.StringSerializer;

import java.util.Properties;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

public class BenchmarkProducer implements Runnable {
    private final int threadId;
    private final String bootstrapServers;
    private final String topic;
    private final int opsPerSecTotal;
    private final int numThreads;
    private final int recordSize;
    private final long durationMs;
    private final LatencyRecorder recorder;
    private final AtomicBoolean running;
    private final AtomicLong totalSent;
    private final CountDownLatch latch;

    public BenchmarkProducer(int threadId, String bootstrapServers, String topic,
                             int opsPerSecTotal, int numThreads, int recordSize,
                             long durationMs, LatencyRecorder recorder,
                             AtomicBoolean running, AtomicLong totalSent, CountDownLatch latch) {
        this.threadId = threadId;
        this.bootstrapServers = bootstrapServers;
        this.topic = topic;
        this.opsPerSecTotal = opsPerSecTotal;
        this.numThreads = numThreads;
        this.recordSize = recordSize;
        this.durationMs = durationMs;
        this.recorder = recorder;
        this.running = running;
        this.totalSent = totalSent;
        this.latch = latch;
    }

    @Override
    public void run() {
        Properties props = new Properties();
        props.put(ProducerConfig.BOOTSTRAP_SERVERS_CONFIG, bootstrapServers);
        props.put(ProducerConfig.KEY_SERIALIZER_CLASS_CONFIG, StringSerializer.class.getName());
        props.put(ProducerConfig.VALUE_SERIALIZER_CLASS_CONFIG, ByteArraySerializer.class.getName());
        props.put(ProducerConfig.ACKS_CONFIG, "1");
        props.put(ProducerConfig.LINGER_MS_CONFIG, "5");
        props.put(ProducerConfig.BATCH_SIZE_CONFIG, String.valueOf(Math.max(16384, recordSize * 8)));
        props.put(ProducerConfig.BUFFER_MEMORY_CONFIG, String.valueOf(64 * 1024 * 1024));
        props.put(ProducerConfig.MAX_REQUEST_SIZE_CONFIG, String.valueOf(Math.max(1048576, recordSize + 1024)));

        byte[] payload = new byte[recordSize];
        // Fill with non-zero data to avoid compression shortcuts
        for (int i = 0; i < payload.length; i++) payload[i] = (byte) (i % 128);

        int myOpsPerSec = opsPerSecTotal / numThreads;
        long intervalNs = 1_000_000_000L / myOpsPerSec;

        try (KafkaProducer<String, byte[]> producer = new KafkaProducer<>(props)) {
            long startTime = System.currentTimeMillis();
            long nextSendNs = System.nanoTime();

            while (running.get() && (System.currentTimeMillis() - startTime) < durationMs) {
                long now = System.nanoTime();
                if (now < nextSendNs) {
                    long waitNs = nextSendNs - now;
                    if (waitNs > 1_000_000) {
                        Thread.sleep(waitNs / 2_000_000);
                    }
                    continue;
                }

                long sendStart = System.currentTimeMillis();
                ProducerRecord<String, byte[]> record = new ProducerRecord<>(topic, payload);

                producer.send(record, (metadata, exception) -> {
                    long latency = System.currentTimeMillis() - sendStart;
                    recorder.record(latency);
                    if (exception != null) {
                        System.err.printf("[Producer-%d] Error: %s%n", threadId, exception.getMessage());
                    }
                });

                totalSent.incrementAndGet();
                nextSendNs += intervalNs;

                // Prevent falling too far behind
                if (System.nanoTime() - nextSendNs > 1_000_000_000L) {
                    nextSendNs = System.nanoTime();
                }
            }

            producer.flush();
        } catch (Exception e) {
            System.err.printf("[Producer-%d] Fatal: %s%n", threadId, e.getMessage());
        } finally {
            latch.countDown();
        }
    }
}
