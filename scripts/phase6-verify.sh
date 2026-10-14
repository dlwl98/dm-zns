#!/usr/bin/env bash
# Phase 6 — 메타데이터 영속화와 크래시 일관성.
#
#   T1  정상 종료 뒤 복구     ext4 파일 → umount → 타깃 제거·재생성 → md5 + e2fsck
#   T2  fsync 뒤 크래시       fsync 한 파일은 남고, 저널 재생 뒤 파일시스템 정상
#   T3  부하 중 크래시 반복   ext4 위 동시 쓰기 + fsync 도중 임의 시점 크래시 (P6_ITERS 회)
#   T4  GC 직후 크래시        이주된 블록이 남는가 — reset 전 커밋
#   T5  복구 시간 / 매핑 메모리  로그만 / 체크포인트 직후
#
# 크래시는 'dmsetup message <dev> 0 crash' 로 흉내 낸다: 그 순간부터 커밋과
# I/O 를 모두 버린다. 이미 보낸 쓰기는 장치에 닿을 수 있다(전원 차단과 같다).
# 복구 뒤 "mappings to unwritten space" 가 0 이어야 한다 — 매핑이 쓰이지 않은
# 위치를 가리키면 크래시 일관성이 깨진 것이다.
#
# 사용:  sudo bash scripts/phase6-verify.sh
#        sudo UNDERLYING=/dev/nvme0n1 P6_ITERS=5 bash scripts/phase6-verify.sh
set -uo pipefail

# shellcheck source=scripts/common.sh
. "$(dirname "$0")/common.sh"

DM_NAME=${DM_NAME:-my-m1-device}
UNDERLYING=${UNDERLYING:-/dev/nullb0}
MOD_KO="src/dm-zns-base.ko"
DEV="/dev/mapper/$DM_NAME"
MNT=/mnt/zns6
T=/tmp/zns-phase6
ITERS=${P6_ITERS:-10}

[ "$(id -u)" -eq 0 ] || { echo "root 로 실행하세요:  sudo bash $0" >&2; exit 1; }
[ -f "$MOD_KO" ] || { echo "$MOD_KO 가 없습니다. 먼저 'cd src && make'." >&2; exit 1; }

mkdir -p "$T" "$MNT"
pass=0; fail=0
ok()   { echo "  [OK]   $*"; pass=$((pass+1)); }
bad()  { echo "  [FAIL] $*"; fail=$((fail+1)); }
step() { echo; echo "=== $* ==="; }

st() { zns_stat "$1" "$DM_NAME"; }

fresh() {
	mountpoint -q "$MNT" && umount "$MNT"
	bash scripts/teardown.sh >/dev/null 2>&1
	zns_under_up >/dev/null || return 1
	zns_load_module "$MOD_KO" || return 1
	zns_create_target "$DM_NAME" "$UNDERLYING"
}

# 크래시 흉내 뒤: 파일시스템을 떼고 타깃을 내린다 (dtr 는 커밋하지 않는다)
down_after_crash() {
	local i

	if mountpoint -q "$MNT"; then
		umount "$MNT" 2>/dev/null || umount -l "$MNT"
	fi
	for i in 1 2 3 4 5 6 7 8 9 10; do
		dmsetup remove "$DM_NAME" 2>/dev/null && return 0
		sleep 1
	done
	return 1
}

# 다시 만들고(복구) 그 사이 커널 로그를 $T/reopen.log 에 남긴다.
# 복구가 쓰이지 않은 위치를 가리키는 매핑을 찾았으면 실패.
reopen() {
	local n0 rc bad_map

	n0=$(dmesg | wc -l)
	zns_reopen_target "$DM_NAME" "$UNDERLYING"
	rc=$?
	dmesg | tail -n +$((n0 + 1)) >"$T/reopen.log"
	[ $rc -eq 0 ] || return 1
	bad_map=$(sed -nE 's/.* ([0-9]+) mappings to unwritten space.*/\1/p' "$T/reopen.log" | tail -1)
	[ "${bad_map:-0}" -eq 0 ]
}

recover_line() { grep "recover:" "$T/reopen.log" | tail -1 | sed 's/^.*zns-base: //'; }

# =====================================================================
step "T1. 정상 종료 뒤 복구 — ext4 파일이 그대로인가"
# =====================================================================
fresh || { echo "환경 구성 실패"; exit 1; }
echo "  노출 $(( $(blockdev --getsz "$DEV") / 2048 )) MiB / 하위 $(( $(blockdev --getsz "$UNDERLYING") / 2048 )) MiB"

mkfs.ext4 -q -F "$DEV" || bad "mkfs 실패"
mount "$DEV" "$MNT" || bad "mount 실패"
for i in 1 2 3 4; do
	dd if=/dev/urandom of="$MNT/f$i" bs=1M count=32 status=none
