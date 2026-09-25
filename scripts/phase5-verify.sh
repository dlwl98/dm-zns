#!/usr/bin/env bash
# Phase 5 / M3 — GC + zone reset 사이클 검증.
#
#   T1  결정적 이주 — 특정 zone 을 지정해 GC 를 강제하고, 이주된 데이터가
#       비트 단위로 보존되는지 fio verify 패턴으로 확인한다.
#       ("GC 가 언젠가 그 zone 을 고르겠지" 에 기대지 않는다.)
#   T2  valid==0 공짜 회수 — 전부 죽은 zone 은 이주 없이 reset 만으로.
#   T3  M3 공식 기준 — 용량의 80% 를 채우고 같은 영역에 1.2배 분량을
#       random overwrite. "no space left" 없이 완주해야 한다.
#
# status 필드: $4 used $5 total $6 free_z $7 nr_z $8 valid $9 gc
#              $10 migrated $11 mem $12 runs $13 flush $14 compact $15 dropped
#
# 사용:  sudo bash scripts/phase5-verify.sh
set -uo pipefail

# shellcheck source=scripts/common.sh
. "$(dirname "$0")/common.sh"

DM_NAME=${DM_NAME:-my-m1-device}
UNDERLYING=${UNDERLYING:-/dev/nullb0}
MOD_KO="src/dm-zns-base.ko"
DEV="/dev/mapper/$DM_NAME"
T=/tmp/zns-phase5

[ "$(id -u)" -eq 0 ] || { echo "root 로 실행하세요:  sudo bash $0" >&2; exit 1; }
[ -f "$MOD_KO" ] || { echo "$MOD_KO 가 없습니다. 먼저 'cd src && make'." >&2; exit 1; }

mkdir -p "$T"
pass=0; fail=0
ok()   { echo "  [OK]   $*"; pass=$((pass+1)); }
bad()  { echo "  [FAIL] $*"; fail=$((fail+1)); }
step() { echo; echo "=== $* ==="; }

stat_f() { dmsetup status "$DM_NAME" 2>/dev/null | awk -v n="$1" '{print $n}'; }

show_stat() {
	printf "    used %d MiB / %d MiB   free_zones %s/%s   valid_blk %s   gc %s   migrated %s\n" \
		$(( $(stat_f 4) / 2048 )) $(( $(stat_f 5) / 2048 )) \
		"$(stat_f 6)" "$(stat_f 7)" "$(stat_f 8)" "$(stat_f 9)" "$(stat_f 10)"
}

zone_state() {	# zone_state <idx>  →  "F:0" 꼴
	dmsetup message "$DM_NAME" 0 zones 2>/dev/null \
		| tr ' ' '\n' | awk -F: -v z="$1" '$1==z {print $2":"$3}'
}

fresh_target() {
	bash scripts/teardown.sh >/dev/null 2>&1
	zns_under_up >/dev/null || return 1
	zns_load_module "$MOD_KO" || return 1
	zns_create_target "$DM_NAME" "$UNDERLYING" || return 1
}

FIO_COMMON="--filename=$DEV --direct=1 --ioengine=libaio --minimal"

# 크기는 전부 하위 장치 지오메트리에서 유도한다 (zone 크기와 무관하게
# 같은 검사가 되도록).
fresh_target || { echo "환경 구성 실패"; exit 1; }
ZONE_MB=$(zns_zone_mb)
CAP_MB=$(zns_cap_mb)
HALF_MB=$(( ZONE_MB / 2 ))
HALF_BLK=$(( HALF_MB * 256 ))		# 1 MiB = 4 KiB 블록 256개
echo "  하위 장치: zone ${ZONE_MB} MiB, 용량 ${CAP_MB} MiB"

# =====================================================================
step "T1. 결정적 이주 — 지정 GC 후 데이터 보존"
# 배치 (Z = zone 크기):
#   X  : LBA[  0, Z/2)          → 물리 zone0 앞 절반
#   A  : LBA[Z/2, Z  ) 검증패턴  → 물리 zone0 뒷 절반  (zone0 만석)
#   X' : LBA[  0, Z/2) 재기록    → zone1 으로. zone0 은 FULL, valid 절반(A만)
#   gc 0                         → A 의 Z/2 만큼이 이주되고 zone0 은 FREE
#   A verify_only                → 이주를 거친 A 가 비트 단위로 그대로인가
# =====================================================================

