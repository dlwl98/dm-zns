#!/usr/bin/env bash
# mkfs·mount: ZNS 장치 하나로 파일시스템을 만들 수 있는지, 걸리는 시간과 사용 가능 용량.
#   ZNS 단독:            ext4 직접, F2FS(-m), dm-zoned, ext4 on dm-zns
#   일반 장치(루프) 추가: ext4 on dm-zoned, F2FS multi-device
#   dm-zns 는 umount → 타깃 제거 → format 없이 다시 생성(복구) → mount 도 잰다.
# root 로, 레포 루트에서:  sudo DMZADM=<dmzadm 경로> bash scripts/eval-mkfs.sh [OUT]
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
# 실패 메시지 한 줄 (쉼표 제거)
why() { grep -m1 -iE "$2" "$1" | tr -d ',' | cut -c1-80; }

# ---- ZNS 단독 ----
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
# 다시 만들기 (복구 경로)
echo hello >"$MNT/probe"; sync; umount "$MNT"; dmsetup remove ev-dmzns
tic; zns_reopen_target ev-dmzns "$ZDEV"; setup=$(toc)
tic; mount /dev/mapper/ev-dmzns "$MNT"; mt=$(toc)
ok=$( [ "$(cat "$MNT/probe" 2>/dev/null)" = hello ] && echo ok || echo "probe 불일치")
row dmzns-reopen none "$setup" "" "$mt" "$(ev_avail_mb)" "$ok / recover_ms=$(dmsetup status ev-dmzns | awk '{print $27}')"

# ---- 일반 장치 추가 ----
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