done
(cd "$MNT" && md5sum f1 f2 f3 f4) >"$T/t1.md5"
umount "$MNT"
dmsetup remove "$DM_NAME" || bad "dmsetup remove 실패"

if reopen; then
	echo "  $(recover_line)"
	mount "$DEV" "$MNT" || bad "복구 뒤 mount 실패"
	(cd "$MNT" && md5sum -c --quiet "$T/t1.md5") \
		&& ok "복구 뒤 파일 4개 md5 일치" || bad "md5 불일치"
	umount "$MNT"
	e2fsck -fn "$DEV" >"$T/t1-fsck.txt" 2>&1 \
		&& ok "e2fsck 정상" || { bad "e2fsck 오류"; tail -5 "$T/t1-fsck.txt"; }
else
	bad "복구 실패"
	cat "$T/reopen.log"
fi

# =====================================================================
step "T2. fsync 한 파일은 크래시 뒤에도 남는가"
# =====================================================================
mount "$DEV" "$MNT" || bad "mount 실패"
dd if=/dev/urandom of="$MNT/durable" bs=1M count=64 conv=fsync status=none
(cd "$MNT" && md5sum durable) >"$T/t2.md5"
dd if=/dev/urandom of="$MNT/volatile" bs=1M count=64 status=none	# fsync 없음
dmsetup message "$DM_NAME" 0 crash >/dev/null
down_after_crash || bad "크래시 뒤 타깃을 내리지 못함"

if reopen; then
	echo "  $(recover_line)"
	if mount "$DEV" "$MNT"; then
		(cd "$MNT" && md5sum -c --quiet "$T/t2.md5") \
			&& ok "fsync 한 파일 그대로 (저널 재생 뒤)" || bad "fsync 한 파일 손상"
		echo "  fsync 안 한 파일: $(stat -c %s "$MNT/volatile" 2>/dev/null || echo 없음) bytes"
		umount "$MNT"
		e2fsck -fn "$DEV" >"$T/t2-fsck.txt" 2>&1 \
			&& ok "e2fsck 정상" || { bad "e2fsck 오류"; tail -5 "$T/t2-fsck.txt"; }
	else
		bad "크래시 뒤 mount 실패"
	fi
else
	bad "크래시 뒤 복구 실패"
	cat "$T/reopen.log"
fi

# =====================================================================
step "T3. 쓰는 도중 크래시 ${ITERS}회 — 매번 파일시스템 정상, fsync 한 것은 남는가"
# 매 회: mount → 증인 파일(fsync) → 4 스레드 무작위 쓰기 + 주기 fsync →
# 2~5초 뒤 크래시 → 복구 → mount(저널 재생) → 증인 md5 → e2fsck.
# =====================================================================
fresh || { echo "환경 구성 실패"; exit 1; }
mkfs.ext4 -q -F "$DEV" || bad "mkfs 실패"
: >"$T/t3.md5"
: >"$T/t3-recover.txt"
done_it=0
for it in $(seq 1 "$ITERS"); do
	mount "$DEV" "$MNT" || { bad "반복 $it: mount 실패"; break; }
	dd if=/dev/urandom of="$MNT/w$it" bs=1M count=8 conv=fsync status=none
	(cd "$MNT" && md5sum "w$it") >>"$T/t3.md5"

	fio --name=load --directory="$MNT" --rw=randwrite --bs=4k-64k --size=48M \
	    --numjobs=4 --ioengine=psync --fsync=16 --time_based --runtime=30 \
	    --group_reporting --minimal >/dev/null 2>&1 &
	fpid=$!
	sleep $(( RANDOM % 4 + 2 ))
	dmsetup message "$DM_NAME" 0 crash >/dev/null
	wait $fpid
	down_after_crash || { bad "반복 $it: 크래시 뒤 타깃을 내리지 못함"; break; }

	if ! reopen; then
		bad "반복 $it: 복구 실패 또는 쓰이지 않은 위치를 가리키는 매핑"
		cat "$T/reopen.log"
		break
	fi
	echo "$it $(recover_line)" >>"$T/t3-recover.txt"
	mount "$DEV" "$MNT" || { bad "반복 $it: 크래시 뒤 mount 실패"; break; }
	if ! (cd "$MNT" && md5sum -c --quiet "$T/t3.md5"); then
		bad "반복 $it: fsync 한 증인 파일 손상"
		umount "$MNT"
		break
	fi
	umount "$MNT"
	if ! e2fsck -fn "$DEV" >"$T/t3-fsck-$it.txt" 2>&1; then
		bad "반복 $it: e2fsck 오류"
		tail -5 "$T/t3-fsck-$it.txt"
		break
	fi
	done_it=$it
done
echo "  GC $(st 9)회 (마지막 생성 이후), 커밋 $(st 22)회"
tail -1 "$T/t3-recover.txt" | sed 's/^/  마지막 복구: /'
[ "$done_it" -eq "$ITERS" ] \
	&& ok "크래시 ${ITERS}회 모두 복구 · 저널 재생 · e2fsck 정상, 증인 파일 ${ITERS}개 보존"

