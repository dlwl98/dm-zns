#!/usr/bin/env bash
# max_util 별 WAF 와 GC·컴팩션·체크포인트 비용. 노출 용량을 채운 뒤 같은 양을 16K 임의 덮어쓰기 한다.
#   sudo bash scripts/eval-op.sh [OUT]
set -u
OUT=${1:-results/op}
UTILS=${UTILS:-"600 750 900"}
ZDEV=${ZDEV:-/dev/nvme0n1}
UNDERLYING=$ZDEV
NAME=ev-op
DEV=/dev/mapper/$NAME
. scripts/common.sh
mkdir -p "$OUT"
st() { dmsetup status "$NAME" | awk -v n="$1" '{print $n}'; }
CSV=$OUT/op.csv
echo "max_util,exposed_mib,fill_s,churn_mib,churn_s,churn_mib_s,gc_runs,migrated_mib,waf,gc_ms_avg,compactions,compact_ms,compact_mib_read,ckpts,ckpt_ms,log_blocks,index_kib" >"$CSV"

for U in $UTILS; do
	dmsetup remove "$NAME" 2>/dev/null
	zns_load_module src/dm-zns-base.ko || exit 1
	echo mq-deadline >"/sys/block/$(basename "$ZDEV")/queue/scheduler"
	ZNS_MAX_UTIL=$U zns_table "$ZDEV" format | dmsetup create "$NAME" || { echo "$U: create 실패"; continue; }
	CAP=$(( $(blockdev --getsz "$DEV") / 2048 ))
	echo "== max_util $U: 노출 ${CAP} MiB"
	t0=$(date +%s)
	fio --name=fill --filename="$DEV" --rw=write --bs=1M --iodepth=8 --direct=1 --ioengine=libaio \
	    --size="${CAP}M" --minimal >"$OUT/fill-$U.txt" 2>&1
	fill_s=$(( $(date +%s) - t0 ))
	gc0=$(st 9); mig0=$(st 10); gcms0=$(st 29)
	t0=$(date +%s)
	fio --name=churn --filename="$DEV" --rw=randwrite --bs=16k --iodepth=16 --direct=1 --ioengine=libaio \
	    --size="${CAP}M" --io_size="${CAP}M" --minimal >"$OUT/churn-$U.txt" 2>&1 || echo "  churn 에러"
	churn_s=$(( $(date +%s) - t0 ))
	dmsetup status "$NAME" >"$OUT/status-$U.txt"
	gc=$(( $(st 9) - gc0 )); mig=$(( ($(st 10) - mig0) * 4 / 1024 ))
	gcms=$(( $(st 29) - gcms0 ))
	python3 - "$U" "$CAP" "$fill_s" "$churn_s" "$gc" "$mig" "$gcms" "$(st 14)" "$(st 30)" "$(st 31)" \
	    "$(st 23)" "$(st 32)" "$(st 21)" "$(st 25)" >>"$CSV" <<'EOF'
import sys
u, cap, fill_s, churn_s, gc, mig, gcms, comp, comp_ms, comp_ents, ck, ck_ms, logb, idx = map(int, sys.argv[1:15])
waf = (cap + mig) / cap
print(f"{u},{cap},{fill_s},{cap},{churn_s},{cap/max(churn_s,1):.1f},{gc},{mig},{waf:.2f},"
      f"{gcms/max(gc,1):.0f},{comp},{comp_ms},{comp_ents*16//(1<<20)},{ck},{ck_ms},{logb},{idx}")
EOF
	tail -1 "$CSV"
	dmsetup remove "$NAME"
done
blkzone reset "$ZDEV"
echo; column -s, -t "$CSV"
