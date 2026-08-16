#!/usr/bin/env bash
# Phase 4 — 매핑을 평면 배열에서 LSM-Tree 로 바꾼 뒤의 검증.
#
#   T1  정확성 회귀 — 큰 읽기 == 작은 읽기
#   T2  정확성 회귀 — crc32c 왕복
#   T3  LSM 이 실제로 도는가 — flush / compaction 발생
#   T4  컴팩션이 죽은 매핑을 집계하는가 — M3 이 회수할 몫의 정량화
#
# T1·T2 는 Phase 2 와 같은 검사다. 자료구조를 갈아엎었으므로 회귀부터 본다.
#
# 사용:  sudo bash scripts/phase4-verify.sh
set -uo pipefail

# shellcheck source=scripts/common.sh
. "$(dirname "$0")/common.sh"

DM_NAME=${DM_NAME:-my-m1-device}
UNDERLYING=${UNDERLYING:-/dev/nullb0}
MOD_KO="src/dm-zns-base.ko"
MOD_NAME="dm_zns_base"
DEV="/dev/mapper/$DM_NAME"
T=/tmp/zns-phase4
SCATTER_MB=32

[ "$(id -u)" -eq 0 ] || { echo "root 로 실행하세요:  sudo bash $0" >&2; exit 1; }
[ -f "$MOD_KO" ] || { echo "$MOD_KO 가 없습니다. 먼저 'cd src && make'." >&2; exit 1; }

mkdir -p "$T"
pass=0; fail=0
ok()   { echo "  [OK]   $*"; pass=$((pass+1)); }
bad()  { echo "  [FAIL] $*"; fail=$((fail+1)); }
step() { echo; echo "=== $* ==="; }

# dmsetup status 필드:
#   $4 wp  $5 total  $6 mem_n  $7 nr_runs  $8 flushes  $9 compactions  $10 dropped
stat_f() { dmsetup status "$DM_NAME" 2>/dev/null | awk -v n="$1" '{print $n}'; }

show_stat() {
	local wp tot mem runs fl cp dr
	wp=$(stat_f 4);  tot=$(stat_f 5); mem=$(stat_f 6); runs=$(stat_f 7)
	fl=$(stat_f 8);  cp=$(stat_f 9);  dr=$(stat_f 10)
	printf "    wp %d MiB / %d MiB   memtable %s   runs %s   flush %s   compact %s   dropped %s\n" \
		$((wp/2048)) $((tot/2048)) "$mem" "$runs" "$fl" "$cp" "$dr"
}

create_target() {
	local sectors
	sectors=$(blockdev --getsz "$UNDERLYING")
	echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME"
}

step "0. 환경 구성"
bash scripts/teardown.sh >/dev/null 2>&1
bash scripts/nullblk-up.sh >/dev/null || { echo "nullblk-up 실패" >&2; exit 1; }
zns_load_module "$MOD_KO" || exit 1
create_target || { echo "dmsetup create 실패"; dmesg | tail -10; exit 1; }
echo "  $DEV 준비"
show_stat

# =====================================================================
step "T1. 정확성 회귀 — 큰 읽기 == 작은 읽기"

fio --name=scatter --filename="$DEV" --rw=randwrite --bs=4k \
    --size=${SCATTER_MB}M --ioengine=libaio --iodepth=32 --direct=1 \
    --minimal >"$T/scatter.txt" 2>&1 \
	|| { bad "흩뿌리기 실패"; tail -3 "$T/scatter.txt"; }

dd if="$DEV" of="$T/big.bin"   bs=1M   count=$SCATTER_MB          iflag=direct status=none
dd if="$DEV" of="$T/small.bin" bs=4096 count=$((SCATTER_MB*256))  iflag=direct status=none

if cmp -s "$T/big.bin" "$T/small.bin"; then
	ok "1 MiB 읽기 == 4 KiB 읽기 — LSM 조회 경로 정상"
else
	bad "불일치 — $(cmp "$T/big.bin" "$T/small.bin" 2>&1 | head -1)"
fi
show_stat

# =====================================================================
step "T2. 정확성 회귀 — crc32c 왕복"

if fio --name=vfy --filename="$DEV" --rw=randwrite --bs=4k --size=32M \
       --ioengine=libaio --iodepth=16 --direct=1 \
       --verify=crc32c --do_verify=1 --verify_fatal=1 \
       --minimal >"$T/verify.txt" 2>&1; then
	ok "crc32c 통과 — 쓴 데이터가 그대로 읽힘"
