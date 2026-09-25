#!/usr/bin/env bash
# 여러 phase 검증을 순서대로 돌리고 요약만 출력한다. 전체 로그는 OUT 디렉터리에.
#
# 각 phase 동안 새로 찍힌 커널 경고(WARNING/BUG/hung task)도 함께 본다 —
# 검사가 전부 통과해도 커널이 경고를 냈다면 통과로 치지 않는다.
#
# 사용 (레포 루트에서, sudo 없이 — 내부에서 sudo 로 부른다):
#   bash scripts/run-phases.sh <OUT_DIR> <phase번호...>
#   예) bash scripts/run-phases.sh ~/results/run1 1 2 3 4 5
#       UNDERLYING=/dev/nvme0n1 P2_T4=0 P5_T3=0 bash scripts/run-phases.sh ~/results/run2 1 2 3 4 5
#
# sudo 는 환경변수를 버리므로 UNDERLYING / P2_T4 / P5_T3 는 여기서 명시적으로 넘긴다.
set -uo pipefail
ENV_PASS=(UNDERLYING="${UNDERLYING:-/dev/nullb0}" P2_T4="${P2_T4:-1}" P5_T3="${P5_T3:-1}")

OUT=$1; shift
mkdir -p "$OUT"
cd "$(dirname "$0")/.." || exit 1

KWARN='WARNING:|BUG:|Oops|hung_task|blocked for more than|Call Trace'
total_fail=0
for p in "$@"; do
	log="$OUT/phase$p.log"
	mark=$(sudo dmesg | wc -l)
	start=$(date +%s)
	sudo "${ENV_PASS[@]}" bash "scripts/phase$p-verify.sh" >"$log" 2>&1
	rc=$?
	sudo dmesg | tail -n +"$((mark + 1))" >"$OUT/phase$p.dmesg"
	nwarn=$(grep -c -E "$KWARN" "$OUT/phase$p.dmesg")
	echo "---- phase$p  exit=$rc  ($(( $(date +%s) - start ))s)  커널경고=$nwarn"
	grep -E '\[OK\]  |\[FAIL\]|PASSED|실패 항목' "$log" | sed 's/^/  /'
	if [ "$nwarn" -gt 0 ]; then
		grep -E "$KWARN" "$OUT/phase$p.dmesg" | head -3 | sed 's/^/  ! /'
		total_fail=$((total_fail + 1))
	fi
	[ "$rc" -ne 0 ] && total_fail=$((total_fail + 1))
done
echo "==== 실패한 phase(검사 실패 또는 커널 경고): $total_fail"
exit $total_fail
