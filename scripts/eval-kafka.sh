#!/usr/bin/env bash
# 세 구성 위 Kafka. Scenario A 는 Producer 4 스레드, B 는 Consumer 1개를 더한다. 한 단계 DUR 초.
#   sudo KAFKA=<Kafka 4.2.0 경로> DMZADM=<dmzadm 경로> bash scripts/eval-kafka.sh [OUT] [configs...]
set -u
OUT=${1:-results/kafka}
shift || true
CONFIGS=${*:-dmzns dmzoned f2fs}
DUR=${DUR:-30}
LIMIT_MB=${LIMIT_MB:-6144}
. scripts/eval-common.sh
mkdir -p "$OUT"

steps() {
	case $1 in
	1024)    echo "1000 5000 10000 20000 50000" ;;
	10240)   echo "500 1000 5000 10000" ;;
	102400)  echo "100 500 1000 3000" ;;
	1048576) echo "50 100 500" ;;
	esac
}

for cfg in $CONFIGS; do
	echo "== $cfg"
	ev_up "$cfg" || { echo "  setup 실패"; continue; }
	echo "   $EV_DEV, 사용 가능 $(ev_avail_mb) MiB"
	mkdir -p "$OUT/$cfg"
	kafka_up "$MNT/kafka-logs" || { echo "  kafka 기동 실패"; kafka_down; ev_down; continue; }
	for SC in A B; do
		MODE=producer; [ "$SC" = B ] && MODE=both
		for RS in 1024 10240 102400 1048576; do
			for OPS in $(steps $RS); do
				mb=$(( OPS * RS * DUR / 1048576 ))
				if [ "$mb" -gt "$LIMIT_MB" ]; then echo "  $SC ${RS}B ${OPS}op/s: skip (${mb} MiB)"; continue; fi
				topic_reset
				sync; echo 3 >/proc/sys/vm/drop_caches
				iostat -x -t 1 >"$OUT/$cfg/iostat_${SC}_${RS}_${OPS}.log" & IOP=$!
				java -cp "$CP" benchmark.BenchmarkMain --bootstrap-server localhost:9092 --topic perf-test \
					--mode $MODE --threads 4 --ops "$OPS" --record-size "$RS" --duration "$DUR" \
					--output-dir "$OUT/$cfg" --report-interval 5 >"$OUT/$cfg/log_${SC}_${RS}_${OPS}.txt" 2>&1
				kill $IOP 2>/dev/null; wait $IOP 2>/dev/null
				echo "  $SC ${RS}B ${OPS}op/s: $(grep -E '^Throughput|^Latency' "$OUT/$cfg/log_${SC}_${RS}_${OPS}.txt" | tr '\n' ' ')"
			done
		done
	done
	kafka_down
	[ "$cfg" = dmzns ] && dmsetup status ev-dmzns >"$OUT/dmzns-status.txt"
	ev_down
done

{
	echo "config,scenario,record_size_bytes,target_ops,actual_ops,throughput_mb_s,duration_sec,total_records,avg_ms,p50_ms,p90_ms,p99_ms,max_ms,consumed"
	for cfg in $CONFIGS; do
		for f in "$OUT/$cfg"/result_*.csv; do [ -f "$f" ] && tail -n +2 "$f" | sed "s/^/$cfg,/"; done
	done
} >"$OUT/kafka.csv"
echo "== $OUT/kafka.csv ($(($(wc -l <"$OUT/kafka.csv") - 1)) rows)"
