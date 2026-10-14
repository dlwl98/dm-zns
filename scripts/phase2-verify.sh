#!/usr/bin/env bash
# Phase 2 검증 — 쓴 데이터가 그대로 읽히는가. M1 기준(delta 0)은 읽기를
# 하지 않으므로 읽기 경로는 여기서 본다.
#
#   T1  큰 읽기 == 작은 읽기      (멀티블록 read 매핑)
#   T2  fio --verify=crc32c       (쓰기→읽기 왕복 무결성)
#   T3  타깃 재생성               (메타데이터에서 복구 — 데이터가 남는가)
#   T4  노출 용량 끝까지          (OP 덕에 ENOSPC 없이 두 번 채워지는가)
#
# 사용:  sudo bash scripts/phase2-verify.sh
set -uo pipefail

# shellcheck source=scripts/common.sh
. "$(dirname "$0")/common.sh"

DM_NAME=${DM_NAME:-my-m1-device}
UNDERLYING=${UNDERLYING:-/dev/nullb0}
MOD_KO="src/dm-zns-base.ko"
MOD_NAME="dm_zns_base"
DEV="/dev/mapper/$DM_NAME"
T=/tmp/zns-phase2
SCATTER_MB=32

[ "$(id -u)" -eq 0 ] || { echo "root 로 실행하세요:  sudo bash $0" >&2; exit 1; }
[ -f "$MOD_KO" ] || { echo "$MOD_KO 가 없습니다. 먼저 'cd src && make'." >&2; exit 1; }

mkdir -p "$T"
pass=0; fail=0
ok()   { echo "  [OK]   $*"; pass=$((pass+1)); }
bad()  { echo "  [FAIL] $*"; fail=$((fail+1)); }
step() { echo; echo "=== $* ==="; }

teardown() {
	umount /mnt/x 2>/dev/null
	dmsetup remove "$DM_NAME" 2>/dev/null
	rmmod "$MOD_NAME" 2>/dev/null
	zns_under_down >/dev/null 2>&1
}

create_target() {
	zns_table "$UNDERLYING" format | dmsetup create "$DM_NAME"
}

reopen_target() {
	zns_table "$UNDERLYING" | dmsetup create "$DM_NAME"
}

step "0. 환경 구성"
teardown
zns_under_up >/dev/null || { echo "nullblk-up 실패" >&2; exit 1; }
zns_load_module "$MOD_KO" || exit 1
create_target || { echo "dmsetup create 실패"; dmesg | tail -10; exit 1; }
echo "  $DEV 준비 완료"

# =====================================================================
# T1 — 멀티블록 read 오매핑
#
# 임의 순서로 4 KiB 씩 흩어 쓴 뒤, 같은 영역을 (a) 1 MiB 큰 읽기와
# (b) 4 KiB 작은 읽기로 각각 받아 비교한다.
#
# 4 KiB 읽기는 블록 하나만 건드리므로 항상 옳다. 따라서 둘이 다르면
# 큰 읽기 경로가 틀린 것이다 — 무엇을 썼는지 알 필요가 없는 차등 검사.
# =====================================================================
step "T1. 큰 읽기와 작은 읽기가 같은 내용을 주는가"

fio --name=scatter --filename="$DEV" --rw=randwrite --bs=4k \
    --size=${SCATTER_MB}M --ioengine=libaio --iodepth=32 --direct=1 \
    --minimal >"$T/scatter.txt" 2>&1 \
	|| { bad "흩뿌리기(randwrite) 실패"; tail -3 "$T/scatter.txt"; }

echo "  ${SCATTER_MB} MiB 를 4 KiB 단위 임의 순서로 기록"

dd if="$DEV" of="$T/big.bin"   bs=1M   count=$SCATTER_MB        iflag=direct status=none
dd if="$DEV" of="$T/small.bin" bs=4096 count=$((SCATTER_MB*256)) iflag=direct status=none

if cmp -s "$T/big.bin" "$T/small.bin"; then
	ok "1 MiB 읽기 == 4 KiB 읽기  (멀티블록 경로 정상)"
else
	bad "두 읽기 결과가 다름 — 멀티블록 read 가 여전히 어긋난다"
	echo "         $(cmp "$T/big.bin" "$T/small.bin" 2>&1 | head -1)"
fi

# =====================================================================
# T2 — 쓰기→읽기 왕복 무결성
# =====================================================================
step "T2. fio crc32c 왕복 검증"

