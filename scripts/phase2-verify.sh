#!/usr/bin/env bash
# Phase 2 검증 — 쓴 데이터가 그대로 읽히는가. M1 기준(delta 0)은 읽기를
# 하지 않으므로 읽기 경로는 여기서 본다.
#
#   T1  큰 읽기 == 작은 읽기      (멀티블록 read 매핑)
#   T2  fio --verify=crc32c       (쓰기→읽기 왕복 무결성)
#   T3  타깃 재생성               (ctr 의 zone reset)
#   T4  용량 경계                 (끝을 넘기면 ENOSPC)
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
	local sectors
	sectors=$(blockdev --getsz "$UNDERLYING")
	echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME"
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
# T3 — ctr 의 zone reset
#
# nullb0 는 그대로 둔 채 dm 타깃만 지웠다 다시 만든다. reset 이 없으면
# 모듈의 wp_offset(0) 과 하위의 실제 wp 가 어긋나 첫 쓰기부터 거절당한다.
# =====================================================================
step "T3. nullb0 를 살려둔 채 타깃만 재생성"

dmsetup remove "$DM_NAME" || bad "dmsetup remove 실패"
create_target || { bad "타깃 재생성 실패"; dmesg | tail -5; }

before=$(dmesg | grep -c blk_update_request)
dd if=/dev/urandom of="$DEV" bs=4k count=256 oflag=direct status=none 2>"$T/recreate.txt"
rc=$?
after=$(dmesg | grep -c blk_update_request)

if [ $rc -eq 0 ] && [ $((after - before)) -eq 0 ]; then
	ok "재생성 후 쓰기 정상 (delta 0) — zone reset 이 먹었다"
else
	bad "재생성 후 쓰기 실패 (rc=$rc, delta=$((after-before)))"
	dmesg | grep blk_update_request | tail -3
fi

# =====================================================================
# T4 — 용량 경계
#     디바이스를 끝까지 채우므로 반드시 마지막에.
#
# 노출 용량 = 물리 용량이라, 끝에 닿으면 GC 가 거의 꽉 찬 zone 을 옮기기만
# 하고 공간을 못 만든다. 쓰기 하나가 GC 를 WRITE_GC_RETRIES 번 기다린 뒤
# ENOSPC 가 되고, fio 는 continue_on_error 로 남은 쓰기를 계속 낸다.
# zone 이 작고 I/O 가 빠르면(null_blk) 수 초에 끝나지만, zone 이 크고 GC 가
# 느린 장치에서는 끝나지 않는다 — P2_T4=0 으로 건너뛴다.
# =====================================================================
step "T4. 용량을 넘기면 조용히 새지 않고 끊기는가"

if [ "${P2_T4:-1}" = 0 ]; then
	echo "  [SKIP] P2_T4=0"
else
CAP_MB=$(( $(blockdev --getsz "$UNDERLYING") / 2048 ))
echo "  디바이스 용량 ${CAP_MB} MiB — 넘겨서 써 본다"

before_oos=$(dmesg | grep -c "zns-base.*out of space")
before_blk=$(dmesg | grep -c blk_update_request)

fio --name=fill --filename="$DEV" --rw=write --bs=1M \
    --size=${CAP_MB}M --ioengine=libaio --iodepth=8 --direct=1 \
    --continue_on_error=all --minimal >"$T/fill.txt" 2>&1

after_oos=$(dmesg | grep -c "zns-base.*out of space")
after_blk=$(dmesg | grep -c blk_update_request)

echo "  out-of-space 로그 증가:      $((after_oos - before_oos))"
echo "  blk_update_request 증가:     $((after_blk - before_blk))"

if [ $((after_oos - before_oos)) -gt 0 ]; then
	ok "경계에서 ENOSPC 로 끊었다 (의도된 동작)"
	dmesg | grep "zns-base.*out of space" | tail -1 | sed 's/^/         /'
else
	bad "경계 검사가 걸리지 않았다"
fi
fi

# =====================================================================
step "결과"
echo "  통과 $pass / 실패 $fail"
[ $fail -eq 0 ] && echo "  >>> PHASE 2 PASSED <<<" || echo "  >>> 실패 항목 있음 <<<"
echo
echo "  정리:  sudo bash scripts/teardown.sh"

exit $fail