# =====================================================================
step "T4. GC 직후 크래시 — 이주된 블록이 남는가"
# phase5 T1 과 같은 배치로 zone0 에 살아 있는 앵커 절반을 만든 뒤 gc 0.
# 이주 뒤 flush 없이 곧바로 크래시한다. GC 가 reset 전에 커밋하지 않았다면
# 앵커의 매핑은 reset 된 zone0 을 가리켜 검증이 깨진다.
# =====================================================================
fresh || { echo "환경 구성 실패"; exit 1; }
ZONE_MB=$(zns_zone_mb)
HALF_MB=$(( ZONE_MB / 2 ))
FIO_COMMON="--filename=$DEV --direct=1 --ioengine=libaio --minimal"
ANCHOR_JOB="--name=anchor --offset=${HALF_MB}M --size=${HALF_MB}M --rw=write --bs=4k \
            --iodepth=16 --verify=crc32c --randseed=777"

fio --name=x --offset=0 --size=${HALF_MB}M --rw=write --bs=1M --iodepth=8 \
    $FIO_COMMON >"$T/x1.txt" 2>&1 || bad "X 기록 실패"
fio $ANCHOR_JOB --do_verify=0 $FIO_COMMON >"$T/anchor-w.txt" 2>&1 || bad "앵커 기록 실패"
fio --name=x2 --offset=0 --size=${HALF_MB}M --rw=write --bs=1M --iodepth=8 \
    $FIO_COMMON >"$T/x2.txt" 2>&1 || bad "X 재기록 실패"

mig0=$(st 10)
msg=$(dmsetup message "$DM_NAME" 0 gc 0 2>&1)
mig=$(( $(st 10) - mig0 ))
echo "  gc 0 → ${msg}, 이주 ${mig} 블록"
dmsetup message "$DM_NAME" 0 crash >/dev/null
down_after_crash || bad "크래시 뒤 타깃을 내리지 못함"

if reopen; then
	echo "  $(recover_line)"
	if fio $ANCHOR_JOB --verify_only=1 $FIO_COMMON >"$T/anchor-v.txt" 2>&1; then
		ok "이주된 앵커 ${mig} 블록이 크래시 뒤에도 그대로"
	else
		bad "앵커 검증 실패 — GC 이주가 커밋되지 않았다"
		grep -iE "verify|error|bad" "$T/anchor-v.txt" | head -3
	fi
else
	bad "복구 실패"
	cat "$T/reopen.log"
fi

# =====================================================================
step "T5. 복구 시간과 매핑 메모리"
# 4 KiB 무작위 쓰기 W 만큼 → 레코드 W/4KiB 개. 정상 종료 뒤 재생성(로그 재생),
# 체크포인트 뒤 재생성(체크포인트만 읽기) 시간을 잰다.
# =====================================================================
CAP_MB=$(zns_cap_mb)
SIZES=${P6_SIZES:-"$(( CAP_MB / 16 )) $(( CAP_MB / 4 )) $(( CAP_MB * 3 / 4 ))"}
printf "  %-9s %-9s %-11s %-10s %-12s %-12s\n" "쓴 양" "로그블록" "인덱스KiB" "링KiB" "복구(로그)" "복구(체크)" | tee "$T/recovery.txt"
for W in $SIZES; do
	fresh || { echo "환경 구성 실패"; exit 1; }
	fio --name=rnd --offset=0 --size=${W}M --rw=randwrite --bs=4k --iodepth=32 \
	    --end_fsync=1 $FIO_COMMON >"$T/rnd-$W.txt" 2>&1 || bad "${W} MiB 쓰기 실패"
	logb=$(st 21); idx=$(st 25); ring=$(st 26)
	dmsetup remove "$DM_NAME"
	reopen || { bad "${W} MiB: 복구 실패"; cat "$T/reopen.log"; continue; }
	r_log=$(st 27)
	dmsetup message "$DM_NAME" 0 checkpoint >/dev/null
	dmsetup remove "$DM_NAME"
	reopen || { bad "${W} MiB: 체크포인트 뒤 복구 실패"; cat "$T/reopen.log"; continue; }
	r_ck=$(st 27)
	printf "  %-9s %-9s %-11s %-10s %-12s %-12s\n" "${W}MiB" "$logb" "$idx" "$ring" "${r_log}ms" "${r_ck}ms" | tee -a "$T/recovery.txt"
done
ok "복구 시간 측정 완료 ($T/recovery.txt)"

# =====================================================================
step "결과"
echo "  통과 $pass / 실패 $fail"
[ $fail -eq 0 ] && echo "  >>> PHASE 6 PASSED <<<" || echo "  >>> 실패 항목 있음 <<<"

step "정리"
mountpoint -q "$MNT" && umount "$MNT"
bash scripts/teardown.sh

exit $fail
