#!/usr/bin/env bash
# T6: ext4 on dm-zns 위 Kafka 장시간 부하. 보존 크기를 작게 잡아 segment 삭제와 GC 가 계속 일어나게 한다.
#   sudo KAFKA=<Kafka 4.2.0 경로> DURATION=3600 bash scripts/eval-t6.sh [OUT]
set -u
OUT=${1:-results/t6}
DURATION=${DURATION:-3600}
OPS=${OPS:-2000}
RS=${RS:-10240}
KAFKA_EXTRA="log.retention.bytes=1073741824;log.segment.bytes=134217728;log.retention.check.interval.ms=10000"
. scripts/eval-common.sh
mkdir -p "$OUT"
mark=$(dmesg | wc -l)

ev_up dmzns || { echo "setup 실패"; exit 1; }
kafka_up "$MNT/kafka-logs" || { echo "kafka 기동 실패"; exit 1; }
topic_reset
dmsetup status ev-dmzns >"$OUT/status-start.txt"
( while sleep 60; do echo "$(date +%s) $(dmsetup status ev-dmzns | cut -d' ' -f4-)"; done ) >"$OUT/status-trace.txt" &
TP=$!
iostat -x -t 10 >"$OUT/iostat.log" & IOP=$!
java -cp "$CP" benchmark.BenchmarkMain --bootstrap-server localhost:9092 --topic perf-test \
	--mode both --threads 4 --ops "$OPS" --record-size "$RS" --duration "$DURATION" \
	--output-dir "$OUT" --report-interval 30 >"$OUT/log.txt" 2>&1
kill $TP $IOP 2>/dev/null; wait $TP $IOP 2>/dev/null
dmsetup status ev-dmzns >"$OUT/status-end.txt"
du -sh "$MNT/kafka-logs" >"$OUT/du.txt"
kafka_down
umount "$MNT"
e2fsck -fn /dev/mapper/ev-dmzns >"$OUT/fsck.txt" 2>&1; echo "e2fsck=$?" >>"$OUT/fsck.txt"
dmesg | tail -n +$((mark + 1)) >"$OUT/kernel.dmesg"
echo "kernel warnings: $(grep -cE 'WARNING:|BUG:|Oops|hung_task|blocked for more than|soft lockup' "$OUT/kernel.dmesg")" | tee "$OUT/summary.txt"
grep -E '^Throughput|^Latency|^Total' "$OUT/log.txt" | tee -a "$OUT/summary.txt"
tail -1 "$OUT/fsck.txt" | tee -a "$OUT/summary.txt"
ev_down
