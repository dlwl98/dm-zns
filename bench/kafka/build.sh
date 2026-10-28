#!/usr/bin/env bash
# Kafka 부하 생성기 빌드 (JDK 17).  Kafka 배포판의 libs 로 컴파일하고, 실행할 때도 같은 libs 를 쓴다.
#   KAFKA=<Kafka 4.2.0 경로> bash bench/kafka/build.sh   →  bench/kafka/kafka-benchmark.jar
#
# 실행:  java -cp "bench/kafka/kafka-benchmark.jar:$KAFKA/libs/*" benchmark.BenchmarkMain \
#            --mode producer|both --threads 4 --ops <OP/s> --record-size <bytes> --duration <s> \
#            --output-dir <dir> [--report-interval 5] [--topic perf-test] [--bootstrap-server localhost:9092]
#   OP/s 를 고정해 보내고, 전송 시점부터 브로커 응답(acks=1)까지의 지연을 기록한다.
#   끝나면 avg/p50/p90/p99/max 를 <dir>/result_<A|B>_<KB>KB_<ops>ops_<시각>.csv 로 남긴다.
set -e
KAFKA=${KAFKA:-/opt/kafka_2.13-4.2.0}
D=$(cd "$(dirname "$0")" && pwd)
rm -rf "$D/build"
mkdir -p "$D/build"
javac -cp "$KAFKA/libs/*" -d "$D/build" "$D"/src/main/java/benchmark/*.java
jar cf "$D/kafka-benchmark.jar" -C "$D/build" benchmark
echo "$D/kafka-benchmark.jar"
