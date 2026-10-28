package benchmark;

import org.apache.kafka.clients.consumer.*;
import org.apache.kafka.common.serialization.ByteArrayDeserializer;
import org.apache.kafka.common.serialization.StringDeserializer;

import java.time.Duration;
import java.util.Collections;
import java.util.Properties;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.CountDownLatch;

public class BenchmarkConsumer implements Runnable {
    private final String bootstrapServers;
    private final String topic;
    private final AtomicBoolean running;
    private final AtomicLong totalConsumed;
    private final CountDownLatch latch;

    public BenchmarkConsumer(String bootstrapServers, String topic,
                             AtomicBoolean running, AtomicLong totalConsumed, CountDownLatch latch) {
        this.bootstrapServers = bootstrapServers;
        this.topic = topic;
        this.running = running;
        this.totalConsumed = totalConsumed;
        this.latch = latch;
    }

    @Override
    public void run() {
        Properties props = new Properties();
        props.put(ConsumerConfig.BOOTSTRAP_SERVERS_CONFIG, bootstrapServers);
        props.put(ConsumerConfig.GROUP_ID_CONFIG, "benchmark-consumer-group");
        props.put(ConsumerConfig.KEY_DESERIALIZER_CLASS_CONFIG, StringDeserializer.class.getName());
        props.put(ConsumerConfig.VALUE_DESERIALIZER_CLASS_CONFIG, ByteArrayDeserializer.class.getName());
        props.put(ConsumerConfig.AUTO_OFFSET_RESET_CONFIG, "latest");
        props.put(ConsumerConfig.FETCH_MIN_BYTES_CONFIG, "1");
        props.put(ConsumerConfig.MAX_POLL_RECORDS_CONFIG, "500");

        try (KafkaConsumer<String, byte[]> consumer = new KafkaConsumer<>(props)) {
            consumer.subscribe(Collections.singletonList(topic));

            while (running.get()) {
                ConsumerRecords<String, byte[]> records = consumer.poll(Duration.ofMillis(100));
                totalConsumed.addAndGet(records.count());
            }
        } catch (Exception e) {
            System.err.printf("[Consumer] Fatal: %s%n", e.getMessage());
        } finally {
            latch.countDown();
        }
    }
}
