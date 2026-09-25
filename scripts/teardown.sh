#!/usr/bin/env bash
# 실험 환경을 정리한다. 여러 번 돌려도 안전하다.
#
# 순서가 중요하다. 위에서부터 참조를 끊어 내려가야 한다:
#
#   mount    파일시스템이 dm 타깃을 잡고 있다
#     ↓
#   dm 타깃  device mapper 가 /dev/nullb0 을 잡고 있다
#     ↓
#   null_blk
#
# 거꾸로 하면 커널이 "사용 중"으로 거부하고, null_blk 이 어중간하게 남아
# 환경을 다시 만들 수 없게 된다.
#
# 사용:  sudo bash scripts/teardown.sh
set -uo pipefail

DM_NAME=${DM_NAME:-my-m1-device}
NB_NAME=${NB_NAME:-nullb0}
MOD_NAME=${MOD_NAME:-dm_zns_base}
MNT=${MNT:-/mnt/x}

[ "$(id -u)" -eq 0 ] || { echo "root 로 실행하세요:  sudo bash $0" >&2; exit 1; }

did() { echo "  [정리] $*"; }
skip(){ echo "  [건너뜀] $*"; }
warn(){ echo "  [남음] $*"; }

echo "=== 1. 마운트 ==="
if mountpoint -q "$MNT" 2>/dev/null; then
	if umount "$MNT"; then
		did "umount $MNT"
	else
		warn "$MNT 를 못 내렸다 — 쓰는 프로세스가 있다:"
		fuser -vm "$MNT" 2>&1 | sed 's/^/         /'
		exit 1
	fi
else
	skip "$MNT 는 마운트돼 있지 않음"
fi

echo "=== 2. dm 타깃 ==="
if dmsetup info "$DM_NAME" >/dev/null 2>&1; then
	if dmsetup remove "$DM_NAME"; then
		did "dmsetup remove $DM_NAME"
	else
		warn "$DM_NAME 을 못 지웠다 — 아직 열려 있는 곳이 있다:"
		dmsetup info "$DM_NAME" | sed 's/^/         /'
		exit 1
	fi
else
	skip "dm 타깃 $DM_NAME 없음"
fi

echo "=== 3. dm 모듈 ==="
if [ -d "/sys/module/$MOD_NAME" ]; then
	# dm 타깃 해제 직후에는 모듈 참조가 아직 안 떨어져 있을 수 있어
	# 곧바로 rmmod 하면 EBUSY 가 난다. 몇 번 다시 시도한다.
	rm_done=0
	for i in $(seq 1 10); do
		rmmod "$MOD_NAME" 2>/dev/null
		# rmmod 의 반환값보다 실제로 사라졌는지를 본다 —
		# 내려가는 중에는 반환값과 상태가 어긋날 수 있다.
		if [ ! -d "/sys/module/$MOD_NAME" ]; then
			did "rmmod $MOD_NAME"
			rm_done=1
			break
		fi
		sleep 0.3
	done
	if [ $rm_done -eq 0 ]; then
		warn "rmmod $MOD_NAME 실패 — 아직 쓰는 타깃이 있습니다:"
		dmsetup ls 2>&1 | sed 's/^/         /'
		echo "         (이 상태로 insmod 하면 예전 모듈이 그대로 쓰입니다)"
	fi
else
	skip "모듈 $MOD_NAME 적재돼 있지 않음"
fi

echo "=== 4. null_blk 인스턴스 ==="
CONFIG="/sys/kernel/config/nullb/$NB_NAME"
if [ -d "$CONFIG" ]; then
	echo 0 > "$CONFIG/power" 2>/dev/null || true
	if rmdir "$CONFIG" 2>/dev/null; then
		did "$NB_NAME 제거 (configfs)"
	else
		warn "$CONFIG 를 못 지웠다"
	fi
else
	skip "$NB_NAME 인스턴스 없음"
fi

echo "=== 5. null_blk 모듈 ==="
# 인스턴스 제거가 비동기라 곧바로 rmmod 하면 refcount 가 아직 안 떨어져
# 있을 수 있다. 잠깐 기다렸다 다시 시도한다.
if lsmod | grep -q '^null_blk '; then
	remaining=$(ls /sys/kernel/config/nullb/ 2>/dev/null)
	if [ -n "$remaining" ]; then
		skip "다른 인스턴스가 남아 있어 유지: $remaining"
	else
		for i in 1 2 3; do
			if rmmod null_blk 2>/dev/null; then
				did "rmmod null_blk"
				break
			fi
			[ $i -eq 3 ] && warn "null_blk 모듈이 남아 있다 (refcount: $(lsmod | awk '/^null_blk /{print $3}'))" \
			             || sleep 1
		done
	fi
else
	skip "null_blk 적재돼 있지 않음"
fi

echo
echo "=== 남은 것 확인 ==="
left=0
mountpoint -q "$MNT" 2>/dev/null && { warn "$MNT 마운트됨"; left=1; }
dmsetup ls 2>/dev/null | grep -qv "No devices found" && \
	dmsetup ls 2>/dev/null | grep -q . && { echo "  dm 타깃:"; dmsetup ls | sed 's/^/         /'; }
for m in dm_zns_base null_blk; do
	[ -d "/sys/module/$m" ] && echo "  모듈: $m (refcnt $(cat /sys/module/$m/refcnt 2>/dev/null))"
done
ls /dev/nullb* 2>/dev/null | sed 's/^/  디바이스: /'
[ $left -eq 0 ] && echo "  (문제되는 잔재 없음)"

echo
echo "  null_blk 모듈이 남아 있어도 무해합니다 — nullblk-up.sh 가 이미 적재된"
echo "  모듈을 그대로 재사용합니다. 완전히 내리려면: sudo rmmod null_blk"