ANCHOR_JOB="--name=anchor --offset=${HALF_MB}M --size=${HALF_MB}M --rw=write --bs=4k \
            --iodepth=16 --verify=crc32c --randseed=777"

fio --name=x  --offset=0 --size=${HALF_MB}M --rw=write --bs=1M --iodepth=8 \
    $FIO_COMMON >"$T/x1.txt" 2>&1 || bad "X 기록 실패"
fio $ANCHOR_JOB --do_verify=0 $FIO_COMMON >"$T/anchor-w.txt" 2>&1 \
	|| bad "앵커 기록 실패"
fio --name=x2 --offset=0 --size=${HALF_MB}M --rw=write --bs=1M --iodepth=8 \
    $FIO_COMMON >"$T/x2.txt" 2>&1 || bad "X 재기록 실패"

z0=$(zone_state 0)
echo "  zone0 상태: $z0   (기대: U:$HALF_BLK — FULL, 앵커 ${HALF_BLK}블록만 생존)"
[ "$z0" = "U:$HALF_BLK" ] && ok "zone0 이 기대 상태" || bad "zone0 상태가 다름: $z0"

mig_before=$(stat_f 10)
free_before=$(stat_f 6)

msg=$(dmsetup message "$DM_NAME" 0 gc 0 2>&1) || true
echo "  message gc 0 → ${msg:-(응답 없음)}"

mig_delta=$(( $(stat_f 10) - mig_before ))
free_delta=$(( $(stat_f 6) - free_before ))
z0=$(zone_state 0)
show_stat

[ "$mig_delta" -eq "$HALF_BLK" ] \
	&& ok "정확히 $HALF_BLK 블록(${HALF_MB} MiB) 이주됨" \
	|| bad "migrated 델타 $mig_delta (기대 $HALF_BLK)"
# free 순증은 0 이 맞다: 첫 GC 는 이주분을 받을 GC 전용 zone 을 새로
# 열고(free -1) victim 을 회수한다(free +1). 회수 자체는 zone0 상태로,
# 죽어 있던 Z/2 의 반환은 used 감소(1.5Z → Z)로 확인한다.
[ "$z0" = "F:0" ] \
	&& ok "zone0 이 reset 되어 FREE 로 (free 순증 $free_delta — 이주분은 GC 전용 zone 이 수용)" \
	|| bad "zone0 회수 안 됨 (zone0=$z0)"
used_mb=$(( $(stat_f 4) / 2048 ))
[ "$used_mb" -eq "$ZONE_MB" ] \
	&& ok "used ${used_mb} MiB — 죽어 있던 ${HALF_MB} MiB 가 회수됨" \
	|| bad "used ${used_mb} MiB (기대 $ZONE_MB)"

if fio $ANCHOR_JOB --verify_only=1 $FIO_COMMON >"$T/anchor-v.txt" 2>&1; then
	ok "verify_only 통과 — 이주를 거친 앵커가 비트 단위로 보존"
else
	bad "verify 실패 — GC 이주가 데이터를 깨뜨렸다"
	grep -iE "verify|error|bad" "$T/anchor-v.txt" | head -5
fi

# =====================================================================
step "T2. valid==0 zone 의 공짜 회수 (이주 없이 reset 만)"
# =====================================================================
fresh_target || { echo "환경 구성 실패"; exit 1; }

fio --name=w1 --offset=0 --size=${ZONE_MB}M --rw=write --bs=1M --iodepth=8 \
    $FIO_COMMON >"$T/w1.txt" 2>&1 || bad "1차 기록 실패"
fio --name=w2 --offset=0 --size=${ZONE_MB}M --rw=write --bs=1M --iodepth=8 \
    $FIO_COMMON >"$T/w2.txt" 2>&1 || bad "재기록 실패"

