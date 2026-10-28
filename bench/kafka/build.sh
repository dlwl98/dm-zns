#!/usr/bin/env bash
# Kafka 부하 생성기 빌드 (JDK 17). 실행할 때도 Kafka 배포판의 libs 를 classpath 에 둔다.
#   KAFKA=<Kafka 4.2.0 경로> bash bench/kafka/build.sh
set -e
KAFKA=${KAFKA:-/opt/kafka_2.13-4.2.0}
D=$(cd "$(dirname "$0")" && pwd)
rm -rf "$D/build"
mkdir -p "$D/build"
javac -cp "$KAFKA/libs/*" -d "$D/build" "$D"/src/main/java/benchmark/*.java
jar cf "$D/kafka-benchmark.jar" -C "$D/build" benchmark
echo "$D/kafka-benchmark.jar"
