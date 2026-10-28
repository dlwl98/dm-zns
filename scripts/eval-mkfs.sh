#!/usr/bin/env bash
# ZNS 하나로 mkfs 가 되는지, 그리고 각 구성의 mkfs·mount 시간과 사용 가능 용량.
#   sudo DMZADM=<dmzadm 경로> bash scripts/eval-mkfs.sh [OUT]
set -u
OUT=${1:-results/mkfs}
. scripts/eval-common.sh
mkdir -p "$OUT"
CSV=$OUT/mkfs.csv
echo "config,regular_dev,setup_s,mkfs_s,mount_s,avail_mib,result" >"$CSV"

T0=
tic() { T0=$(date +%s.%N); }
toc() { awk -v a="$T0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.2f", b-a}'; }
row() { echo "$1,$2,$3,$4,$5,$6,$7" >>"$CSV"; tail -1 "$CSV"; }
why() { grep -m1 -iE "$2" "$1" | tr -d ',' | cut -c1-80; }

ev_down
mark=$(dmesg | wc -l)
tic; mkfs.ext4 -q -F "$ZDEV" >"$OUT/ext4-raw.log" 2>&1; rc=$?; s=$(toc)
n=$(dmesg | tail -n +$((mark + 1)) | grep -c 'I/O error')
row ext4-raw none "" "$s" "" "" "mkfs rc=$rc / 커널 I/O 오류 ${n}건"

ev_down
tic; mkfs.f2fs -f -m "$ZDEV" >"$OUT/f2fs-alone.log" 2>&1; rc=$?; s=$(toc)
row f2fs-alone none "" "$s" "" "" "mkfs rc=$rc / $(why "$OUT/f2fs-alone.log" 'error|not|no ')"

ev_down
modprobe dm-zoned
tic; "$DMZADM" --format "$ZDEV" --force >"$OUT/dmzoned-alone.log" 2>&1; rc=$?; s=$(toc)
row dmzoned-alone none "$s" "" "" "" "dmzadm rc=$rc / $(why "$OUT/dmzoned-alone.log" 'error|not|fail')"

ev_down
tic; zns_load_module src/dm-zns-base.ko && zns_create_target ev-dmzns "$ZDEV"; setup=$(toc)
tic; ev_mkext4 /dev/mapper/ev-dmzns; mk=$(toc)
tic; mount /dev/mapper/ev-dmzns "$MNT"; mt=$(toc)
row dmzns none "$setup" "$mk" "$mt" "$(ev_avail_mb)" ok
# format 없이 다시 만들어 복구 경로를 잰다
echo hello >"$MNT/probe"; sync; umount "$MNT"; dmsetup remove ev-dmzns
tic; zns_reopen_target ev-dmzns "$ZDEV"; setup=$(toc)
tic; mount /dev/mapper/ev-dmzns "$MNT"; mt=$(toc)
ok=$( [ "$(cat "$MNT/probe" 2>/dev/null)" = hello ] && echo ok || echo "probe 불일치")
row dmzns-reopen none "$setup" "" "$mt" "$(ev_avail_mb)" "$ok / recover_ms=$(dmsetup status ev-dmzns | awk '{print $27}')"

ev_down
tic
truncate -s "${REG_MB}M" "$IMG"; EV_LOOP=$(losetup -f --show "$IMG")
"$DMZADM" --format "$EV_LOOP" "$ZDEV" --force --seq=2 --label=evdmz >"$OUT/dmzoned.log" 2>&1 \
	&& "$DMZADM" --start "$EV_LOOP" "$ZDEV" >>"$OUT/dmzoned.log" 2>&1
setup=$(toc)
EV_DEV=/dev/mapper/$(dmsetup ls --target zoned | awk 'NR==1{print $1}')
tic; ev_mkext4 "$EV_DEV"; mk=$(toc)
tic; mount "$EV_DEV" "$MNT"; mt=$(toc)
row dmzoned "${REG_MB}MiB" "$setup" "$mk" "$mt" "$(ev_avail_mb)" ok

ev_down
modprobe f2fs
truncate -s "${REG_MB}M" "$IMG"; EV_LOOP=$(losetup -f --show "$IMG")
tic; mkfs.f2fs -f -m -c "$ZDEV" "$EV_LOOP" >"$OUT/f2fs-default.log" 2>&1; rc=$?; mk=$(toc)
row f2fs-default "${REG_MB}MiB" "" "$mk" "" "" "mkfs rc=$rc / $(why "$OUT/f2fs-default.log" 'error|not|insufficient')"
tic; mkfs.f2fs -f -m -o "$F2FS_OP" -c "$ZDEV" "$EV_LOOP" >"$OUT/f2fs.log" 2>&1; mk=$(toc)
tic; mount -t f2fs "$EV_LOOP" "$MNT"; mt=$(toc)
row "f2fs-o$F2FS_OP" "${REG_MB}MiB" "" "$mk" "$mt" "$(ev_avail_mb)" ok

ev_down
echo; column -s, -t "$CSV"