z0=$(zone_state 0)
[ "$z0" = "U:0" ] && ok "zone0 = FULL, valid 0 (전부 죽음)" \
	|| bad "zone0 상태가 다름: $z0 (기대 U:0)"

mig_before=$(stat_f 10)
free_before=$(stat_f 6)
dmsetup message "$DM_NAME" 0 gc 0 >/dev/null 2>&1 || true

mig_delta=$(( $(stat_f 10) - mig_before ))
free_delta=$(( $(stat_f 6) - free_before ))
[ "$mig_delta" -eq 0 ] && [ "$free_delta" -eq 1 ] \
	&& ok "이주 0 블록으로 회수 — 공짜 reset 경로 동작" \
	|| bad "mig_delta=$mig_delta free_delta=$free_delta (기대 0/+1)"

# =====================================================================
step "T3. M3 공식 기준 — 80% 채우고 1.2배 overwrite"
# 용량의 80% 를 채운 뒤 같은 영역에 그 1.2배를 randwrite.
# GC 없이는 물리적으로 불가능한 워크로드다 (필요 쓰기 1.76 × 용량 > 용량).
#
# P5_T3=0 이면 건너뛴다 (큰 장치에서는 오래 걸리므로 T1/T2 와 따로 돌릴 때).
# =====================================================================
if [ "${P5_T3:-1}" = 0 ]; then
	echo "  [SKIP] P5_T3=0"
else
fresh_target || { echo "환경 구성 실패"; exit 1; }
FILL_MB=$(( CAP_MB * 80 / 100 ))
CHURN_MB=$(( FILL_MB * 12 / 10 ))

echo "  채우기: ${FILL_MB} MiB 순차 기록"
t0=$(date +%s)
fio --name=fill --offset=0 --size=${FILL_MB}M --rw=write --bs=1M --iodepth=8 \
    $FIO_COMMON >"$T/fill.txt" 2>&1 \
	&& ok "채우기 완료 ($(( $(date +%s) - t0 ))s)" || bad "채우기 실패"
show_stat

echo "  churn: 같은 ${FILL_MB} MiB 영역에 ${CHURN_MB} MiB(1.2배) random overwrite"
gc_before=$(stat_f 9)
t0=$(date +%s)
if fio --name=churn --offset=0 --size=${FILL_MB}M --io_size=${CHURN_MB}M \
       --rw=randwrite --bs=16k --iodepth=16 \
       $FIO_COMMON >"$T/churn.txt" 2>&1; then
	ok "1.2배 overwrite 를 'no space left' 없이 완주 (M3 성공 기준, $(( $(date +%s) - t0 ))s)"
else
	bad "churn 실패 — fio 에러:"
	grep -iE "error|no space" "$T/churn.txt" | head -5
fi
gc_delta=$(( $(stat_f 9) - gc_before ))
show_stat

[ "$gc_delta" -gt 0 ] \
	&& ok "자동 GC 가 ${gc_delta}회 돌아 zone 을 재활용" \
	|| bad "GC 가 한 번도 안 돌았다"

# churn 뒤 매핑 일관성 — 큰 읽기 == 작은 읽기 (앞 64 MiB 표본)
dd if="$DEV" of="$T/big.bin"   bs=1M   count=64    iflag=direct status=none
dd if="$DEV" of="$T/small.bin" bs=4096 count=16384 iflag=direct status=none
cmp -s "$T/big.bin" "$T/small.bin" \
	&& ok "차등 읽기 일치 — GC 를 거친 매핑이 일관적" \
	|| bad "차등 읽기 불일치: $(cmp "$T/big.bin" "$T/small.bin" 2>&1 | head -1)"
fi

# =====================================================================
step "결과"
echo "  통과 $pass / 실패 $fail"
[ $fail -eq 0 ] && echo "  >>> PHASE 5 (M3) PASSED <<<" || echo "  >>> 실패 항목 있음 <<<"
echo
echo "  회귀도 확인하세요:  sudo bash scripts/phase4-verify.sh && sudo bash scripts/phase3-verify.sh"

step "정리"
bash scripts/teardown.sh

exit $fail