else
	bad "crc32c 실패"
	grep -iE "verify|error|bad" "$T/verify.txt" | head -5
fi

# =====================================================================
# T3 — MemTable 이 4096 엔트리(16 MiB)마다 run 으로 내려가고,
#      run 이 8개 쌓이면(128 MiB) 컴팩션이 돈다.
# =====================================================================
step "T3. LSM 이 실제로 도는가"

fl_before=$(stat_f 8); cp_before=$(stat_f 9)

fio --name=bulk --filename="$DEV" --rw=write --bs=1M --size=256M \
    --ioengine=libaio --iodepth=8 --direct=1 --minimal >"$T/bulk.txt" 2>&1 \
	|| bad "대량 쓰기 실패"

fl_after=$(stat_f 8); cp_after=$(stat_f 9)
echo "  256 MiB 기록 후:"
show_stat

if [ "$((fl_after - fl_before))" -gt 0 ]; then
	ok "MemTable flush 발생 ($((fl_after - fl_before))회) — run 이 만들어진다"
else
	bad "flush 가 한 번도 안 일어났다"
fi

if [ "$((cp_after - cp_before))" -gt 0 ]; then
	ok "컴팩션 발생 ($((cp_after - cp_before))회) — run 이 병합된다"
else
	bad "컴팩션이 안 돌았다 (run 임계에 못 미쳤을 수 있음)"
fi

# =====================================================================
# T4 — 같은 자리를 반복해서 덮어쓴다.
#
#      논리적으로는 계속 같은 64 MiB 인데, GC 가 없으므로 물리 공간은
#      쓴 만큼 계속 줄어든다. 그 차이가 곧 죽은 데이터이고, 컴팩션이
#      밀어낸 매핑 수(dropped)가 그것을 집계한다.
# =====================================================================
step "T4. 덮어쓰기가 만들어내는 쓰레기를 집계하는가"

bash scripts/teardown.sh >/dev/null 2>&1
bash scripts/nullblk-up.sh >/dev/null
zns_load_module "$MOD_KO" || exit 1
create_target

LIVE_MB=64
ROUNDS=4

for r in $(seq 1 $ROUNDS); do
	fio --name=ow --filename="$DEV" --rw=write --bs=1M --size=${LIVE_MB}M \
	    --ioengine=libaio --iodepth=8 --direct=1 --minimal \
	    >"$T/ow$r.txt" 2>&1 || bad "덮어쓰기 $r 회차 실패"
	printf "  %d회차 (누적 %d MiB 기록, 살아있는 데이터는 여전히 %d MiB)\n" \
		"$r" "$((LIVE_MB * r))" "$LIVE_MB"
	show_stat
done

wp_mb=$(( $(stat_f 4) / 2048 ))
dropped=$(stat_f 10)
dropped_mb=$(( dropped * 4 / 1024 ))      # 엔트리 1개 = 4 KiB 블록
garbage_mb=$(( wp_mb - LIVE_MB ))

echo
echo "  소모한 물리 공간 : ${wp_mb} MiB"
echo "  살아있는 데이터  : ${LIVE_MB} MiB"
echo "  쓰레기           : ${garbage_mb} MiB   ← M3 의 GC 가 회수할 몫"
echo "  집계된 dropped   : ${dropped} 엔트리 = ${dropped_mb} MiB"

if [ "$dropped" -gt 0 ]; then
	ok "죽은 매핑이 집계된다 — M3 의 valid/invalid 판정이 컴팩션에서 나온다"
else
	bad "dropped 가 0 — 컴팩션이 중복을 못 걸러내고 있다"
fi

# 아직 컴팩션되지 않은 run 안의 중복은 집계 전이므로 dropped 는 하한이다.
if [ "$dropped_mb" -le "$garbage_mb" ]; then
	ok "dropped(${dropped_mb} MiB) ≤ 실제 쓰레기(${garbage_mb} MiB) — 하한으로 타당"
else
	bad "dropped 가 실제 쓰레기보다 크다 — 중복 집계 의심"
fi

# =====================================================================
step "결과"
echo "  통과 $pass / 실패 $fail"
[ $fail -eq 0 ] && echo "  >>> PHASE 4 PASSED <<<" || echo "  >>> 실패 항목 있음 <<<"
echo
echo "  ext4 회귀도 같이 봐야 합니다:  sudo bash scripts/phase3-verify.sh"

step "정리"
bash scripts/teardown.sh

exit $fail