if fio --name=vfy --filename="$DEV" --rw=randwrite --bs=4k --size=32M \
       --ioengine=libaio --iodepth=16 --direct=1 \
       --verify=crc32c --do_verify=1 --verify_fatal=1 \
       --minimal >"$T/verify.txt" 2>&1; then
	ok "crc32c 검증 통과 — 쓴 데이터가 그대로 읽힘"
else
	bad "crc32c 검증 실패"
	grep -iE "verify|error|bad" "$T/verify.txt" | head -5
fi

# =====================================================================
# T3 — 타깃 재생성 = 메타데이터 복구
#
# 하위 장치는 그대로 둔 채 dm 타깃만 지웠다 format 없이 다시 만든다.
# ctr 가 메타 zone 에서 매핑을 되살리므로 T2 에서 쓴 데이터가 그대로 읽혀야
# 하고, 하위 wp 에서 이어 쓰므로 새 쓰기도 거절되지 않아야 한다.
# =====================================================================
step "T3. 하위 장치를 살려둔 채 타깃만 재생성 (복구)"

dmsetup remove "$DM_NAME" || bad "dmsetup remove 실패"
reopen_target || { bad "타깃 재생성 실패"; dmesg | tail -5; }
dmesg | grep "zns-base: recover:" | tail -1 | sed 's/^.*zns-base: /  /'

if fio --name=vfy --filename="$DEV" --rw=randwrite --bs=4k --size=32M \
       --ioengine=libaio --iodepth=16 --direct=1 \
       --verify=crc32c --verify_only=1 --verify_fatal=1 \
       --minimal >"$T/reopen-verify.txt" 2>&1; then
	ok "재생성 뒤 T2 데이터가 그대로 읽힘 — 매핑 복구"
else
	bad "재생성 뒤 T2 데이터 검증 실패"
	grep -iE "verify|error|bad" "$T/reopen-verify.txt" | head -5
fi

if dd if=/dev/urandom of="$DEV" bs=4k count=256 oflag=direct status=none 2>"$T/recreate.txt"; then
	ok "재생성 뒤 새 쓰기 정상 — 하위 wp 에서 이어 씀"
else
	bad "재생성 뒤 쓰기 실패"
	cat "$T/recreate.txt"
fi

# =====================================================================
# T4 — 노출 용량 끝까지, 두 번
#     디바이스를 끝까지 채우므로 반드시 마지막에.
#
# 노출 용량이 데이터 풀의 3/4 이하라(over-provisioning) GC 가 고르는 zone
# 에는 늘 비울 자리가 있다. 노출 용량 전체를 두 번 써도 ENOSPC 가 없어야
# 한다. 두 번째는 첫 번째를 전부 덮어써 GC 를 돌린다.
# =====================================================================
step "T4. 노출 용량 전체를 두 번 써도 공간이 막히지 않는가"

if [ "${P2_T4:-1}" = 0 ]; then
	echo "  [SKIP] P2_T4=0"
else
CAP_MB=$(( $(blockdev --getsz "$DEV") / 2048 ))
echo "  노출 용량 ${CAP_MB} MiB (하위 $(( $(blockdev --getsz "$UNDERLYING") / 2048 )) MiB)"

before_oos=$(dmesg | grep -c "zns-base.*out of space")
t0=$(date +%s)
rc=0
for pass in 1 2; do
	fio --name=fill$pass --filename="$DEV" --rw=write --bs=1M \
	    --size=${CAP_MB}M --ioengine=libaio --iodepth=8 --direct=1 \
	    --minimal >"$T/fill$pass.txt" 2>&1 || rc=1
done
after_oos=$(dmesg | grep -c "zns-base.*out of space")
echo "  두 번 채우기 $(( $(date +%s) - t0 ))s, GC $(zns_stat 9 "$DM_NAME")회"

if [ $rc -eq 0 ] && [ $((after_oos - before_oos)) -eq 0 ]; then
	ok "노출 용량 끝까지 두 번 — ENOSPC 없음"
else
	bad "채우기 실패 (rc=$rc, out-of-space 로그 $((after_oos - before_oos)))"
	grep -iE "error|no space" "$T"/fill*.txt | head -3
fi
fi

# =====================================================================
step "결과"
echo "  통과 $pass / 실패 $fail"
[ $fail -eq 0 ] && echo "  >>> PHASE 2 PASSED <<<" || echo "  >>> 실패 항목 있음 <<<"
echo
echo "  정리:  sudo bash scripts/teardown.sh"

exit $fail
