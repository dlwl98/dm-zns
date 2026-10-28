#!/usr/bin/env bash
# 세 구성과 ZNS 직접(기준선)의 fio 성능.
#   sudo DMZADM=<dmzadm 경로> bash scripts/eval-fio.sh [OUT] [SIZE]
set -u
OUT=${1:-results/fio}
SIZE=${2:-4G}
. scripts/eval-common.sh
mkdir -p "$OUT"
CSV=$OUT/fio.csv
echo "config,job,bw_mib_s,iops,clat_p50_us,clat_p99_us" >"$CSV"

emit() {
	python3 - "$1" "$2" "$3" >>"$CSV" <<'EOF'
import json, sys
cfg, job, path = sys.argv[1:4]
j = json.load(open(path))["jobs"][0]
side = j["write"] if j["write"]["io_bytes"] > 0 else j["read"]
pct = side["clat_ns"].get("percentile", {})
p50 = pct.get("50.000000", 0) / 1000
p99 = pct.get("99.000000", 0) / 1000
print(f"{cfg},{job},{side['bw']/1024:.1f},{side['iops']:.0f},{p50:.0f},{p99:.0f}")
EOF
}

run() {
	local cfg=$1 job=$2
	shift 2
	sync; echo 3 >/proc/sys/vm/drop_caches
	fio --name="$job" --output-format=json "$@" >"$OUT/$cfg-$job.json" 2>"$OUT/$cfg-$job.err" \
		&& emit "$cfg" "$job" "$OUT/$cfg-$job.json" \
		|| echo "$cfg,$job,FAIL,,," >>"$CSV"
	tail -1 "$CSV"
}

for cfg in ${CONFIGS:-dmzns dmzoned f2fs}; do
	echo "== $cfg"
	if ! ev_up "$cfg"; then echo "$cfg,setup,FAIL,,," >>"$CSV"; continue; fi
	echo "   $EV_DEV, 사용 가능 $(ev_avail_mb) MiB"
	F="--filename=$MNT/fio.dat --size=$SIZE --ioengine=libaio --direct=1"
	run "$cfg" seqwrite  $F --rw=write     --bs=1M --iodepth=8 --end_fsync=1
	run "$cfg" seqread   $F --rw=read      --bs=1M --iodepth=8
	run "$cfg" randwrite $F --rw=randwrite --bs=4k --iodepth=32 --time_based --runtime=60 --end_fsync=1
	run "$cfg" randread  $F --rw=randread  --bs=4k --iodepth=32 --time_based --runtime=30
	[ "$cfg" = dmzns ] && dmsetup status ev-dmzns >"$OUT/dmzns-status.txt"
	ev_down
done

echo "== raw ZNS (기준선)"
ev_down
run raw seqwrite --filename="$ZDEV" --zonemode=zbd --size=$SIZE --ioengine=libaio --direct=1 --rw=write --bs=1M --iodepth=8
run raw seqread  --filename="$ZDEV" --zonemode=zbd --size=$SIZE --ioengine=libaio --direct=1 --rw=read  --bs=1M --iodepth=8
run raw randread --filename="$ZDEV" --zonemode=zbd --size=$SIZE --ioengine=libaio --direct=1 --rw=randread --bs=4k --iodepth=32 --time_based --runtime=30
ev_down
echo; column -s, -t "$CSV"
