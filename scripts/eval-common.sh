# 평가 공통: 세 구성을 만들어 $MNT 에 마운트한다.  source 해서 쓴다 (root, 레포 루트에서).
#   dmzns    ext4 on dm-zns            ZNS 단독
#   dmzoned  ext4 on dm-zoned          ZNS + 일반 장치(루프 파일)
#   f2fs     F2FS multi-device         ZNS + 일반 장치(루프 파일)
# 일반 장치는 루트 디스크 위 루프 파일이다(REG_MB). F2FS 는 zone 이 크고 적으면(2 GiB × 16)
# 예비 섹션이 커서, 일반 장치 16 GiB + 예비 비율 40% 에서 쓸 수 있는 공간이 약 11 GiB 다.
#
# 필요한 것: src/dm-zns-base.ko (make), dm-zoned-tools 의 dmzadm (DMZADM),
#            f2fs-tools 1.14 이상, Kafka 4.2.0 (KAFKA), bench/kafka 빌드 (eval-kafka, eval-t6)
# shellcheck shell=bash
ZDEV=${ZDEV:-/dev/nvme0n1}
REG_MB=${REG_MB:-16384}
F2FS_OP=${F2FS_OP:-40}
DMZADM=${DMZADM:-dmzadm}
IMG=${IMG:-/var/tmp/ev-regular.img}
MNT=${MNT:-/mnt/ev}
EV_DEV=
EV_LOOP=
. scripts/common.sh
mkdir -p "$MNT"

ev_down() {
	local t
	umount "$MNT" 2>/dev/null
	dmsetup remove ev-dmzns 2>/dev/null
	for t in $(dmsetup ls --target zoned 2>/dev/null | awk '$1!="No"{print $1}'); do
		dmsetup remove "$t" 2>/dev/null
	done
	for t in $(losetup -j "$IMG" 2>/dev/null | cut -d: -f1); do losetup -d "$t"; done
	rm -f "$IMG"
	EV_DEV= EV_LOOP=
	blkzone reset "$ZDEV"
	echo mq-deadline >"/sys/block/$(basename "$ZDEV")/queue/scheduler"
}

ev_mkext4() { mkfs.ext4 -q -F -E lazy_itable_init=0,lazy_journal_init=0 "$1"; }

# ev_up <dmzns|dmzoned|f2fs>
ev_up() {
	ev_down
	case $1 in
	dmzns)
		zns_load_module src/dm-zns-base.ko || return 1
		zns_create_target ev-dmzns "$ZDEV" || return 1
		EV_DEV=/dev/mapper/ev-dmzns
		ev_mkext4 "$EV_DEV" && mount "$EV_DEV" "$MNT" ;;
	dmzoned)
		modprobe dm-zoned
		truncate -s "${REG_MB}M" "$IMG"
		EV_LOOP=$(losetup -f --show "$IMG")
		"$DMZADM" --format "$EV_LOOP" "$ZDEV" --force --seq=2 --label=evdmz >/dev/null 2>&1 || return 1
		"$DMZADM" --start "$EV_LOOP" "$ZDEV" >/dev/null 2>&1 || return 1
		EV_DEV=/dev/mapper/$(dmsetup ls --target zoned | awk 'NR==1{print $1}')
		ev_mkext4 "$EV_DEV" && mount "$EV_DEV" "$MNT" ;;
	f2fs)
		modprobe f2fs
		truncate -s "${REG_MB}M" "$IMG"
		EV_LOOP=$(losetup -f --show "$IMG")
		wipefs -a "$EV_LOOP" >/dev/null 2>&1
		mkfs.f2fs -f -m -o "$F2FS_OP" -c "$ZDEV" "$EV_LOOP" >/dev/null 2>&1 || return 1
		EV_DEV=$EV_LOOP
		mount -t f2fs "$EV_DEV" "$MNT" ;;
	*) return 1 ;;
	esac
	sync
	echo 3 >/proc/sys/vm/drop_caches
}

# 노출(사용 가능) 크기, MiB
ev_avail_mb() { df -BM --output=avail "$MNT" | tail -1 | tr -dc 0-9; }

# ---- Kafka (KRaft standalone, 브로커 하나) ----
KAFKA=${KAFKA:-/opt/kafka_2.13-4.2.0}
BENCH=${BENCH:-bench/kafka}
CP="$BENCH/kafka-benchmark.jar:$KAFKA/libs/*"
export KAFKA_HEAP_OPTS=${KAFKA_HEAP_OPTS:-"-Xms2G -Xmx2G"}

kafka_up() {	# kafka_up <log dir>   (KAFKA_EXTRA="a=b;c=d" 로 설정 추가)
	local cfg=/tmp/ev-kafka.properties id
	if [ ! -x "$KAFKA/bin/kafka-server-start.sh" ] || [ ! -f "$BENCH/kafka-benchmark.jar" ]; then
		echo "KAFKA=$KAFKA 또는 $BENCH/kafka-benchmark.jar 가 없습니다 (bench/kafka/build.sh)" >&2
		return 1
	fi
	sed -e "s#^log.dirs=.*#log.dirs=$1#" -e "s#^num.partitions=.*#num.partitions=8#" \
		"$KAFKA/config/server.properties" >"$cfg"
	cat >>"$cfg" <<'EOF'
file.delete.delay.ms=1000
log.segment.delete.delay.ms=1000
message.max.bytes=2097152
replica.fetch.max.bytes=2097152
EOF
	[ -n "${KAFKA_EXTRA:-}" ] && echo "$KAFKA_EXTRA" | tr ';' '\n' >>"$cfg"
	id=$("$KAFKA/bin/kafka-storage.sh" random-uuid)
	"$KAFKA/bin/kafka-storage.sh" format -t "$id" --standalone -c "$cfg" >/dev/null || return 1
	"$KAFKA/bin/kafka-server-start.sh" -daemon "$cfg"
	for _ in $(seq 1 60); do
		"$KAFKA/bin/kafka-topics.sh" --bootstrap-server localhost:9092 --list >/dev/null 2>&1 && return 0
		sleep 1
	done
	return 1
}

kafka_down() {
	"$KAFKA/bin/kafka-server-stop.sh" >/dev/null 2>&1
	for _ in $(seq 1 60); do pgrep -f kafka.Kafka >/dev/null || return 0; sleep 1; done
	pkill -9 -f kafka.Kafka
}

topic_reset() {
	"$KAFKA/bin/kafka-topics.sh" --bootstrap-server localhost:9092 --delete --topic perf-test >/dev/null 2>&1
	sleep 3
	"$KAFKA/bin/kafka-topics.sh" --bootstrap-server localhost:9092 --create --topic perf-test \
		--partitions 8 --replication-factor 1 >/dev/null 2>&1
	sleep 1
}
