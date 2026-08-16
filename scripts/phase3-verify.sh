#!/usr/bin/env bash
# Phase 3 / M2 — ext4 라운드트립.
#
# M1 위에 실제 파일시스템을 얹는다. 코드를 더하는 단계라기보다,
# M1 이 진짜로 맞게 동작하는지 파일시스템으로 검증하는 단계다.
#
#   umount / remount 사이에 md5 를 비교하는 이유는 페이지 캐시 우회다.
#   안 그러면 DM 을 거치지 않고 RAM 에서 곧장 읽혀 "정상"이라는
#   잘못된 결론이 난다.
#
# 사용:  sudo bash scripts/phase3-verify.sh
set -uo pipefail

# shellcheck source=scripts/common.sh
. "$(dirname "$0")/common.sh"

DM_NAME=${DM_NAME:-my-m1-device}
UNDERLYING=${UNDERLYING:-/dev/nullb0}
MOD_KO="src/dm-zns-base.ko"
MOD_NAME="dm_zns_base"
DEV="/dev/mapper/$DM_NAME"
MNT=/mnt/x
T=/tmp/zns-phase3

[ "$(id -u)" -eq 0 ] || { echo "root 로 실행하세요:  sudo bash $0" >&2; exit 1; }
[ -f "$MOD_KO" ] || { echo "$MOD_KO 가 없습니다. 먼저 'cd src && make'." >&2; exit 1; }

mkdir -p "$T" "$MNT"
pass=0; fail=0
ok()   { echo "  [OK]   $*"; pass=$((pass+1)); }
bad()  { echo "  [FAIL] $*"; fail=$((fail+1)); }
step() { echo; echo "=== $* ==="; }

# 소모량 — GC 가 없으므로 wp 는 되돌아오지 않는다.
usage() {
	local s wp tot
	s=$(dmsetup status "$DM_NAME" 2>/dev/null) || { echo "?"; return; }
	wp=$(echo "$s"  | awk '{print $4}')
	tot=$(echo "$s" | awk '{print $5}')
	if [ -n "${wp:-}" ] && [ -n "${tot:-}" ] && [ "$tot" -gt 0 ]; then
		printf "%d MiB / %d MiB (%d%%)" $((wp/2048)) $((tot/2048)) $((wp*100/tot))
	else
		echo "?"
	fi
}

# 정리 순서: mount → dm 타깃 → null_blk.  거꾸로 하면 "사용 중"으로 거부된다.
teardown() {
	umount "$MNT" 2>/dev/null
	dmsetup remove "$DM_NAME" 2>/dev/null
	rmmod "$MOD_NAME" 2>/dev/null
	bash scripts/nullblk-down.sh >/dev/null 2>&1
}

step "0. 환경 구성"
teardown
bash scripts/nullblk-up.sh >/dev/null || { echo "nullblk-up 실패" >&2; exit 1; }
zns_load_module "$MOD_KO" || exit 1
SECTORS=$(blockdev --getsz "$UNDERLYING")
echo "0 $SECTORS zns-base $UNDERLYING" | dmsetup create "$DM_NAME" \
	|| { echo "dmsetup create 실패"; dmesg | tail -10; exit 1; }
echo "  $DEV 준비  (소모 $(usage))"

# ---------------------------------------------------------------- mkfs
step "1. mkfs.ext4"
# lazy init 을 끄면 mkfs 시점에 전부 기록되어 소모량이 결정적이 된다.
# (켜두면 mount 후 커널이 백그라운드로 마저 쓴다.)
if mkfs.ext4 -q -F -E lazy_itable_init=0,lazy_journal_init=0 "$DEV" >"$T/mkfs.txt" 2>&1; then
	ok "mkfs.ext4 성공  (소모 $(usage))"
else
	bad "mkfs.ext4 실패"
	tail -5 "$T/mkfs.txt"
	dmesg | grep -E "zns-base|blk_update_request" | tail -5
	teardown; exit 1
fi

# ---------------------------------------------------------------- 쓰기
step "2. 마운트 후 데이터 기록"
mount "$DEV" "$MNT" || { bad "mount 실패"; dmesg | tail -5; teardown; exit 1; }
ok "mount 성공"

dd if=/dev/urandom of="$MNT/a.bin" bs=1M count=20 status=none
dd if=/dev/urandom of="$MNT/b.bin" bs=1M count=20 status=none
dd if=/dev/urandom of="$MNT/c.bin" bs=1M count=10 status=none

# 작은 파일 다수 — 메타데이터·저널 경로를 함께 지나가게 한다
mkdir -p "$MNT/many"
for i in $(seq 1 100); do
	dd if=/dev/urandom of="$MNT/many/f$i" bs=4k count=1 status=none
done

sync
echo "  50 MiB(큰 파일 3개) + 100개 작은 파일 기록  (소모 $(usage))"

# 해시 A — 아직 페이지 캐시에 있을 수 있다
( cd "$MNT" && find . -type f | sort | xargs md5sum ) >"$T/hash-A.txt"
echo "  해시 A: $(wc -l <"$T/hash-A.txt") 개 파일"

# ---------------------------------------------------------------- 왕복
step "3. umount → 재mount (페이지 캐시 우회)"
umount "$MNT" || { bad "umount 실패"; teardown; exit 1; }
ok "umount 성공"

if command -v e2fsck >/dev/null; then
	if e2fsck -fn "$DEV" >"$T/fsck.txt" 2>&1; then
		ok "e2fsck — 파일시스템 일관성 정상"
	else
		bad "e2fsck 가 오류를 보고했다"
		tail -12 "$T/fsck.txt"
	fi
fi

mount "$DEV" "$MNT" || { bad "재mount 실패"; dmesg | tail -5; teardown; exit 1; }
ok "재mount 성공"

( cd "$MNT" && find . -type f | sort | xargs md5sum ) >"$T/hash-B.txt"

step "4. 해시 비교"
if diff -q "$T/hash-A.txt" "$T/hash-B.txt" >/dev/null; then
	ok "A == B — 쓴 데이터가 그대로 돌아왔다 (M2 성공 기준)"
else
	bad "해시 불일치"
	diff "$T/hash-A.txt" "$T/hash-B.txt" | head -10
fi

# ---------------------------------------------------------------- 마무리
step "결과"
echo "  최종 소모: $(usage)"
echo "  통과 $pass / 실패 $fail"
[ $fail -eq 0 ] && echo "  >>> M2 PASSED <<<" || echo "  >>> 실패 항목 있음 <<<"

echo
echo "  GC 가 없으므로 위 소모량은 되돌아오지 않는다. 논리적으로 같은 자리를"
echo "  덮어써도 물리 공간은 계속 줄어든다 — M3 이 회수할 몫이다."

step "정리"
bash scripts/teardown.sh

exit $fail
