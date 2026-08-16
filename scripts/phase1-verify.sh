#!/usr/bin/env bash
# Phase 1 검증 — 커널 6.17 포팅이 6·7월 결과를 재현하는지 확인한다.
#
#   1) teardown (dm 타깃 → null_blk 순서)
#   2) nullblk 기동 + 모듈 적재 + dm 타깃 생성
#   3) 위쪽 면이 conventional 로 광고되는지 확인   ← 이번에 새로 보는 것
#   4) fio 4K randwrite 100MB, blk_update_request 증가분(delta) 측정
#
# 사용:  sudo bash scripts/phase1-verify.sh
set -uo pipefail

DM_NAME=${DM_NAME:-my-m1-device}
UNDERLYING=${UNDERLYING:-/dev/nullb0}
MOD_KO="src/dm-zns-base.ko"
MOD_NAME="dm_zns_base"

[ "$(id -u)" -eq 0 ] || { echo "root 로 실행하세요:  sudo bash $0" >&2; exit 1; }
[ -f "$MOD_KO" ] || { echo "$MOD_KO 가 없습니다. 먼저 'cd src && make'." >&2; exit 1; }

pass=0; fail=0
ok()   { echo "  [OK]   $*"; pass=$((pass+1)); }
bad()  { echo "  [FAIL] $*"; fail=$((fail+1)); }
step() { echo; echo "=== $* ==="; }

# ---------------------------------------------------------------- 1. teardown
# 순서 중요: dm 타깃이 살아 있으면 device mapper 가 하위 디바이스를 계속
# 참조하므로 null_blk 이 내려가지 않는다. (6월 보고서 질문사항)
step "1. 이전 잔재 정리"
umount /mnt/x 2>/dev/null && echo "  umount /mnt/x"
dmsetup remove "$DM_NAME" 2>/dev/null && echo "  dmsetup remove $DM_NAME"
rmmod "$MOD_NAME" 2>/dev/null && echo "  rmmod $MOD_NAME"
bash scripts/nullblk-down.sh >/dev/null 2>&1 && echo "  nullblk down"
echo "  정리 완료"

# ---------------------------------------------------------------- 2. 셋업
step "2. 환경 구성"
bash scripts/nullblk-up.sh || { echo "nullblk-up 실패" >&2; exit 1; }

insmod "$MOD_KO" || { echo "insmod 실패 — dmesg 확인" >&2; exit 1; }
echo "  insmod OK"

SECTORS=$(blockdev --getsz "$UNDERLYING")
echo "0 $SECTORS zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || {
	echo "dmsetup create 실패 — 아래 dmesg 참고" >&2
	dmesg | tail -20
	exit 1
}
echo "  dmsetup create OK  (${SECTORS} sectors)"

DM_DEV=$(basename "$(readlink -f "/dev/mapper/$DM_NAME")")   # dm-0 등
Q="/sys/block/$DM_DEV/queue"
echo "  /dev/mapper/$DM_NAME  ->  /dev/$DM_DEV"

# ------------------------------------------------- 3. 위/아래 면 확인 (핵심)
step "3. 위쪽 면이 conventional 인가"

under_name=$(basename "$UNDERLYING")
echo "  아래(underlying $under_name):  zoned=$(cat /sys/block/$under_name/queue/zoned 2>/dev/null)" \
     " chunk_sectors=$(cat /sys/block/$under_name/queue/chunk_sectors 2>/dev/null)" \
     " nr_zones=$(cat /sys/block/$under_name/queue/nr_zones 2>/dev/null)"

dm_zoned=$(cat "$Q/zoned" 2>/dev/null || echo "<없음>")
dm_chunk=$(cat "$Q/chunk_sectors" 2>/dev/null || echo "<없음>")
dm_nrz=$(cat "$Q/nr_zones" 2>/dev/null || echo "<없음>")
echo "  위(dm  $DM_DEV):         zoned=$dm_zoned chunk_sectors=$dm_chunk nr_zones=$dm_nrz"

if [ "$dm_zoned" = "none" ]; then
	ok "위쪽이 conventional 로 광고됨 (zoned=none)"
else
	bad "위쪽이 아직 zoned 로 보임 (zoned=$dm_zoned) — io_hints 가 안 먹었다"
fi

# ---------------------------------------------------------------- 4. fio
step "4. fio 4K randwrite 100MB"
before=$(dmesg | grep -c blk_update_request)

fio --name=randw --filename="/dev/mapper/$DM_NAME" --rw=randwrite \
    --bs=4k --size=100M --ioengine=libaio --iodepth=32 --direct=1 \
    --minimal >/tmp/phase1-fio.txt 2>&1
fio_rc=$?

after=$(dmesg | grep -c blk_update_request)
delta=$((after - before))

if [ $fio_rc -ne 0 ]; then
	bad "fio 실패 (rc=$fio_rc) — /tmp/phase1-fio.txt 확인"
	tail -5 /tmp/phase1-fio.txt
else
	ok "fio 완주"
fi

echo "  blk_update_request delta: $delta"
if [ "$delta" -eq 0 ]; then
	ok "underlying 이 쓰기를 한 건도 거절하지 않음 (delta 0)"
else
	bad "거절된 쓰기가 있음 (delta $delta)"
	dmesg | grep blk_update_request | tail -5
fi

# ---------------------------------------------------------------- 요약
step "결과"
echo "  통과 $pass / 실패 $fail"
[ $fail -eq 0 ] && echo "  >>> PHASE 1 PASSED <<<" || echo "  >>> 실패 항목 있음 <<<"
echo
echo "  디바이스는 켜둔 채로 둡니다. 직접 볼 것들:"
echo "    sudo dmsetup table $DM_NAME"
echo "    sudo blkzone report $UNDERLYING | head -3"
echo "    sudo dmesg | grep zns-base | tail"
echo
echo "  정리:  sudo bash scripts/teardown.sh"

exit $fail
