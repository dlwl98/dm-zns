# 검증 스크립트들이 함께 쓰는 helper.  source 해서 쓴다.
# shellcheck shell=bash

# 모듈을 확실히 "이 .ko 로" 올린다.
#
# insmod 실패를 무시하면 이미 올라가 있던 예전 모듈로 시험하게 되는데,
# 코드를 고친 뒤라면 새 코드를 검증했다고 착각하게 된다. 쌓아온 검증이
# 통째로 무의미해지는 종류의 사고라, 여기서 끊는다.
zns_load_module() {
	local ko=$1
	local name=${2:-dm_zns_base}
	local sys="/sys/module/${name//-/_}"
	local i want got

	want=$(modinfo -F srcversion "$ko" 2>/dev/null)
	got=$(cat "$sys/srcversion" 2>/dev/null)

	# 이미 올라와 있는 것이 바로 이 .ko 라면 그대로 쓴다.
	#
	# 우리가 원하는 보장은 "방금 새로 올렸다" 가 아니라 "올라간 것이 이
	# 코드다" 이다. 모듈에 전역 상태가 없고(상태는 전부 타깃별) 타깃은
	# teardown 에서 이미 내려갔으므로, 굳이 내렸다 올릴 이유가 없다.
	if [ -n "$want" ] && [ "$got" = "$want" ]; then
		return 0
	fi

	# 다른 빌드가 올라와 있다면 반드시 내려야 한다. 그러지 못하면
	# 예전 코드로 시험해 놓고 새 코드를 검증했다고 착각하게 된다.
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

zns_create_target() {
	local name=${1:-my-m1-device}
	local under=${2:-/dev/nullb0}
	local sectors

	sectors=$(blockdev --getsz "$under") || return 1
	echo "0 $sectors zns-base $under" | dmsetup create "$name"
}
