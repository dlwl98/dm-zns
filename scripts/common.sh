# 검증 스크립트들이 함께 쓰는 helper.  source 해서 쓴다.
# shellcheck shell=bash

# 모듈을 확실히 "이 .ko 로" 올린다.
#
# 이미 올라가 있는 다른 빌드로 시험하지 않도록, 적재된 모듈의 srcversion 이
# 이 .ko 와 같을 때만 성공한다.
zns_load_module() {
	local ko=$1
	local name=${2:-dm_zns_base}
	local sys="/sys/module/${name//-/_}"
	local i want got

	want=$(modinfo -F srcversion "$ko" 2>/dev/null)
	got=$(cat "$sys/srcversion" 2>/dev/null)

	# 이미 올라와 있는 것이 바로 이 .ko 라면 그대로 쓴다.
	# 모듈에 전역 상태가 없으므로(상태는 전부 타깃별) 다시 올릴 필요가 없다.
	if [ -n "$want" ] && [ "$got" = "$want" ]; then
		return 0
	fi

	# 다른 빌드가 올라와 있다면 반드시 내린다.
	if [ -d "$sys" ]; then
		for i in $(seq 1 10); do
			rmmod "$name" 2>/dev/null
			[ -d "$sys" ] || break
			sleep 0.3
		done
	fi

	if [ -d "$sys" ]; then
		echo "[!] $name 을 내리지 못했습니다 (refcnt $(cat "$sys/refcnt" 2>/dev/null))." >&2
		echo "    아직 쓰는 타깃:" >&2
		dmsetup ls 2>&1 | sed 's/^/      /' >&2
		return 1
	fi

	# 방금 내린 직후에는 커널이 같은 이름을 잠시 더 붙들고 있어
	# insmod 가 EEXIST 로 튕길 수 있다. 몇 번 더 시도한다.
	for i in $(seq 1 10); do
		insmod "$ko" 2>/dev/null && break
		sleep 0.3
	done

	got=$(cat "$sys/srcversion" 2>/dev/null)
	if [ -z "$got" ]; then
		echo "[!] insmod $ko 실패:" >&2
		insmod "$ko" 2>&1 | sed 's/^/      /' >&2
		return 1
	fi
	if [ -n "$want" ] && [ "$got" != "$want" ]; then
		echo "[!] 적재된 모듈이 $ko 와 다릅니다 (srcversion $got != $want)" >&2
		return 1
	fi
	return 0
}

# dmsetup status 필드 뽑기.
#   $4 wp  $5 total  $6 mem_n  $7 nr_runs  $8 flushes  $9 compactions  $10 dropped
zns_stat() {
	dmsetup status "${2:-my-m1-device}" 2>/dev/null | awk -v n="$1" '{print $n}'
}

# ── 하위 zoned 장치 준비/정리 ──────────────────────────────────────────
#
# UNDERLYING 이 null_blk(nullb*) 이면 configfs 로 만들고 지운다.
# 그 외 실제 zoned 장치(예: /dev/nvme0n1)는 host-managed 인지 확인하고
# I/O 스케줄러만 맞춘다. 타깃을 format 으로 만들면 전체 zone 을 reset
# 하므로 남은 데이터는 상관없다.
#
#   null_blk:  sudo bash scripts/phase1-verify.sh
#   실제 장치: sudo UNDERLYING=/dev/nvme0n1 bash scripts/phase1-verify.sh
zns_is_nullblk() {
	case "$(basename "${1:-${UNDERLYING:-/dev/nullb0}}")" in
	nullb*) return 0 ;;
	esac
	return 1
}

zns_under_up() {
	local dev=${UNDERLYING:-/dev/nullb0}
	local q="/sys/block/$(basename "$dev")/queue"

	if zns_is_nullblk "$dev"; then
		bash scripts/nullblk-up.sh
		return
	fi

	[ -b "$dev" ] || { echo "[!] $dev 가 없습니다" >&2; return 1; }
	[ "$(cat "$q/zoned" 2>/dev/null)" = "host-managed" ] \
		|| { echo "[!] $dev 는 host-managed zoned 장치가 아닙니다" >&2; return 1; }

	# 5.15 에서 zone 별 쓰기 순서를 블록 계층이 지켜주는 장치는 mq-deadline
	# 의 zone write lock 뿐이다. 스케줄러가 none 이면 하드웨어 큐가 여럿일
	# 때 순서대로 보낸 쓰기도 도착 순서가 바뀔 수 있다.
	# sysfs 값이라 부팅마다 초기화되므로 매번 여기서 맞춘다.
	if ! grep -q '\[mq-deadline\]' "$q/scheduler"; then
		echo mq-deadline >"$q/scheduler" \
			|| { echo "[!] $dev 스케줄러를 mq-deadline 으로 못 바꿨다" >&2; return 1; }
	fi
	echo "[*] $dev: host-managed, $(cat "$q/nr_zones") zones x" \
	     "$(( $(cat "$q/chunk_sectors") / 2048 )) MiB, scheduler" \
	     "$(grep -o '\[[^]]*\]' "$q/scheduler")"
}

# null_blk 일 때만 실제로 내린다. 아무것도 안 했으면 1.
zns_under_down() {
	zns_is_nullblk || return 1
	bash scripts/nullblk-down.sh
}

# 노출(논리) 크기, 섹터. ctr 의 over-provisioning 검사와 같은 식이다:
#   데이터 zone = 전체 - 메타 2, 풀 = 데이터 - 활성 2,
#   노출 ≤ 풀 × zone × ZNS_MAX_UTIL/1000 (기본 750)
zns_logical_sectors() {
	local under=${1:-${UNDERLYING:-/dev/nullb0}}
	local util=${ZNS_MAX_UTIL:-750}
	local zs nz

	zs=$(cat "/sys/block/$(basename "$under")/queue/chunk_sectors") || return 1
	nz=$(( $(blockdev --getsz "$under") / zs ))
	echo $(( (nz - 4) * (zs / 8) * util / 1000 * 8 ))
}

# dm 테이블 한 줄.  zns_table <하위 장치> [format]
zns_table() {
	local under=$1
	shift
	local extra=""

	[ -n "${ZNS_MAX_UTIL:-}" ] && extra=" max_util:$ZNS_MAX_UTIL"
	echo "0 $(zns_logical_sectors "$under") zns-base $under$extra${1:+ $1}"
}

# 하위 장치의 zone 크기 / 노출 용량 (MiB)
zns_zone_mb() { echo $(( $(cat "/sys/block/$(basename "$UNDERLYING")/queue/chunk_sectors") / 2048 )); }
zns_cap_mb()  { echo $(( $(zns_logical_sectors "$UNDERLYING") / 2048 )); }

# 새 타깃 (장치를 지우고 시작)
zns_create_target() {
	local name=${1:-my-m1-device}
	local under=${2:-/dev/nullb0}

	zns_table "$under" format | dmsetup create "$name"
}

# 기존 메타데이터에서 복구해 다시 만든다
zns_reopen_target() {
	local name=${1:-my-m1-device}
	local under=${2:-/dev/nullb0}

	zns_table "$under" | dmsetup create "$name"
}
