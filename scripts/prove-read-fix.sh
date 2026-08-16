#!/usr/bin/env bash
# T1(큰 읽기 == 작은 읽기)이 실제로 결함을 잡아내는 검사인지 확인한다.
#
# 통과만 보고 "고쳤다" 고 말할 수는 없다. 그 테스트가 애초에 버그를
# 검출할 수 있는지를 보여야 한다. 그래서 같은 검사를 두 번 돌린다:
#
#   수정 전 모듈 → 불일치가 나와야 한다 (검출력 있음)
#   수정 후 모듈 → 일치해야 한다       (수정 유효)
#
# 사용:  sudo bash scripts/prove-read-fix.sh [수정전_모듈_경로]
set -uo pipefail

# shellcheck source=scripts/common.sh
. "$(dirname "$0")/common.sh"

DM_NAME=${DM_NAME:-my-m1-device}
UNDERLYING=${UNDERLYING:-/dev/nullb0}
MOD_NAME="dm_zns_base"
DEV="/dev/mapper/$DM_NAME"
T=/tmp/zns-prove
MB=32

PRE_KO=${1:-/tmp/claude-1000/-home-jinuk----jp/791603d2-b306-44fb-ad78-6186552da63c/scratchpad/pre2/dm-zns-base.ko}
POST_KO="src/dm-zns-base.ko"

[ "$(id -u)" -eq 0 ] || { echo "root 로 실행하세요:  sudo bash $0" >&2; exit 1; }
[ -f "$PRE_KO" ]  || { echo "수정 전 모듈이 없습니다: $PRE_KO" >&2; exit 1; }
[ -f "$POST_KO" ] || { echo "수정 후 모듈이 없습니다: $POST_KO" >&2; exit 1; }

mkdir -p "$T"

teardown() {
	dmsetup remove "$DM_NAME" 2>/dev/null
	rmmod "$MOD_NAME" 2>/dev/null
	bash scripts/nullblk-down.sh >/dev/null 2>&1
}

# 모듈 하나를 적재해 흩뿌린 뒤, 큰 읽기와 작은 읽기를 비교한다.
# 반환: 0 = 일치, 1 = 불일치
run_diff_check() {
	local ko="$1" tag="$2" sectors

	teardown
	bash scripts/nullblk-up.sh >/dev/null || return 2
	zns_load_module "$ko" || return 2
	sectors=$(blockdev --getsz "$UNDERLYING")
	echo "0 $sectors zns-base $UNDERLYING" | dmsetup create "$DM_NAME" || return 2

	fio --name=scatter --filename="$DEV" --rw=randwrite --bs=4k \
	    --size=${MB}M --ioengine=libaio --iodepth=32 --direct=1 \
	    --randseed=12345 --minimal >"$T/$tag-fio.txt" 2>&1

	dd if="$DEV" of="$T/$tag-big.bin"   bs=1M   count=$MB          iflag=direct status=none
	dd if="$DEV" of="$T/$tag-small.bin" bs=4096 count=$((MB*256))  iflag=direct status=none

	cmp -s "$T/$tag-big.bin" "$T/$tag-small.bin"
}

echo "=== 1. 수정 전 모듈 — T1 이 결함을 잡아내는가 ==="
echo "    $PRE_KO"
if run_diff_check "$PRE_KO" pre; then
	echo "  [FAIL] 수정 전인데도 일치했다."
	echo "         → T1 이 이 결함을 검출하지 못한다는 뜻이므로,"
	echo "            Phase 2 의 T1 통과는 근거가 되지 못한다."
	verdict_pre=1
else
	echo "  [OK]   불일치 검출 — $(cmp "$T/pre-big.bin" "$T/pre-small.bin" 2>&1 | head -1)"
	echo "         T1 은 이 결함을 실제로 잡아내는 검사다."
	verdict_pre=0
fi

echo
echo "=== 2. 수정 후 모듈 — 같은 검사를 통과하는가 ==="
echo "    $POST_KO"
if run_diff_check "$POST_KO" post; then
	echo "  [OK]   일치 — 멀티블록 읽기가 올바른 데이터를 준다."
	verdict_post=0
else
	echo "  [FAIL] 여전히 불일치 — $(cmp "$T/post-big.bin" "$T/post-small.bin" 2>&1 | head -1)"
	verdict_post=1
fi

echo
echo "=== 결론 ==="
if [ $verdict_pre -eq 0 ] && [ $verdict_post -eq 0 ]; then
	echo "  검사에 검출력이 있고(수정 전 실패), 수정이 그것을 없앴다(수정 후 통과)."
	echo "  >>> 읽기 경로 수정이 유효함 <<<"
	rc=0
else
	echo "  >>> 결론 보류 — 위 항목 확인 필요 <<<"
	rc=1
fi

echo
echo "  정리:  sudo bash scripts/teardown.sh"
exit $rc
