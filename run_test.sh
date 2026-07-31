#!/usr/bin/env bash
set -euo pipefail

DM_NAME="my-m1-device"
UNDERLYING="/dev/nullb0"
MOD_NAME="dm-zns-base"

sudo dmsetup remove "$DM_NAME" 2>/dev/null || true

if [ -f "scripts/nullblk-down.sh" ]; then
    sudo bash scripts/nullblk-down.sh
fi

if [ ! -b "$UNDERLYING" ]; then
	        sudo bash scripts/nullblk-up.sh
fi

sudo rmmod dm_zns_base 2>/dev/null || true

cd src
make clean >/dev/null
make >/dev/null
cd ..

sudo insmod src/$MOD_NAME.ko

sectors=$(sudo blockdev --getsz "$UNDERLYING")
echo "0 $sectors zns-base $UNDERLYING" | sudo dmsetup create "$DM_NAME"

before=$(sudo dmesg | grep -c blk_update_request || echo 0)
before=$(echo "$before" | tr -d '[:space:]') # 공백 제거

sudo fio --name=randw --filename=/dev/mapper/$DM_NAME --rw=randwrite --bs=4k --size=100M --ioengine=libaio --iodepth=32 --direct=1

after=$(sudo dmesg | grep -c blk_update_request || echo 0)
after=$(echo "$after" | tr -d '[:space:]')

before_num=$((before + 0))
after_num=$((after + 0))
delta=$((after_num - before_num))

echo "delta: $delta"
