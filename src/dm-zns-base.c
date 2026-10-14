// SPDX-License-Identifier: GPL-2.0
/*
 * dm-zns-base: Random-to-Sequential Translation Target
 *
 * 위(upper)쪽: conventional 블록 디바이스로 광고 — ext4 등 임의 I/O 허용.
 * 아래(lower)쪽: host-managed ZNS 디바이스의 sequential-write 제약을 우리가 처리.
 *
 * 핵심 동작:
 *   WRITE  — 임의 LBA 쓰기를 활성 zone 의 wp 에 순차 append.
 *             LBA → PBA 매핑을 LSM-Tree 인덱스에 기록.
 *             덮어써서 죽는 옛 PBA 의 zone valid 를 그 자리에서 감소.
 *   READ   — 인덱스를 조회해 PBA 가 있으면 그 위치로 remapping.
 *             아직 쓰인 적 없는 LBA 는 zero-fill 후 완료.
 *   GC     — free zone 이 바닥나면 valid 가 가장 적은 FULL zone 을 골라
 *             살아있는 블록을 활성 zone 으로 이주시키고 zone 을 reset 해
 *             재사용한다.
 *   META   — 매핑 변경을 마지막 두 zone(메타 zone)의 로그에 기록하고,
 *             장치를 다시 만들 때 체크포인트 + 로그 재생으로 복구한다.
 *             위쪽에는 휘발성 쓰기 캐시가 있는 장치로 광고해 flush/FUA 를
 *             받고, 그 의미를 지킨다('메타데이터' 절).
 *
 *   테이블: <dev> [format] [max_util:<permille>]
 *     format    장치를 지우고 새로 시작. 없으면 메타데이터에서 복구한다.
 *     max_util  노출 용량 상한 = 데이터 풀 × permille/1000 (기본 750).
 *
 * ── LSM-Tree 인덱스 ─────────────────────────────────────────────────
 *
 *   조회:   MemTable(rbtree) → run[0](최신) → run[1] → ... → 없으면 unmapped
 *   flush:  MemTable 이 차면 중위 순회로 "정렬된 불변 run" 하나를 만든다
 *   컴팩션: 최신 run 들을 크기 계층형으로 병합(전용 워커). 같은 LBA 는
 *           최신 것만 남는다.
 *
 * ── zone 관리 ───────────────────────────────────────────────────────
 *
 *   zone 상태: FREE → ACTIVE(할당 중) → FULL → (GC) → FREE
 *   마지막 두 zone 은 META 로, 할당과 GC 의 대상이 아니다.
 *
 *   노출 용량: 데이터 zone N 개 중 활성 둘(사용자, GC)을 뺀 풀 P = N - 2.
 *   논리 크기 L 이 P·zone 보다 작아야 GC 가 언제나 빈 공간을 만든다(가득 찬
 *   zone 만 옮기며 헛도는 livelock 이 없다). 기본은 L ≤ 3/4·P·zone 으로,
 *   GC 몇 번 안에 공간이 생긴다.
 *
 *   하위 ZNS 는 zone 별 wp 위치의 쓰기만 받으므로, 한 zone 에 대한 쓰기는
 *   할당 순서대로 장치에 도착해야 한다. 그래서 zone 마다 제출자를 하나로
 *   둔다:
 *     - 사용자 zone: 순서 보장 제출 워커(sub_wq) 하나. .map 은 할당과 같은
 *       map_lock 구간에서 bio 를 sub_list 에 넣기만 하고, 워커가 그 순서
 *       대로 보낸다. .map 에서 바로 제출하면 current->bio_list 때문에 제출이
 *       .map 이 끝날 때까지 미뤄져, 여러 스레드(ext4 의 writeback 과 jbd2
 *       만으로도 둘)가 쓰면 순서가 뒤바뀐다.
 *     - GC zone: GC 워커 하나(gc_lock). 동기 I/O 라 순서가 자명하다.
 *   5.15 는 zone 당 쓰기를 한 번에 하나씩 내보내는 장치가 mq-deadline 뿐이라
 *   하위 스케줄러가 mq-deadline 이어야 한다(ctr 가 확인).
 *
 *   zone 별 진행 중 I/O 수(wr_inflight / rd_inflight)를 센다. GC 는 쓰기가
 *   남은 zone 을 이주하지 않고, 읽기·쓰기가 모두 끝나기 전에는 reset 하지
 *   않는다. 쓰기가 거절되면 그 zone 을 즉시 마감한다(하위 wp 가 어긋나
 *   이후 쓰기가 전부 거절되는 것을 막는다).
 *
 *   valid[z] 불변식: "map_lookup() 이 zone z 를 가리키게 되는 서로 다른
 *   LBA 블록의 수". map_lock 아래의 매핑 갱신에서만 변한다:
 *     - 새 쓰기:      zone_of(new)++ , 옛 매핑이 있었으면 zone_of(old)--
 *     - GC 이주 CAS:  성공 시 zone_of(new)++ , zone_of(victim)--
 *   FULL 이고 valid==0 인 zone 만 reset 한다. FULL zone 에는 새 할당이
 *   없으므로 valid 는 늘지 않고 줄기만 한다 — 0 은 흡수 상태다.
 *
 *   GC 이주는 compare-and-swap 패턴이다. 인덱스 스냅샷에서 victim 을
 *   가리키는 후보 (lba, pba) 를 모으고, 블록별로 데이터를 복사한 뒤
 *   락 안에서 "map_lookup(lba) 가 아직 pba 인가" 를 재확인하고 갱신한다.
 *   스캔이 낡아도 안전한 이유: victim 은 FULL 이라 그것을 가리키는 매핑은
 *   스냅 이후 늘지 않는다. 경합에서 지면 복사본만 버려진다(죽은 블록).
 *
 *   free zone 1개는 GC 전용으로 예약한다(GC_RESERVED_ZONES). 사용자
 *   쓰기는 그보다 많이 남았을 때만 새 zone 을 열 수 있으므로, GC 는
 *   이주에 쓸 공간이 항상 있다 — 공간이 없어 GC 를 못 하는 교착이 없다.
 *
 *   GC 는 반드시 전용 workqueue 에서 돈다. .map 은 current->bio_list 가
 *   설정된 재귀 방지 구간에서 불리는데, 거기서 submit_bio_wait() 를 하면
 *   기다리는 bio 가 bio_list 에 쌓인 채 영영 제출되지 않는다 — .map 이
 *   리턴해야 풀리는 목록을 .map 안에서 기다리는 자기 교착이다. 쓰기
 *   경로는 워커에 GC 를 맡기고 flush_work() 로 완료만 기다린다(워커의
 *   I/O 는 워커 컨텍스트에서 즉시 제출되므로 안전). dm-zoned 등이
 *   reclaim 을 워커로 빼는 이유가 이것이다.
 *
 * 한계(stretch / 알려진 것):
 *   - GC 이주는 4 KiB 동기 I/O 페어(submit_bio_wait).
 *   - 테이블을 살아 있는 채로 바꾸는 reload 는 지원하지 않는다 — 새 타깃이
 *     복구한 뒤에 옛 타깃이 마지막 커밋을 쓸 수 있다.
 *   - 사용자 쓰기는 활성 zone 하나에 제출자 하나라 장치 기준 zone 당 QD 1.
 *   - 데이터 쓰기가 하나라도 거절되면 fail-stop 이다: 메타 기록을 멈추고
 *     이후 쓰기·flush 는 EIO. 거절된 쓰기의 메모리 매핑을 되돌리지 못하기
 *     때문이다(장치의 매핑은 마지막 커밋에 머물러 일관적이다). 매핑을
 *     무효화하는 tombstone 이 생기면 계속 쓸 수 있게 바꿀 수 있다.
 *   - zone capacity 가 zone size 보다 작은 장치는 지원하지 않는다(쓰기 한도를
 *     zone size 로 잡는다).
 *   - 일찍 마감한 zone 에 ZONE_FINISH 를 보내지 않는다 — active zone 수에
 *     한도가 있는 장치에서는 한도에 걸릴 수 있다.
 *   - 하위 장치에 cgroup 쓰기 제한(blk-throttle)이 걸리면 제출 순서가 바뀔
 *     수 있다.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/workqueue.h>
#include <linux/crc32.h>
#include <linux/ktime.h>

#include "zns-compat.h"	/* 5.15 / 6.17 API 차이 */

#define DM_MSG_PREFIX "zns-base"

/* 4 KiB = 8 섹터 = 1 블록 */
#define SECTORS_PER_BLOCK	8U
#define BLOCK_SHIFT		3U   /* log2(SECTORS_PER_BLOCK) */

/* 매핑되지 않은 블록의 sentinel 값 */
#define PBA_UNMAPPED		((sector_t)-1)

/* MemTable 이 이만큼 차면 run 으로 내린다 */
#define MEMTABLE_MAX		4096U
/* 노드 풀은 여유를 둔다. flush 용 버퍼를 다시 채우기 전에 동시 쓰기가
 * 임계를 넘어설 수 있기 때문. */
#define NODE_POOL_MAX		(MEMTABLE_MAX * 2)
/* run 이 이만큼 쌓이면 컴팩션 */
#define RUNS_COMPACT_AT		8U
/*
 * 읽기에서 "PBA 가 연속인 구간" 을 앞으로 몇 블록까지 훑을지.
 * 조회는 spinlock 안에서 도는데 블록마다 run 들을 이진 탐색하므로,
 * 상한이 없으면 큰 읽기 하나가 락을 수십 µs 잡는다. 짧게 끊어도
 * dm_accept_partial_bio() 가 나머지를 다시 불러 주므로 정확성은 그대로다.
 */
#define READ_SCAN_MAX_BLOCKS	32U

/* GC 전용 예약 free zone 수. 사용자 쓰기는 이보다 많이 남아야 새 zone 을 연다. */
#define GC_RESERVED_ZONES	1U
/* 쓰기 경로에서 할당 실패 시 GC 를 몇 번까지 시도할지 */
#define WRITE_GC_RETRIES	4U
/*
 * 입장 제어: 할당됐지만 아직 완료되지 않은 사용자 쓰기의 상한.
 * 제출 워커가 하나뿐이라, 상한이 없으면 writeback 이 수 GiB 를 큐에 쌓고
 * 그 뒤에 선 fsync/jbd2 쓰기가 한참 밀린다. 동기 쓰기(REQ_SYNC/META/PRIO)
 * 는 두 배까지 허용해 백그라운드 쓰기에 막히지 않게 한다.
 */
#define ZNS_QUEUE_MAX_BYTES	(32ULL << 20)
/* GC 가 victim 의 진행 중 I/O 를 기다리는 최대 시간 */
#define ZNS_QUIESCE_TIMEOUT	(30 * HZ)
/* 노출 용량 상한의 기본값 (permille) */
#define ZNS_DEFAULT_MAX_UTIL	750U

/* ── 메타데이터 ── */
#define ZNS_META_ZONES		2U		/* 마지막 두 zone */
#define ZNS_MD_MAGIC		0x4d534e5aU	/* "ZNSM" */
#define ZNS_MD_VERSION		1U
#define ZNS_MD_BLK		4096U
#define ZNS_MD_BATCH		256U		/* 한 번에 읽고 쓰는 메타 블록 수 (1 MiB) */
/* 마지막 체크포인트 뒤 로그가 이만큼(128 MiB) 쌓이면 다음 커밋을 체크포인트로 — 복구 시간 상한 */
#define ZNS_LOG_CKPT_BLOCKS	32768U
/* 커밋 전 레코드 링. 사용자 쓰기는 GC 몫을 남기고 멈춘다. */
#define ZNS_RING_ORDER		18U
#define ZNS_RING_GC_HEADROOM	4096U
/* flush 가 없어도 이 간격으로 커밋한다 */
#define ZNS_LOG_TICK		(HZ)

enum zns_md_type {
	ZNS_MD_SB = 1,		/* 세대 시작 */
	ZNS_MD_CKPT,		/* 체크포인트 매핑 (lba 순) */
	ZNS_MD_CKPT_END,	/* 체크포인트 끝 — 이게 있어야 세대가 유효 */
	ZNS_MD_LOG,		/* 체크포인트 뒤의 매핑 변경 */
};

/* 메타 블록 머리 (on-disk, little-endian) */
struct zns_md_hdr {
	__le32	magic;
	__le32	type;
	__le64	gen;
	__le64	seq;	/* 메타 zone 안의 블록 번호 (SB = 0) */
	__le32	crc;	/* 블록 전체의 crc32, 이 칸은 0 으로 두고 계산 */
	__le32	count;	/* 블록 안 항목 수 (CKPT_END: 체크포인트 블록 수) */
	__le64	aux;	/* CKPT: 첫 항목 번호, CKPT_END: 총 항목 수, LOG: 첫 레코드 번호 */
};

struct zns_md_sb {
	__le32	version;
	__le32	nr_zones;
	__le64	zone_sectors;
	__le64	logical_blocks;
};

/* 체크포인트 항목 / 로그 레코드. 주소는 4 KiB 블록 단위. */
struct zns_md_ent {
	__le32	lba;
	__le32	pba;
};

struct zns_md_rec {
	__le32	lba;
	__le32	pba;
	__le32	len;
	__le32	flags;
};

#define ZNS_MD_BODY		(ZNS_MD_BLK - sizeof(struct zns_md_hdr))
#define ZNS_ENTS_PER_BLK	((unsigned int)(ZNS_MD_BODY / sizeof(struct zns_md_ent)))
#define ZNS_RECS_PER_BLK	((unsigned int)(ZNS_MD_BODY / sizeof(struct zns_md_rec)))

/* 커밋 전 매핑 변경 한 건 (extent). 블록 단위. */
struct zns_rec {
	u32	lba;
	u32	pba;
	u32	len;
	u32	flags;
};
#define ZNS_REC_VOID		1U	/* 쓰기가 실패했다 — 기록하지 않는다 */

/* ------------------------------------------------------------------ *
 * 자료구조                                                             *
 * ------------------------------------------------------------------ */

/* 매핑 한 건 */
struct zns_ent {
	sector_t lba_blk;	/* 논리 블록 번호 (LBA >> BLOCK_SHIFT) */
	sector_t pba;		/* 물리 섹터 */
};

/* MemTable 노드 — 노드 풀에서 bump 로 떼어 쓴다 */
struct zns_mnode {
	struct rb_node rb;
	struct zns_ent ent;
};

/* 불변 정렬 run (SSTable 에 해당) */
struct zns_run {
	struct list_head list;	/* c->runs — head 가 최신 */
	struct zns_ent  *ents;	/* lba_blk 오름차순 */
	unsigned int	 n;
	unsigned int	 cap;	/* ents 의 할당 크기 (메모리 통계) */
};

enum zns_zone_state {
	ZNS_ZONE_FREE = 0,
	ZNS_ZONE_ACTIVE,
	ZNS_ZONE_FULL,
	ZNS_ZONE_META,
};

struct zns_zone {
	sector_t	start;	/* zone 시작 (절대 섹터) */
	sector_t	wp;	/* 다음 할당 위치 (절대 섹터) */
	sector_t	sub_wp;	/* 제출 워커가 다음에 보내야 할 위치 (자가 검사용) */
	u32		valid;	/* 이 zone 을 가리키는 살아있는 매핑 수 (블록) */
	u8		state;
	atomic_t	wr_inflight;	/* 할당됐지만 아직 완료되지 않은 사용자 쓰기 */
	atomic_t	rd_inflight;	/* 이 zone 으로 remap 된 진행 중 읽기 */
};

struct zns_base_c;

/*
 * bio 별 상태 (ti->per_io_data_size). .map 입구에서 zidx = -1 로 초기화하고,
 * zone 에 걸린 I/O 만 zidx 를 채운다. .end_io 는 zidx 로 뒷정리를 한다.
 */
struct zns_pio {
	struct zns_base_c *c;	/* FUA 복제본의 완료 콜백이 쓴다 */
	u64		rec_seq;	/* 이 쓰기의 매핑 레코드 번호 */
	int		zidx;
	bool		write;
	bool		fua;
	u8		epoch;	/* 커밋이 기다리는 쓰기 묶음 (0/1) */
	unsigned int	bytes;	/* 입장 제어에 잡힌 쓰기 크기 */
};

/* GC 이주 후보 */
struct zns_cand {
	sector_t lba_blk;
	sector_t pba;
};

/* ------------------------------------------------------------------ *
 * Per-target 상태                                                      *
 * ------------------------------------------------------------------ */
struct zns_base_c {
	struct dm_dev	*dev;

	spinlock_t	 map_lock;	/* 인덱스 + zone 배열 + 레코드 링 보호 */
	sector_t	 total_sectors;	/* nr_zones * zone_sectors (메타 zone 포함) */
	sector_t	 total_blocks;	/* 논리(노출) 크기, 블록 */
	sector_t	 zone_sectors;
	unsigned int	 nr_zones;	/* 하위 장치의 zone 전부 */
	unsigned int	 data_zones;	/* 앞쪽 데이터 zone 수 (뒤 ZNS_META_ZONES 개는 메타) */

	/* --- zone --- */
	struct zns_zone	*zones;
	int		 active_user_idx;	/* 사용자 쓰기용 활성 zone, 없으면 -1 */
	int		 active_gc_idx;		/* GC 이주용 — 분리 이유는 머리 주석 */
	unsigned int	 free_zones;

	/* --- LSM 인덱스 --- */
	struct rb_root		 memtable;
	unsigned int		 mem_n;
	struct zns_mnode	*node_pool;
	unsigned int		 node_used;
	struct zns_ent		*spare;		/* 다음 flush 용 버퍼 */
	struct zns_run		*spare_run;
	struct list_head	 runs;		/* 최신이 head */
	unsigned int		 nr_runs;
	bool			 compacting;	/* 컴팩션/GC 스캔 상호 배제 */
	wait_queue_head_t	 compact_done;	/* compacting 이 풀리면 깨운다 */
	struct workqueue_struct	*compact_wq;
	struct work_struct	 compact_work;
	unsigned int		 compact_idle_runs; /* 병합할 게 없던 run 수 */
	atomic_t		 gc_waiting;	/* compacting 이 풀리기를 기다리는 GC */

	/* --- 순서 보장 제출 --- */
	struct bio_list	 sub_list;	/* map_lock. 사용자 쓰기, 할당 순서 그대로 */
	struct workqueue_struct *sub_wq;	/* ordered — 사용자 쓰기의 유일한 제출자 */
	struct work_struct	 sub_work;
	atomic64_t	 queued_bytes;	/* 할당됐지만 완료 안 된 사용자 쓰기 */
	wait_queue_head_t admit_wq;	/* queued_bytes 가 줄면 깨운다 */
	wait_queue_head_t idle_wq;	/* zone 의 inflight 가 0 이 되면 깨운다 */

	/* --- GC --- */
	struct mutex	 gc_lock;	/* GC 는 한 번에 하나 */
	struct workqueue_struct *gc_wq;	/* GC 전용 — .map 에서 직접 못 도는 이유는
					 * 파일 머리 주석 참고 */
	struct work_struct	 gc_work;
	struct bio_set	 gc_bs;		/* GC I/O 전용 — 상위와 풀을 나눠 쓰지 않는다 */

	/* --- 메타데이터: 기록 위치 (log_lock) --- */
	struct mutex	 log_lock;	/* 커밋·체크포인트 직렬화 */
	unsigned int	 md_cur;	/* 지금 쓰는 메타 zone (0/1) */
	u64		 md_gen;	/* 그 zone 의 세대 */
	u64		 md_seq;	/* 다음 메타 블록 번호 */
	sector_t	 md_wp;		/* 다음 메타 블록 위치 */
	unsigned int	 md_log_blocks;	/* 마지막 체크포인트 뒤 로그 블록 수 */
	bool		 md_err;	/* fail-stop: 메타 기록이나 데이터 쓰기 실패 — 이후 쓰기·flush 는 EIO */
	struct page	*md_pages[ZNS_MD_BATCH];
	struct bio_set	 md_bs;

	/* --- 메타데이터: 커밋 전 레코드 (map_lock) --- */
	struct zns_rec	*ring;		/* 매핑 변경, 할당 순서 그대로 */
	u64		 ring_cap;	/* 2 의 거듭제곱 */
	u64		 rec_next;	/* 다음 레코드 번호 */
	u64		 rec_committed;	/* 이 앞까지 장치에 기록됨 */
	bool		 frozen;	/* 체크포인트 스냅샷 동안 새 할당 정지 */
	bool		 commit_req;	/* 주기·링 압박 커밋 요청 */
	u32		 epoch;		/* 쓰기 묶음 번호. 홀짝으로 inflight 를 나눈다 */
	atomic_t	 ep_inflight[2];
	wait_queue_head_t ep_wq;	/* 묶음의 쓰기가 다 끝나면 */
	wait_queue_head_t ring_wq;	/* 링 자리가 나거나 frozen 이 풀리면 */
	struct bio_list	 flush_list;	/* 빈 flush bio — 커밋 뒤 완료 */
	struct bio_list	 fua_list;	/* 데이터가 끝난 FUA 쓰기 — 커밋 뒤 완료 */
	struct bio_set	 fua_bs;
	struct workqueue_struct *log_wq;
	struct work_struct	 log_work;
	struct delayed_work	 log_tick;
	bool		 crashed;	/* message crash: 전원 차단 흉내 */

	/* --- 통계 (dmsetup status) --- */
	u64	stat_commits;
	u64	stat_ckpts;
	u64	stat_log_blocks;
	u64	recover_ms;
	u64	stat_flushes;
	u64	stat_compactions;
	u64	stat_dropped;	/* 인덱스에서 제거된 중복 엔트리 수 */
	u64	stat_gc_runs;	/* 회수 완료한 zone 수 */
	u64	stat_migrated;	/* GC 가 이주시킨 블록 수 */
	u64	stat_write_err;	/* 하위가 거절한 사용자 쓰기 */
	u64	stat_order_viol; /* 제출 워커의 자가 검사 위반 (0 이어야 함) */
};

static inline unsigned int zidx_of(struct zns_base_c *c, sector_t pba)
{
	return (unsigned int)(pba / c->zone_sectors);
}

static inline struct zns_zone *zone_of(struct zns_base_c *c, sector_t pba)
{
	return &c->zones[zidx_of(c, pba)];
}

static inline struct zns_pio *zns_pio(struct bio *bio)
{
	return dm_per_bio_data(bio, sizeof(struct zns_pio));
}

/* ==================================================================
 * MemTable — rbtree
 * ================================================================== */

/*
 * 매핑 갱신. 호출자가 map_lock 을 잡고 있어야 한다.
 *
 * 반환:
 *   1        기존 노드를 제자리 갱신 — *old_pba 에 이전 값.
 *            (MemTable 이 최신이므로 runs 는 볼 필요 없다.)
 *   0        새 노드 삽입 — 이 LBA 는 MemTable 에 없었다.
 *            옛 매핑은 runs 에 있을 수 있으니 호출자가 확인한다.
 *   -ENOMEM  노드 풀 부족.
 */
static int mem_upsert(struct zns_base_c *c, sector_t lba_blk, sector_t pba,
		      sector_t *old_pba)
{
	struct rb_node **p = &c->memtable.rb_node;
	struct rb_node *parent = NULL;
	struct zns_mnode *n;

	while (*p) {
		parent = *p;
		n = rb_entry(parent, struct zns_mnode, rb);

		if (lba_blk < n->ent.lba_blk) {
			p = &parent->rb_left;
		} else if (lba_blk > n->ent.lba_blk) {
			p = &parent->rb_right;
		} else {
			*old_pba = n->ent.pba;
			n->ent.pba = pba;
			c->stat_dropped++;	/* 옛 엔트리는 인덱스에서 즉사 */
			return 1;
		}
	}

	if (c->node_used >= NODE_POOL_MAX)
		return -ENOMEM;

	n = &c->node_pool[c->node_used++];
	n->ent.lba_blk = lba_blk;
	n->ent.pba     = pba;

	rb_link_node(&n->rb, parent, p);
	rb_insert_color(&n->rb, &c->memtable);
	c->mem_n++;
	return 0;
}

/* map_lock 필요 */
static sector_t mem_lookup(struct zns_base_c *c, sector_t lba_blk)
{
	struct rb_node *node = c->memtable.rb_node;

	while (node) {
		struct zns_mnode *n = rb_entry(node, struct zns_mnode, rb);

		if (lba_blk < n->ent.lba_blk)
			node = node->rb_left;
		else if (lba_blk > n->ent.lba_blk)
			node = node->rb_right;
		else
			return n->ent.pba;
	}
	return PBA_UNMAPPED;
}

/* ==================================================================
 * run — 정렬 배열 이진 탐색
 * ================================================================== */

static sector_t run_lookup(const struct zns_run *r, sector_t lba_blk)
{
	unsigned int lo = 0, hi = r->n;

	while (lo < hi) {
		unsigned int mid = lo + (hi - lo) / 2;

		if (r->ents[mid].lba_blk < lba_blk)
			lo = mid + 1;
		else
			hi = mid;
	}

	if (lo < r->n && r->ents[lo].lba_blk == lba_blk)
		return r->ents[lo].pba;
	return PBA_UNMAPPED;
}

/* runs 만 조회 (MemTable 제외). map_lock 필요. */
static sector_t runs_lookup(struct zns_base_c *c, sector_t lba_blk)
{
	struct zns_run *r;
	sector_t pba;

	list_for_each_entry(r, &c->runs, list) {
		pba = run_lookup(r, lba_blk);
		if (pba != PBA_UNMAPPED)
			return pba;
	}
	return PBA_UNMAPPED;
}

/* 전체 조회. map_lock 필요. 최신부터 보므로 처음 맞는 것이 정답이다. */
static sector_t map_lookup(struct zns_base_c *c, sector_t lba_blk)
{
	sector_t pba = mem_lookup(c, lba_blk);

	if (pba != PBA_UNMAPPED)
		return pba;
	return runs_lookup(c, lba_blk);
}

/* ==================================================================
 * flush — MemTable → 불변 run
 * ================================================================== */

/* map_lock 필요. 미리 잡아둔 spare/spare_run 을 소비하므로 할당이 없다. */
static bool mem_flush_locked(struct zns_base_c *c)
{
	struct zns_run *run = c->spare_run;
	struct rb_node *node;
	unsigned int i = 0;

	if (!run || !c->spare || c->mem_n == 0)
		return false;

	for (node = rb_first(&c->memtable); node; node = rb_next(node)) {
		struct zns_mnode *n = rb_entry(node, struct zns_mnode, rb);

		c->spare[i++] = n->ent;
	}

	run->ents = c->spare;
	run->n    = i;
	run->cap  = NODE_POOL_MAX;

	c->spare     = NULL;
	c->spare_run = NULL;

	c->memtable  = RB_ROOT;
	c->mem_n     = 0;
	c->node_used = 0;

	list_add(&run->list, &c->runs);
	c->nr_runs++;
	c->stat_flushes++;
	return true;
}

/* 락 밖에서. 소비된 spare 를 다시 채운다. */
static void zns_refill_spares(struct zns_base_c *c)
{
	struct zns_ent *ents = NULL;
	struct zns_run *run = NULL;
	unsigned long flags;
	bool need_ents, need_run;

	spin_lock_irqsave(&c->map_lock, flags);
	need_ents = !c->spare;
	need_run  = !c->spare_run;
	spin_unlock_irqrestore(&c->map_lock, flags);

	if (need_ents)
		ents = zns_kvmalloc_array(NODE_POOL_MAX, sizeof(*ents), GFP_NOIO);
	if (need_run)
		run = kzalloc(sizeof(*run), GFP_NOIO);

	spin_lock_irqsave(&c->map_lock, flags);
	if (ents && !c->spare) {
		c->spare = ents;
		ents = NULL;
	}
	if (run && !c->spare_run) {
		c->spare_run = run;
		run = NULL;
	}
	spin_unlock_irqrestore(&c->map_lock, flags);

	kvfree(ents);
	kfree(run);
}

/* ==================================================================
 * 컴팩션 — 최신 run 몇 개를 하나로 병합 (크기 계층형)
 *
 * 매번 run 전부를 병합하면 한 번에 인덱스 전체(수백만 항목)를 다시 쓰고,
 * 그 사이 쌓인 run 때문에 다음 병합은 더 커진다. 대신 최신 run 부터 모으다가
 * 다음(더 오래된) run 이 지금까지 모은 양의 COMPACT_GROWTH 배보다 크면
 * 멈춘다. run 크기가 기하급수로 커지는 층을 이루므로, 항목 하나가 다시
 * 쓰이는 횟수와 run 수가 모두 O(log N) 이다. 병합 결과는 병합한 run 들의
 * 자리에 들어가므로, 더 오래된 run 에 남은 같은 LBA 항목은 새 run 이 가린다.
 *
 * 병합은 전용 워커에서 돈다. 쓰기 경로에서 돌면 그 쓰기가 병합 시간만큼
 * 멈추고, 끝날 때까지 CPU 를 놓지 않는다.
 * ================================================================== */

#define COMPACT_GROWTH	2U
/*
 * run 이 이보다 많으면 크기 규칙을 무시하고 병합한다(조회 비용 상한).
 * 이때도 가장 오래된 run 은 뺀다 — 인덱스 대부분을 담고 있어, 넣으면 매번
 * 인덱스 전체를 다시 쓰고 그 사이 쌓인 run 때문에 또 강제 병합이 걸린다.
 */
#define RUNS_FORCE_ALL	32U
/* 병합 한 번의 항목 수 상한 (16 B × 32M = 512 MiB, kvmalloc 의 INT_MAX 아래) */
#define MERGE_MAX_ENTRIES	(32U << 20)

struct zns_heap_ent {
	sector_t	lba;
	unsigned int	ri;	/* snap 인덱스 — 작을수록 최신 */
};

static inline bool heap_less(const struct zns_heap_ent *a,
			     const struct zns_heap_ent *b)
{
	return a->lba < b->lba || (a->lba == b->lba && a->ri < b->ri);
}

static void heap_sift_down(struct zns_heap_ent *h, unsigned int n,
			   unsigned int i)
{
	for (;;) {
		unsigned int l = 2 * i + 1, r = l + 1, m = i;

		if (l < n && heap_less(&h[l], &h[m]))
			m = l;
		if (r < n && heap_less(&h[r], &h[m]))
			m = r;
		if (m == i)
			return;
		swap(h[i], h[m]);
		i = m;
	}
}

/*
 * snap[0] 이 가장 최신. 같은 LBA 는 최신 run 의 것만 남긴다.
 * (lba, run) 순 최소 힙으로 k-way 병합 — O(total · log k). sleepable.
 */
static struct zns_ent *zns_merge_runs(struct zns_run **snap, unsigned int k,
				      unsigned int *out_n, u64 *dropped)
{
	struct zns_heap_ent *h;
	struct zns_ent *out;
	unsigned int *pos;
	unsigned int total = 0, n = 0, hn = 0, i;
	unsigned long steps = 0;
	u64 drop = 0;

	*out_n = 0;
	*dropped = 0;
	for (i = 0; i < k; i++)
		total += snap[i]->n;
	if (!total)
		return NULL;

	out = zns_kvmalloc_array(total, sizeof(*out), GFP_NOIO);
	pos = kcalloc(k, sizeof(*pos), GFP_NOIO);
	h   = kmalloc_array(k, sizeof(*h), GFP_NOIO);
	if (!out || !pos || !h) {
		kvfree(out);
		kfree(pos);
		kfree(h);
		return NULL;
	}

	for (i = 0; i < k; i++) {
		if (!snap[i]->n)
			continue;
		h[hn].lba = snap[i]->ents[0].lba_blk;
		h[hn].ri  = i;
		hn++;
	}
	for (i = hn / 2; i-- > 0; )
		heap_sift_down(h, hn, i);

	while (hn) {
		sector_t lba = h[0].lba;
		bool first = true;

		/* 이 LBA 를 가진 run 들이 최신 순으로 나온다 — 첫 것만 남긴다 */
		while (hn && h[0].lba == lba) {
			unsigned int ri = h[0].ri;

			if (first) {
				out[n++] = snap[ri]->ents[pos[ri]];
				first = false;
			} else {
				drop++;
			}
			if (++pos[ri] < snap[ri]->n)
				h[0].lba = snap[ri]->ents[pos[ri]].lba_blk;
			else
				h[0] = h[--hn];
			heap_sift_down(h, hn, 0);
		}
		if (!(++steps & 0xffff))
			cond_resched();
	}

	kfree(pos);
	kfree(h);
	*out_n = n;
	*dropped = drop;
	return out;
}

/*
 * 병합 한 번. 병합했으면 true (더 할 게 있을 수 있다).
 *
 * compacting 은 GC 의 인덱스 스캔과의 상호 배제를 겸한다 — GC 도 run 배열을
 * 락 밖에서 읽으므로, 병합이 끝난 run 을 kvfree 하는 것과 겹치면 안 된다.
 * 풀 때마다 compact_done 을 깨운다.
 */
static bool zns_compact_step(struct zns_base_c *c)
{
	struct zns_run **snap = NULL;
	struct zns_run *merged = NULL;
	struct zns_run *r;
	struct zns_ent *ents;
	struct list_head *anchor;
	unsigned int cap, k = 0, i, n = 0;
	unsigned long flags;
	u64 dropped = 0, sum = 0;
	bool force;

	spin_lock_irqsave(&c->map_lock, flags);
	/* GC 가 기다리고 있으면 양보한다 — 병합을 연달아 잡으면 GC 가 굶는다 */
	if (c->compacting || c->nr_runs < RUNS_COMPACT_AT ||
	    atomic_read(&c->gc_waiting)) {
		spin_unlock_irqrestore(&c->map_lock, flags);
		return false;
	}
	c->compacting = true;
	cap = c->nr_runs;
	spin_unlock_irqrestore(&c->map_lock, flags);

	snap   = kcalloc(cap, sizeof(*snap), GFP_NOIO);
	merged = kzalloc(sizeof(*merged), GFP_NOIO);
	if (!snap || !merged)
		goto out_abort;

	/*
	 * run 은 불변이고 리스트 앞쪽에만 추가된다. 앞에서부터 고른 k 개는
	 * 병합하는 동안 새 run 이 앞에 붙어도 그대로이고 연속해 있다.
	 */
	spin_lock_irqsave(&c->map_lock, flags);
	force = c->nr_runs > RUNS_FORCE_ALL;
	list_for_each_entry(r, &c->runs, list) {
		if (k >= cap)
			break;
		if (force) {
			if (list_is_last(&r->list, &c->runs))
				break;
		} else if (k >= 1 && r->n > COMPACT_GROWTH * sum) {
			break;
		}
		if (k >= 2 && sum + r->n > MERGE_MAX_ENTRIES)
			break;
		sum += r->n;
		snap[k++] = r;
	}
	if (k < 2)
		c->compact_idle_runs = c->nr_runs;	/* 다음 flush 까지 할 일 없음 */
	spin_unlock_irqrestore(&c->map_lock, flags);

	if (k < 2)
		goto out_abort;

	/* 할당이 안 되면 가장 오래된 것부터 빼며 더 작게 병합한다 */
	for (ents = NULL; k >= 2; k--) {
		ents = zns_merge_runs(snap, k, &n, &dropped);
		if (ents)
			break;
	}
	if (!ents) {
		spin_lock_irqsave(&c->map_lock, flags);
		c->compact_idle_runs = c->nr_runs;	/* 다음 flush 까지 재시도 안 함 */
		spin_unlock_irqrestore(&c->map_lock, flags);
		goto out_abort;
	}

	merged->ents = ents;
	merged->n    = n;
	merged->cap  = 0;
	for (i = 0; i < k; i++)
		merged->cap += snap[i]->n;	/* zns_merge_runs 의 할당 크기 */

	spin_lock_irqsave(&c->map_lock, flags);
	anchor = snap[k - 1]->list.next;
	for (i = 0; i < k; i++) {
		list_del(&snap[i]->list);
		c->nr_runs--;
	}
	list_add_tail(&merged->list, anchor);
	c->nr_runs++;
	c->stat_compactions++;
	c->stat_dropped += dropped;
	c->compacting = false;
	spin_unlock_irqrestore(&c->map_lock, flags);
	wake_up_all(&c->compact_done);

	DMDEBUG("compact: %u runs -> %u entries, %llu dropped",
		k, n, (unsigned long long)dropped);

	for (i = 0; i < k; i++) {
		kvfree(snap[i]->ents);
		kfree(snap[i]);
	}
	kfree(snap);
	return true;

out_abort:
	spin_lock_irqsave(&c->map_lock, flags);
	c->compacting = false;
	spin_unlock_irqrestore(&c->map_lock, flags);
	wake_up_all(&c->compact_done);
	kfree(snap);
	kfree(merged);
	return false;
}

static void zns_compact_workfn(struct work_struct *w)
{
	struct zns_base_c *c = container_of(w, struct zns_base_c, compact_work);

	while (zns_compact_step(c))
		cond_resched();
}

/* 락 없이 불러도 된다. 할 일이 있을 때만 워커를 깨운다. */
static void zns_kick_compact(struct zns_base_c *c)
{
	unsigned int runs = READ_ONCE(c->nr_runs);

	if (runs >= RUNS_COMPACT_AT && runs != READ_ONCE(c->compact_idle_runs))
		queue_work(c->compact_wq, &c->compact_work);
}

/* ==================================================================
 * zone 할당
 * ================================================================== */

/*
 * 활성 zone 에서 len_sectors 만큼 할당한다. map_lock 필요.
 *
 * 활성 zone 에 안 들어가면 그 zone 을 FULL 로 마감하고(자투리는 하위에
 * 쓰이지 않은 채 남는다 — reset 때 같이 회수) 새 free zone 을 연다.
 *
 * is_gc: GC 이주용 할당은 예약분(GC_RESERVED_ZONES)까지 쓸 수 있다.
 * 사용자 쓰기는 예약을 남겨둬야 한다 — GC 가 이주에 쓸 공간이 없어
 * GC 를 못 하는 교착을 막는 장치다.
 *
 * 반환: 시작 PBA, 실패 시 PBA_UNMAPPED.
 */
static sector_t zone_alloc_locked(struct zns_base_c *c, sector_t len_sectors,
				  bool is_gc)
{
	struct zns_zone *z = NULL;
	unsigned int min_free = is_gc ? 0 : GC_RESERVED_ZONES;
	int *aidx = is_gc ? &c->active_gc_idx : &c->active_user_idx;
	unsigned int i;
	sector_t pba;

	if (*aidx >= 0)
		z = &c->zones[*aidx];

	if (z && z->wp + len_sectors > z->start + c->zone_sectors) {
		z->state = ZNS_ZONE_FULL;
		*aidx = -1;
		z = NULL;
	}

	if (!z) {
		if (c->free_zones <= min_free)
			return PBA_UNMAPPED;

		for (i = 0; i < c->nr_zones; i++) {
			if (c->zones[i].state == ZNS_ZONE_FREE) {
				z = &c->zones[i];
				WARN_ON_ONCE(atomic_read(&z->wr_inflight));
				z->state = ZNS_ZONE_ACTIVE;
				z->wp = z->start;
				z->sub_wp = z->start;
				*aidx = i;
				c->free_zones--;
				break;
			}
		}
		if (!z)
			return PBA_UNMAPPED;	/* free_zones 와 불일치 — 방어 */
	}

	pba = z->wp;
	z->wp += len_sectors;
	return pba;
}

/* ==================================================================
 * 메타데이터 — 매핑 로그와 체크포인트
 *
 * 마지막 두 zone 이 메타 zone 이다. 한 번에 하나에만 이어 쓰고, 체크포인트를
 * 새로 쓸 때 다른 하나를 reset 해 그쪽으로 옮겨 간다(세대 gen + 1). 한 zone
 * 의 내용은 기록 순서대로
 *
 *     [SB] [CKPT ...] [CKPT_END] [LOG] [LOG] ...
 *
 * SB 는 세대와 지오메트리, CKPT 는 그 시점의 매핑 전체(lba 순), LOG 는 그
 * 뒤의 매핑 변경(extent)이다. 블록마다 crc 를 둔다. 복구는 CKPT_END 까지
 * 온전한 zone 중 세대가 큰 것을 골라 체크포인트를 읽고 로그를 차례로 재생
 * 한다. zone 은 앞에서부터만 쓰이므로 블록 위치 순서가 곧 기록 순서다.
 *
 * ── 무엇을 언제 기록하나 ──
 *   매핑 변경은 map_lock 아래에서 레코드 링에 쌓인다 — 순서가 매핑 갱신
 *   순서와 같다. 사용자 쓰기의 레코드는 데이터가 장치에 닿은 뒤에만 기록할
 *   수 있다(먼저 기록하면 크래시 뒤 쓰이지 않은 위치를 가리킨다). 그래서
 *   쓰기마다 epoch 를 붙인다. 커밋은 epoch 를 넘긴 뒤 이전 epoch 의 쓰기가
 *   다 끝나기를 기다리고, 그때까지의 레코드를 기록한다. 실패한 쓰기는 VOID
 *   로 표시돼 빠진다. GC 이주의 레코드는 데이터를 쓴 뒤 붙으므로 바로 기록할
 *   수 있다.
 *
 * ── 위쪽과의 계약 (크래시 일관성) ──
 *   위쪽에는 휘발성 쓰기 캐시가 있는 장치로 광고한다(flush_supported).
 *     - flush: 그 전에 완료된 쓰기의 레코드를 기록한 뒤 완료한다.
 *     - FUA 쓰기: 자기 레코드가 기록된 뒤 완료한다.
 *     - 그 밖의 완료된 쓰기: 크래시 뒤 옛 내용 또는 새 내용이다. 쓰이지 않은
 *       위치를 가리키는 일은 없다.
 *   ext4 저널이 가정하는 것은 이 계약(flush + FUA)뿐이다.
 *
 * ── GC 와의 순서 ──
 *   victim 을 reset 하기 전에 커밋한다. 이주한 블록의 새 위치와 victim 의
 *   블록을 죽인 덮어쓰기가 기록된 뒤라야, 크래시 뒤의 매핑이 reset 된 zone
 *   을 가리키지 않는다. 덮어쓰기는 GC 스캔보다 먼저 할당됐으므로 커밋의
 *   epoch 대기가 그 완료까지 기다려 준다.
 * ================================================================== */

static inline u64 zns_ring_used(struct zns_base_c *c)
{
	return c->rec_next - c->rec_committed;
}

/* map_lock 필요. 레코드 번호를 돌려준다. */
static u64 zns_rec_append_locked(struct zns_base_c *c, sector_t lba_blk,
				 sector_t pba, sector_t nr_blocks)
{
	struct zns_rec *r = &c->ring[c->rec_next & (c->ring_cap - 1)];

	r->lba   = (u32)lba_blk;
	r->pba   = (u32)(pba >> BLOCK_SHIFT);
	r->len   = (u32)nr_blocks;
	r->flags = 0;
	return c->rec_next++;
}

/* 메타 zone i(0/1) 의 zone 번호 */
static inline unsigned int zns_md_zone(struct zns_base_c *c, unsigned int i)
{
	return c->data_zones + i;
}

static inline void *zns_md_blk(struct zns_base_c *c, unsigned int i)
{
	return page_address(c->md_pages[i]);
}

static void zns_md_seal(void *blk, u32 type, u64 gen, u64 seq, u32 count,
			u64 aux)
{
	struct zns_md_hdr *h = blk;

	h->magic = cpu_to_le32(ZNS_MD_MAGIC);
	h->type  = cpu_to_le32(type);
	h->gen   = cpu_to_le64(gen);
	h->seq   = cpu_to_le64(seq);
	h->crc   = 0;
	h->count = cpu_to_le32(count);
	h->aux   = cpu_to_le64(aux);
	h->crc   = cpu_to_le32(crc32_le(~0U, blk, ZNS_MD_BLK));
}

/* 블록이 온전하면 type, 아니면 0. gen 이 0 이 아니면 세대도 맞아야 한다. */
static u32 zns_md_check(void *blk, u64 gen)
{
	struct zns_md_hdr *h = blk;
	u32 crc = le32_to_cpu(h->crc);
	bool ok;

	if (le32_to_cpu(h->magic) != ZNS_MD_MAGIC)
		return 0;
	if (gen && le64_to_cpu(h->gen) != gen)
		return 0;
	h->crc = 0;
	ok = crc32_le(~0U, blk, ZNS_MD_BLK) == crc;
	h->crc = cpu_to_le32(crc);
	return ok ? le32_to_cpu(h->type) : 0;
}

/* md_pages[0..n) 를 sect 에서 동기로 읽거나 쓴다. sleepable. */
static int zns_md_io(struct zns_base_c *c, sector_t sect, unsigned int n,
		     zns_req_op_t op)
{
	struct bio *b;
	unsigned int i;
	int ret;

	b = zns_bio_alloc_bs(c->dev->bdev, n, op | REQ_SYNC, GFP_NOIO, &c->md_bs);
	if (!b)
		return -ENOMEM;
	b->bi_iter.bi_sector = sect;
	for (i = 0; i < n; i++) {
		if (!bio_add_page(b, c->md_pages[i], ZNS_MD_BLK, 0)) {
			bio_put(b);
			return -EIO;
		}
	}
	ret = submit_bio_wait(b);
	bio_put(b);
	return ret;
}

/* 지금 메타 zone 에 이어 쓴다. log_lock 필요. */
static int zns_md_append(struct zns_base_c *c, unsigned int n)
{
	int ret = zns_md_io(c, c->md_wp, n, REQ_OP_WRITE);

	if (!ret)
		c->md_wp += (sector_t)n << BLOCK_SHIFT;
	return ret;
}

/* 지금 메타 zone 에 남은 블록 수 */
static sector_t zns_md_room(struct zns_base_c *c)
{
	struct zns_zone *z = &c->zones[zns_md_zone(c, c->md_cur)];

	return (z->start + c->zone_sectors - c->md_wp) >> BLOCK_SHIFT;
}

/*
 * 메타 zone mz 를 reset 하고 세대 gen 을 연다: SB + 체크포인트(ents) +
 * CKPT_END. flush 까지 끝나야 이 세대가 복구 대상이 되므로, 도중에 실패
 * 하거나 크래시가 나도 다른 메타 zone 의 이전 세대가 그대로 남는다.
 * 성공하면 이 zone 으로 옮겨 간다. log_lock 필요 (format 은 ctr 에서 혼자).
 */
static int zns_md_write_gen(struct zns_base_c *c, unsigned int mz, u64 gen,
			    const struct zns_ent *ents, unsigned int n)
{
	sector_t wp = c->zones[mz].start;
	unsigned int nb = 0, done = 0, ckpt_blocks = 0;
	struct zns_md_sb *sb;
	u32 crc_all = ~0U;
	u64 seq = 0;
	void *blk;
	int ret;

	/* SB + 체크포인트 + 끝 — ctr 가 들어맞음을 확인했지만 넘쳐 쓰지 않게 다시 */
	if (2 + DIV_ROUND_UP(n, ZNS_ENTS_PER_BLK) > (c->zone_sectors >> BLOCK_SHIFT))
		return -ENOSPC;

	ret = zns_zone_mgmt(c->dev->bdev, REQ_OP_ZONE_RESET, wp, c->zone_sectors);
	if (ret)
		return ret;

	blk = zns_md_blk(c, nb++);
	memset(blk, 0, ZNS_MD_BLK);
	sb = blk + sizeof(struct zns_md_hdr);
	sb->version        = cpu_to_le32(ZNS_MD_VERSION);
	sb->nr_zones       = cpu_to_le32(c->nr_zones);
	sb->zone_sectors   = cpu_to_le64(c->zone_sectors);
	sb->logical_blocks = cpu_to_le64(c->total_blocks);
	zns_md_seal(blk, ZNS_MD_SB, gen, seq++, 0, 0);

	while (done < n) {
		unsigned int cnt = min_t(unsigned int, n - done, ZNS_ENTS_PER_BLK);
		struct zns_md_ent *e;
		unsigned int i;

		blk = zns_md_blk(c, nb++);
		memset(blk, 0, ZNS_MD_BLK);
		e = blk + sizeof(struct zns_md_hdr);
		for (i = 0; i < cnt; i++) {
			e[i].lba = cpu_to_le32((u32)ents[done + i].lba_blk);
			e[i].pba = cpu_to_le32((u32)(ents[done + i].pba >> BLOCK_SHIFT));
		}
		crc_all = crc32_le(crc_all, (void *)e, cnt * sizeof(*e));
		zns_md_seal(blk, ZNS_MD_CKPT, gen, seq++, cnt, done);
		done += cnt;
		ckpt_blocks++;

		if (nb == ZNS_MD_BATCH) {
			ret = zns_md_io(c, wp, nb, REQ_OP_WRITE);
			if (ret)
				return ret;
			wp += (sector_t)nb << BLOCK_SHIFT;
			nb = 0;
			cond_resched();
		}
	}

	blk = zns_md_blk(c, nb++);
	memset(blk, 0, ZNS_MD_BLK);
	*(__le32 *)(blk + sizeof(struct zns_md_hdr)) = cpu_to_le32(crc_all);
	zns_md_seal(blk, ZNS_MD_CKPT_END, gen, seq++, ckpt_blocks, n);
	ret = zns_md_io(c, wp, nb, REQ_OP_WRITE);
	if (ret)
		return ret;
	wp += (sector_t)nb << BLOCK_SHIFT;

	ret = blkdev_issue_flush(c->dev->bdev);
	if (ret)
		return ret;

	c->md_cur        = mz - zns_md_zone(c, 0);
	c->md_gen        = gen;
	c->md_seq        = seq;
	c->md_wp         = wp;
	c->md_log_blocks = 0;
	return 0;
}

/* 레코드 [from, to) 를 LOG 블록으로 이어 쓴다. 자리가 모자라면 -ENOSPC. log_lock 필요. */
static int zns_write_log(struct zns_base_c *c, u64 from, u64 to)
{
	struct zns_md_rec *rr = NULL;
	unsigned int nb = 0, cnt = 0, need;
	u64 s, nrec = 0, first = 0;
	void *blk = NULL;
	int ret;

	for (s = from; s < to; s++)
		if (!(c->ring[s & (c->ring_cap - 1)].flags & ZNS_REC_VOID))
			nrec++;
	if (!nrec)
		return 0;
	need = (unsigned int)DIV_ROUND_UP_ULL(nrec, ZNS_RECS_PER_BLK);
	if (need > zns_md_room(c))
		return -ENOSPC;

	for (s = from; s < to; s++) {
		const struct zns_rec *r = &c->ring[s & (c->ring_cap - 1)];

		if (r->flags & ZNS_REC_VOID)
			continue;
		if (!blk) {
			blk = zns_md_blk(c, nb);
			memset(blk, 0, ZNS_MD_BLK);
			rr = blk + sizeof(struct zns_md_hdr);
			cnt = 0;
			first = s;
		}
		rr[cnt].lba = cpu_to_le32(r->lba);
		rr[cnt].pba = cpu_to_le32(r->pba);
		rr[cnt].len = cpu_to_le32(r->len);
		if (++cnt < ZNS_RECS_PER_BLK)
			continue;

		zns_md_seal(blk, ZNS_MD_LOG, c->md_gen, c->md_seq++, cnt, first);
		blk = NULL;
		if (++nb == ZNS_MD_BATCH) {
			ret = zns_md_append(c, nb);
			if (ret)
				return ret;
			nb = 0;
		}
	}
	if (blk) {
		zns_md_seal(blk, ZNS_MD_LOG, c->md_gen, c->md_seq++, cnt, first);
		nb++;
	}
	if (nb) {
		ret = zns_md_append(c, nb);
		if (ret)
			return ret;
	}
	c->md_log_blocks   += need;
	c->stat_log_blocks += need;
	return 0;
}

/*
 * 체크포인트: 지금의 매핑 전체를 다른 메타 zone 에 새 세대로 쓰고 그쪽으로
 * 옮긴다. 로그가 길어지거나 메타 zone 이 차면 커밋이 부른다. log_lock 필요.
 *
 * 체크포인트에는 완료된 쓰기만 반영돼야 한다. 새 할당을 잠시 막고(frozen)
 * 진행 중인 쓰기를 모두 끝낸 뒤, 매핑 스냅샷과 레코드 경계를 같은 락 구간
 * 에서 잡는다. 스냅샷 직후 쓰기를 풀어 준다 — 그 뒤의 변경은 경계 이후의
 * 레코드라 새 zone 의 로그로 간다. *boundary 에 덮은 경계를 돌려준다.
 */
static int zns_checkpoint(struct zns_base_c *c, u64 *boundary)
{
	struct zns_run **snap = NULL;
	struct zns_run *memrun, *r;
	struct zns_ent *mem, *ents = NULL;
	struct rb_node *node;
	unsigned long flags;
	unsigned int k = 0, kmax, n = 0, mn = 0;
	u64 total = 0, dropped, b;
	int ret = 0;

	mem    = zns_kvmalloc_array(NODE_POOL_MAX, sizeof(*mem), GFP_NOIO);
	memrun = kzalloc(sizeof(*memrun), GFP_NOIO);
	if (!mem || !memrun) {
		ret = -ENOMEM;
		goto out;
	}

	spin_lock_irqsave(&c->map_lock, flags);
	c->frozen = true;
	spin_unlock_irqrestore(&c->map_lock, flags);
	wait_event(c->ep_wq, !atomic_read(&c->ep_inflight[0]) &&
			     !atomic_read(&c->ep_inflight[1]));
	smp_rmb();

	/* 기다리는 동안 쓰기가 실패했으면 그 매핑을 체크포인트에 넣으면 안 된다 */
	ret = READ_ONCE(c->md_err) ? -EIO : 0;
	/* 체크포인트가 가리킬 데이터를 하위의 휘발성 캐시에서 먼저 내린다 */
	if (!ret)
		ret = blkdev_issue_flush(c->dev->bdev);
	if (ret) {
		spin_lock_irqsave(&c->map_lock, flags);
		c->frozen = false;
		spin_unlock_irqrestore(&c->map_lock, flags);
		wake_up_all(&c->ring_wq);
		goto out;
	}

	/* run 배열을 락 밖에서 읽으므로 컴팩션과 배제한다 (GC 스캔과 같은 방식) */
	atomic_inc(&c->gc_waiting);
	for (;;) {
		spin_lock_irqsave(&c->map_lock, flags);
		if (!c->compacting)
			break;
		spin_unlock_irqrestore(&c->map_lock, flags);
		wait_event(c->compact_done, !READ_ONCE(c->compacting));
	}
	atomic_dec(&c->gc_waiting);
	c->compacting = true;

	/* run 수는 GC 의 flush 로 늘 수 있다 — 넉넉히 잡고 모자라면 다시 */
	for (;;) {
		kmax = c->nr_runs + 1 + 8;
		spin_unlock_irqrestore(&c->map_lock, flags);
		kfree(snap);
		snap = kcalloc(kmax, sizeof(*snap), GFP_NOIO);
		spin_lock_irqsave(&c->map_lock, flags);
		if (!snap || c->nr_runs + 1 <= kmax)
			break;
	}
	if (!snap) {
		c->compacting = false;
		c->frozen = false;
		spin_unlock_irqrestore(&c->map_lock, flags);
		wake_up_all(&c->compact_done);
		wake_up_all(&c->ring_wq);
		ret = -ENOMEM;
		goto out;
	}

	for (node = rb_first(&c->memtable); node; node = rb_next(node))
		mem[mn++] = rb_entry(node, struct zns_mnode, rb)->ent;
	memrun->ents = mem;
	memrun->n    = mn;
	snap[k++] = memrun;			/* 가장 최신 */
	list_for_each_entry(r, &c->runs, list)
		snap[k++] = r;
	b = c->rec_next;
	c->frozen = false;
	spin_unlock_irqrestore(&c->map_lock, flags);
	wake_up_all(&c->ring_wq);

	for (n = 0; n < k; n++)
		total += snap[n]->n;
	n = 0;
	if (total) {
		ents = zns_merge_runs(snap, k, &n, &dropped);
		if (!ents)
			ret = -ENOMEM;
	}

	spin_lock_irqsave(&c->map_lock, flags);
	c->compacting = false;
	spin_unlock_irqrestore(&c->map_lock, flags);
	wake_up_all(&c->compact_done);

	if (!ret)
		ret = zns_md_write_gen(c, zns_md_zone(c, 1 - c->md_cur),
				       c->md_gen + 1, ents, n);
	if (!ret) {
		*boundary = b;
		c->stat_ckpts++;
		DMINFO("checkpoint: gen %llu, %u mappings",
		       (unsigned long long)c->md_gen, n);
	}
	kvfree(ents);
out:
	kfree(snap);
	kfree(memrun);
	kvfree(mem);
	return ret;
}

/*
 * 커밋: 지금까지 완료된 쓰기의 매핑 변경을 장치에 기록한다. flush/FUA,
 * GC 의 reset 전 장벽, 주기 커밋이 모두 이것을 부른다. sleepable.
 */
static int zns_log_commit(struct zns_base_c *c, bool force_ckpt)
{
	unsigned long flags;
	u64 boundary;
	u32 old;
	int ret;

	mutex_lock(&c->log_lock);
	if (READ_ONCE(c->crashed) || READ_ONCE(c->md_err)) {
		ret = -EIO;
		goto out;
	}

	spin_lock_irqsave(&c->map_lock, flags);
	old = c->epoch++;
	boundary = c->rec_next;
	spin_unlock_irqrestore(&c->map_lock, flags);

	/*
	 * 경계 앞 레코드의 쓰기가 모두 끝나야 기록할 수 있다. 그보다 이전 묶음은
	 * 앞선 커밋이 이미 기다렸으므로 이 묶음만 보면 된다(커밋은 직렬).
	 */
	wait_event(c->ep_wq, !atomic_read(&c->ep_inflight[old & 1]));
	smp_rmb();	/* 쓰기 완료 쪽이 남긴 VOID 표시와 md_err 를 본다 */

	/*
	 * 기다리는 동안 데이터 쓰기가 실패했으면 커밋하지 않는다(fail-stop).
	 * 실패한 쓰기의 메모리 매핑은 쓰이지 않은 위치를 가리키므로, 그 뒤의
	 * 체크포인트나 GC reset 이 장치의 매핑을 그 상태로 끌고 가면 안 된다.
	 */
	if (READ_ONCE(c->md_err) || READ_ONCE(c->crashed)) {
		ret = -EIO;
		goto out;
	}

	/* 데이터가 하위의 휘발성 캐시에만 있을 수 있다 — 메타보다 먼저 내린다 */
	ret = blkdev_issue_flush(c->dev->bdev);

	if (!ret) {
		ret = -ENOSPC;
		if (!force_ckpt && c->md_log_blocks < ZNS_LOG_CKPT_BLOCKS)
			ret = zns_write_log(c, c->rec_committed, boundary);
	}
	if (ret == -ENOSPC) {
		ret = zns_checkpoint(c, &boundary);
		/* 메모리가 모자라 체크포인트를 못 하면 자리가 남은 한 로그로 */
		if (ret == -ENOMEM)
			ret = zns_write_log(c, c->rec_committed, boundary);
	}
	if (!ret)
		ret = blkdev_issue_flush(c->dev->bdev);
	if (ret) {
		if (ret != -EIO || !READ_ONCE(c->md_err))
			DMERR("metadata commit failed (%d); writes and flushes will fail from now on",
			      ret);
		WRITE_ONCE(c->md_err, true);
		wake_up_all(&c->ring_wq);
		goto out;
	}

	spin_lock_irqsave(&c->map_lock, flags);
	c->rec_committed = boundary;
	c->stat_commits++;
	spin_unlock_irqrestore(&c->map_lock, flags);
	wake_up_all(&c->ring_wq);
out:
	mutex_unlock(&c->log_lock);
	return ret;
}

/*
 * flush 와 FUA 쓰기를 완료시키는 워커. 모아 둔 것을 커밋 한 번으로 처리
 * 한다(group commit). FUA 쓰기는 자기 레코드가 커밋된 것만 완료한다 — 커밋
 * 도중에 끝난 FUA 쓰기는 다음 바퀴의 커밋을 기다린다.
 */
static void zns_log_workfn(struct work_struct *w)
{
	struct zns_base_c *c = container_of(w, struct zns_base_c, log_work);
	struct bio_list flushes, done, keep;
	unsigned long flags;
	struct bio *bio;
	bool need;
	int ret;

	for (;;) {
		bio_list_init(&flushes);
		bio_list_init(&done);
		bio_list_init(&keep);

		spin_lock_irqsave(&c->map_lock, flags);
		bio_list_merge(&flushes, &c->flush_list);
		bio_list_init(&c->flush_list);
		need = !bio_list_empty(&flushes) || !bio_list_empty(&c->fua_list) ||
		       (c->commit_req && c->rec_next != c->rec_committed);
		c->commit_req = false;
		spin_unlock_irqrestore(&c->map_lock, flags);
		if (!need)
			return;

		ret = zns_log_commit(c, false);

		while ((bio = bio_list_pop(&flushes))) {
			bio->bi_status = ret ? BLK_STS_IOERR : BLK_STS_OK;
			bio_endio(bio);
		}

		spin_lock_irqsave(&c->map_lock, flags);
		while ((bio = bio_list_pop(&c->fua_list))) {
			if (ret || zns_pio(bio)->rec_seq < c->rec_committed)
				bio_list_add(&done, bio);
			else
				bio_list_add(&keep, bio);
		}
		c->fua_list = keep;
		spin_unlock_irqrestore(&c->map_lock, flags);

		while ((bio = bio_list_pop(&done))) {
			if (ret)
				bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
		}
		if (ret)
			return;		/* 이후 커밋도 실패한다 — 남은 건 다음 요청이 끝낸다 */
		cond_resched();
	}
}

static void zns_log_tickfn(struct work_struct *w)
{
	struct zns_base_c *c = container_of(to_delayed_work(w),
					    struct zns_base_c, log_tick);

	if (READ_ONCE(c->rec_next) != READ_ONCE(c->rec_committed)) {
		WRITE_ONCE(c->commit_req, true);
		queue_work(c->log_wq, &c->log_work);
	}
	queue_delayed_work(system_wq, &c->log_tick, ZNS_LOG_TICK);
}

/* ==================================================================
 * GC — victim 의 살아있는 블록을 이주시키고 zone 을 reset 한다
 * ================================================================== */

/* 4 KiB 동기 I/O 한 건. sleepable. */
static int zns_rw_block(struct zns_base_c *c, sector_t sect, struct page *pg,
			zns_req_op_t op)
{
	struct bio *b;
	int ret;

	b = zns_bio_alloc_bs(c->dev->bdev, 1, op | REQ_SYNC, GFP_NOIO, &c->gc_bs);
	if (!b)
		return -ENOMEM;

	b->bi_iter.bi_sector = sect;
	if (!bio_add_page(b, pg, SECTORS_PER_BLOCK << SECTOR_SHIFT, 0)) {
		bio_put(b);
		return -EIO;
	}

	ret = submit_bio_wait(b);
	bio_put(b);
	return ret;
}

/*
 * victim 선정. map_lock 필요.
 * FULL 중 valid 최소. valid==0 이면 이주 없이 공짜로 회수된다.
 * 전부 살아 있는 zone 은 옮겨도 빈 공간이 생기지 않으므로 고르지 않는다.
 */
static int zns_pick_victim_locked(struct zns_base_c *c)
{
	u32 zone_blocks = (u32)(c->zone_sectors >> BLOCK_SHIFT);
	int best = -1;
	u32 best_valid = U32_MAX;
	unsigned int i;

	for (i = 0; i < c->nr_zones; i++) {
		struct zns_zone *z = &c->zones[i];

		if (z->state != ZNS_ZONE_FULL || z->valid >= zone_blocks)
			continue;
		if (z->valid < best_valid) {
			best = i;
			best_valid = z->valid;
			if (best_valid == 0)
				break;
		}
	}
	return best;
}

/*
 * zone 하나를 회수한다. gc_lock 을 잡은 채로, map_lock 밖에서 호출.
 * want_victim >= 0 이면 그 zone 을 강제 지정 (dmsetup message 용).
 *
 * 반환: 0 성공(free zone 이 하나 늘었다), 음수 실패.
 */
static int zns_gc_once(struct zns_base_c *c, int want_victim)
{
	struct zns_zone *victim;
	struct zns_ent *mem_snap = NULL;
	struct zns_run **run_snap = NULL;
	struct zns_cand *cand = NULL;
	struct page *pg = NULL;
	struct rb_node *node;
	unsigned long flags;
	sector_t vstart, vend;
	unsigned int mem_n = 0, k = 0, kmax, ncand = 0, ci;
	unsigned int max_cand;
	int vidx, ret = 0, restarts = 0;

	/* ---- 스냅샷용 메모리 (락 밖 선할당) ---- */
	max_cand = (unsigned int)(c->zone_sectors >> BLOCK_SHIFT);
	mem_snap = zns_kvmalloc_array(NODE_POOL_MAX, sizeof(*mem_snap), GFP_NOIO);
	cand     = zns_kvmalloc_array(max_cand, sizeof(*cand), GFP_NOIO);
	pg       = alloc_page(GFP_NOIO);
	if (!mem_snap || !cand || !pg) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * 스냅 단계는 컴팩션과 상호 배제한다(compacting 플래그).
	 * 컴팩션이 run 을 kvfree 하는 동안 우리가 그 run 배열을 락 밖에서
	 * 스캔하면 use-after-free 다. 병합 한 번이 끝날 때까지 기다린다.
	 */
restart:
	atomic_inc(&c->gc_waiting);	/* 컴팩션 워커가 다음 병합을 잡지 않게 */
	for (;;) {
		spin_lock_irqsave(&c->map_lock, flags);
		if (!c->compacting)
			break;
		spin_unlock_irqrestore(&c->map_lock, flags);
		wait_event(c->compact_done, !READ_ONCE(c->compacting));
	}
	atomic_dec(&c->gc_waiting);
	/* 여기부터 map_lock 보유 + compacting 선점 */
	c->compacting = true;

	vidx = (want_victim >= 0) ? want_victim : zns_pick_victim_locked(c);
	if (vidx < 0 || vidx >= (int)c->nr_zones ||
	    c->zones[vidx].state != ZNS_ZONE_FULL) {
		c->compacting = false;
		spin_unlock_irqrestore(&c->map_lock, flags);
		ret = (want_victim >= 0) ? -EINVAL : -ENOSPC;
		goto out;
	}
	victim = &c->zones[vidx];
	vstart = victim->start;
	vend   = victim->start + c->zone_sectors;

	/*
	 * FULL 로 마감된 직후의 zone 에는 아직 장치에 닿지 않은 쓰기가 남아
	 * 있을 수 있다. 그 상태에서 이주하면 안 쓰인 블록을 읽어 옮기고, reset
	 * 하면 뒤늦은 쓰기가 거절된다. FULL 이면 새 할당이 없으므로 wr_inflight
	 * 는 줄기만 하고, 그 쓰기들은 제출 워커가 독립적으로 내보내므로 기다림은
	 * 끝난다.
	 */
	if (atomic_read(&victim->wr_inflight)) {
		c->compacting = false;
		spin_unlock_irqrestore(&c->map_lock, flags);
		if (++restarts > 3 ||
		    !wait_event_timeout(c->idle_wq,
					!atomic_read(&victim->wr_inflight),
					ZNS_QUIESCE_TIMEOUT)) {
			ret = -ETIMEDOUT;
			goto out;
		}
		goto restart;
	}

	if (victim->valid == 0) {
		/* 이주할 것이 없다 — 스냅 불필요, 바로 reset 으로 */
		c->compacting = false;
		spin_unlock_irqrestore(&c->map_lock, flags);
		goto do_reset;
	}

	/* MemTable 스냅 (mem_n ≤ MEMTABLE_MAX 이므로 락 안 복사 상한 있음) */
	for (node = rb_first(&c->memtable); node; node = rb_next(node)) {
		struct zns_mnode *n = rb_entry(node, struct zns_mnode, rb);

		mem_snap[mem_n++] = n->ent;
	}

	/* run 포인터 스냅 — 개수만 세고 배열은 락 밖에서 잡은 뒤 다시 */
	kmax = c->nr_runs;
	spin_unlock_irqrestore(&c->map_lock, flags);

	run_snap = kcalloc(kmax + 8, sizeof(*run_snap), GFP_NOIO);
	if (!run_snap) {
		spin_lock_irqsave(&c->map_lock, flags);
		c->compacting = false;
		spin_unlock_irqrestore(&c->map_lock, flags);
		ret = -ENOMEM;
		goto out;
	}

	spin_lock_irqsave(&c->map_lock, flags);
	{
		struct zns_run *r;

		list_for_each_entry(r, &c->runs, list) {
			if (k >= kmax + 8)
				break;	/* 그 사이 늘어난 몫은 victim 을
					 * 가리킬 수 없으므로 놓쳐도 무방 */
			run_snap[k++] = r;
		}
	}
	spin_unlock_irqrestore(&c->map_lock, flags);

	/*
	 * ---- 후보 수집 (락 밖) ----
	 * victim 은 FULL: 새 할당이 없으므로 victim 을 가리키는 매핑은 스냅
	 * 이후 늘지 않는다. 스냅에 있는 것이 전부이고, 줄어든 것은 이주
	 * 단계의 재확인(CAS)이 걸러낸다.
	 */
	for (ci = 0; ci < mem_n && ncand < max_cand; ci++) {
		if (mem_snap[ci].pba >= vstart && mem_snap[ci].pba < vend) {
			cand[ncand].lba_blk = mem_snap[ci].lba_blk;
			cand[ncand].pba     = mem_snap[ci].pba;
			ncand++;
		}
	}
	for (ci = 0; ci < k; ci++) {
		struct zns_run *r = run_snap[ci];
		unsigned int j;

		for (j = 0; j < r->n && ncand < max_cand; j++) {
			if (r->ents[j].pba >= vstart && r->ents[j].pba < vend) {
				cand[ncand].lba_blk = r->ents[j].lba_blk;
				cand[ncand].pba     = r->ents[j].pba;
				ncand++;
			}
		}
	}

	/* 스캔 끝 — 컴팩션 재개 허용 */
	spin_lock_irqsave(&c->map_lock, flags);
	c->compacting = false;
	spin_unlock_irqrestore(&c->map_lock, flags);

	/* ---- 블록별 이주 ---- */
	for (ci = 0; ci < ncand; ci++) {
		sector_t lba_blk = cand[ci].lba_blk;
		sector_t pba     = cand[ci].pba;
		sector_t new_pba;
		sector_t old;
		int up, retry;

		/* 이미 죽었으면 복사할 필요도 없다 (최적화, 판정은 아래 CAS) */
		spin_lock_irqsave(&c->map_lock, flags);
		if (map_lookup(c, lba_blk) != pba) {
			spin_unlock_irqrestore(&c->map_lock, flags);
			continue;
		}
		spin_unlock_irqrestore(&c->map_lock, flags);

		/* victim 데이터는 reset 전이므로 매핑이 죽었어도 읽기는 무해 */
		ret = zns_rw_block(c, pba, pg, REQ_OP_READ);
		if (ret)
			goto out;

		for (retry = 0; ; retry++) {
			spin_lock_irqsave(&c->map_lock, flags);

			/* 새 위치 확보 — 인덱스 자리부터 (부분 갱신 방지) */
			if (c->node_used + 1 > NODE_POOL_MAX &&
			    !mem_flush_locked(c)) {
				spin_unlock_irqrestore(&c->map_lock, flags);
				if (retry > 8) {
					ret = -ENOMEM;
					goto out;
				}
				zns_refill_spares(c);
				continue;
			}
			if (c->mem_n + 1 > MEMTABLE_MAX)
				mem_flush_locked(c);

			new_pba = zone_alloc_locked(c, SECTORS_PER_BLOCK, true);
			if (new_pba == PBA_UNMAPPED) {
				spin_unlock_irqrestore(&c->map_lock, flags);
				ret = -ENOSPC;	/* 예약분까지 소진 — 심각 */
				goto out;
			}
			spin_unlock_irqrestore(&c->map_lock, flags);
			break;
		}

		ret = zns_rw_block(c, new_pba, pg, REQ_OP_WRITE);
		if (ret) {
			/* 하위 wp 가 우리 할당 위치와 어긋났다 — 이 GC zone 은 더 못 쓴다 */
			spin_lock_irqsave(&c->map_lock, flags);
			if (c->active_gc_idx == (int)zidx_of(c, new_pba)) {
				c->zones[c->active_gc_idx].state = ZNS_ZONE_FULL;
				c->active_gc_idx = -1;
			}
			spin_unlock_irqrestore(&c->map_lock, flags);
			DMERR_LIMIT("gc: write error %d at %llu, gc zone sealed",
				    ret, (unsigned long long)new_pba);
			goto out;
		}

		/* 레코드 자리 — 링이 꽉 찼으면 먼저 커밋해 비운다 */
		while (READ_ONCE(c->rec_next) - READ_ONCE(c->rec_committed) >=
		       c->ring_cap) {
			ret = zns_log_commit(c, false);
			if (ret)
				goto out;
		}

		/*
		 * CAS: 복사하는 동안 사용자가 같은 LBA 를 덮어썼다면 매핑이
		 * 이미 다른 곳을 가리킨다 — 그 경우 복사본은 그냥 버린다
		 * (new_pba 는 valid 에 안 잡힌 죽은 블록으로 남고, 그 zone 이
		 * 나중에 GC 될 때 자연히 회수된다).
		 */
		spin_lock_irqsave(&c->map_lock, flags);
		if (map_lookup(c, lba_blk) == pba &&
		    zns_ring_used(c) < c->ring_cap) {
			up = mem_upsert(c, lba_blk, new_pba, &old);
			if (up < 0) {
				/* 위에서 자리를 확보했지만 unlock 사이에
				 * 다른 쓰기가 풀을 채웠을 수 있다 — 드묾.
				 * 이 블록 이주는 포기; victim valid 가 남아
				 * reset 은 안 하게 된다(안전한 실패). */
				spin_unlock_irqrestore(&c->map_lock, flags);
				zns_refill_spares(c);
				continue;
			}
			victim->valid--;
			zone_of(c, new_pba)->valid++;
			c->stat_migrated++;
			/* 데이터는 이미 썼다 — 다음 커밋에 바로 기록돼도 된다 */
			zns_rec_append_locked(c, lba_blk, new_pba, 1);
		}
		spin_unlock_irqrestore(&c->map_lock, flags);

		/* 이주도 MemTable 을 채워 run 을 만든다 */
		if ((ci & (MEMTABLE_MAX - 1)) == MEMTABLE_MAX - 1)
			zns_kick_compact(c);
	}

do_reset:
	spin_lock_irqsave(&c->map_lock, flags);
	if (victim->valid != 0 || victim->state != ZNS_ZONE_FULL) {
		/* 이주가 다 못 끝났다(위의 '안전한 실패' 경로). 다음 GC 몫. */
		spin_unlock_irqrestore(&c->map_lock, flags);
		DMWARN("gc: zone %d not fully evacuated (valid=%u)",
		       vidx, victim->valid);
		if (!ret)
			ret = -EAGAIN;
		goto out;
	}
	spin_unlock_irqrestore(&c->map_lock, flags);

	/*
	 * valid 0 이면 이 zone 으로 새로 remap 되는 읽기가 없다(map_lookup 이
	 * 더는 이 zone 을 돌려주지 않는다). 이미 remap 된 읽기와 남은 쓰기만
	 * 끝나면 reset 해도 된다. inflight 증가와 valid 검사는 둘 다 map_lock
	 * 아래라, valid 0 을 본 뒤에는 inflight 가 늘지 않는다.
	 */
	if (!wait_event_timeout(c->idle_wq,
				!atomic_read(&victim->rd_inflight) &&
				!atomic_read(&victim->wr_inflight),
				ZNS_QUIESCE_TIMEOUT)) {
		DMWARN("gc: zone %d still has I/O in flight, reset deferred", vidx);
		ret = -ETIMEDOUT;
		goto out;
	}

	/*
	 * 장치에 기록된 매핑도 이 zone 을 가리키지 않게 한 뒤에 reset 한다.
	 * 이주한 블록의 새 위치, 이 zone 의 블록을 죽인 덮어쓰기가 커밋돼야
	 * 크래시 뒤의 매핑이 reset 된 zone 을 가리키지 않는다.
	 */
	ret = zns_log_commit(c, false);
	if (ret) {
		DMERR_LIMIT("gc: commit before reset failed (%d), zone %d kept",
			    ret, vidx);
		goto out;
	}

	/*
	 * reset 은 sleepable 이라 락 밖. victim 은 FULL & valid 0 —
	 * 매핑에서 도달할 수 없으므로 새 참조가 생기지 않고, gc_lock 이
	 * 다른 GC 의 접근을 막는다.
	 */
	ret = zns_zone_mgmt(c->dev->bdev, REQ_OP_ZONE_RESET,
			    vstart, c->zone_sectors);
	if (ret) {
		DMERR("gc: zone reset failed on zone %d: %d", vidx, ret);
		goto out;
	}

	spin_lock_irqsave(&c->map_lock, flags);
	victim->state = ZNS_ZONE_FREE;
	victim->wp    = victim->start;
	c->free_zones++;
	c->stat_gc_runs++;
	spin_unlock_irqrestore(&c->map_lock, flags);

	DMDEBUG("gc: reclaimed zone %d", vidx);
	ret = 0;

out:
	kvfree(mem_snap);
	kfree(run_snap);
	kvfree(cand);
	if (pg)
		__free_page(pg);
	zns_kick_compact(c);	/* GC 에 양보했던 병합을 이어서 */
	return ret;
}

static void zns_gc_workfn(struct work_struct *w)
{
	struct zns_base_c *c = container_of(w, struct zns_base_c, gc_work);

	mutex_lock(&c->gc_lock);
	zns_gc_once(c, -1);
	mutex_unlock(&c->gc_lock);
}

static void zns_sub_workfn(struct work_struct *w);

/* ==================================================================
 * Constructor / Destructor
 * ================================================================== */

/* ---- 포맷과 복구 ---- */

struct zns_zinfo {
	sector_t	wp;
	u8		cond;
};

static int zns_report_cb(struct blk_zone *zone, unsigned int idx, void *data)
{
	struct zns_zinfo *zi = data;

	zi[idx].wp   = zone->wp;
	zi[idx].cond = zone->cond;
	return 0;
}

/* zone i 에서 장치에 실제로 쓰인 끝 */
static sector_t zns_zi_end(struct zns_base_c *c, const struct zns_zinfo *zi,
			   unsigned int i)
{
	if (zi[i].cond == BLK_ZONE_COND_FULL)
		return c->zones[i].start + c->zone_sectors;
	return zi[i].wp;
}

/* 인덱스를 비운다 */
static void zns_index_reset(struct zns_base_c *c)
{
	struct zns_run *r, *tmp;

	list_for_each_entry_safe(r, tmp, &c->runs, list) {
		list_del(&r->list);
		kvfree(r->ents);
		kfree(r);
	}
	c->nr_runs   = 0;
	c->memtable  = RB_ROOT;
	c->mem_n     = 0;
	c->node_used = 0;
}

/*
 * run 들을 하나로 합친다. keep_oldest 면 가장 오래된 run(체크포인트)은 두고
 * 그 위만 합친다. ctr 에서 혼자 돈다.
 */
static int zns_squash_runs(struct zns_base_c *c, bool keep_oldest)
{
	struct zns_run **snap, *merged, *r;
	struct zns_ent *ents;
	unsigned int k = 0, n = 0, total = 0, i;
	u64 dropped;

	if (c->nr_runs < (keep_oldest ? 3U : 2U))
		return 0;
	snap   = kcalloc(c->nr_runs, sizeof(*snap), GFP_KERNEL);
	merged = kzalloc(sizeof(*merged), GFP_KERNEL);
	if (!snap || !merged)
		goto nomem;
	list_for_each_entry(r, &c->runs, list) {
		if (keep_oldest && list_is_last(&r->list, &c->runs))
			break;
		snap[k++] = r;
		total += r->n;
	}
	ents = zns_merge_runs(snap, k, &n, &dropped);
	if (!ents && total)
		goto nomem;

	for (i = 0; i < k; i++) {
		list_del(&snap[i]->list);
		kvfree(snap[i]->ents);
		kfree(snap[i]);
	}
	merged->ents = ents;
	merged->n    = n;
	merged->cap  = total;
	list_add(&merged->list, &c->runs);	/* 남은 체크포인트 run 보다 최신 */
	c->nr_runs = c->nr_runs - k + 1;
	kfree(snap);
	return 0;
nomem:
	kfree(snap);
	kfree(merged);
	return -ENOMEM;
}

/* 로그 레코드의 블록 하나를 인덱스에 반영한다. ctr 에서 혼자 돈다. */
static int zns_replay(struct zns_base_c *c, sector_t lba_blk, sector_t pba)
{
	unsigned long flags;
	sector_t old;
	bool flushed;
	int up;

	for (;;) {
		spin_lock_irqsave(&c->map_lock, flags);
		if (c->node_used < NODE_POOL_MAX && c->mem_n < MEMTABLE_MAX) {
			up = mem_upsert(c, lba_blk, pba, &old);
			spin_unlock_irqrestore(&c->map_lock, flags);
			return up < 0 ? -ENOMEM : 0;
		}
		flushed = mem_flush_locked(c);
		spin_unlock_irqrestore(&c->map_lock, flags);

		zns_refill_spares(c);
		if (!c->spare || !c->spare_run)
			return -ENOMEM;
		/* 긴 로그는 run 을 많이 만든다 — 체크포인트 위쪽을 수시로 합친다 */
		if (flushed && c->nr_runs >= 64 && zns_squash_runs(c, true))
			return -ENOMEM;
	}
}

struct zns_recov {
	unsigned int	ckpt;		/* 체크포인트의 매핑 수 */
	unsigned int	log_blocks;
	u64		records;
	unsigned int	torn;		/* crc 가 안 맞아 건너뛴 메타 블록 */
	unsigned int	bad;		/* 범위를 벗어난 레코드 */
};

/*
 * 메타 zone idx 의 세대 gen 을 읽어 인덱스를 만든다: 체크포인트를 가장
 * 오래된 run 으로 놓고, 로그를 기록 순서대로 재생한다. 체크포인트가 온전
 * 하지 않으면 -EAGAIN (다른 메타 zone 을 본다).
 *
 * crc 가 안 맞는 블록은 건너뛴다. 크래시로 끝나지 못한 커밋의 블록이다 —
 * 그 커밋을 기다리던 flush/FUA 는 완료되지 않았으므로 버려도 계약이 지켜
 * 진다. 그 뒤에 이어 쓴 블록은 위치 순서가 곧 기록 순서라 그대로 재생한다.
 */
static int zns_load_gen(struct zns_base_c *c, unsigned int idx, u64 gen,
			const struct zns_zinfo *zi, struct zns_recov *st)
{
	unsigned int mz = zns_md_zone(c, idx);
	sector_t s = c->zones[mz].start;
	sector_t end = zns_zi_end(c, zi, mz);
	sector_t data_end = (sector_t)c->data_zones * c->zone_sectors;
	sector_t ck_cap = max_t(sector_t, c->total_blocks, 1);
	unsigned int ckpt_blocks = 0, ckpt_n = 0, i, j;
	struct zns_ent *ck;
	u32 crc_all = ~0U;
	u64 last_seq = 0;
	bool ckpt_done = false;
	int ret = 0;

	zns_index_reset(c);
	memset(st, 0, sizeof(*st));
	ck = zns_kvmalloc_array(ck_cap, sizeof(*ck), GFP_KERNEL);
	if (!ck)
		return -ENOMEM;

	/* 4 KiB 블록 중간에서 끊긴 꼬리는 읽지 않는다 */
	while (s < (end & ~(sector_t)(SECTORS_PER_BLOCK - 1))) {
		unsigned int nb = (unsigned int)min_t(sector_t,
						      (end - s) >> BLOCK_SHIFT,
						      ZNS_MD_BATCH);	/* ≥ 1 */

		ret = zns_md_io(c, s, nb, REQ_OP_READ);
		if (ret)
			goto out;

		for (i = 0; i < nb; i++) {
			void *blk = zns_md_blk(c, i);
			struct zns_md_hdr *h = blk;
			u32 type = zns_md_check(blk, gen);
			u32 cnt = le32_to_cpu(h->count);

			if (!type) {
				st->torn++;
				continue;
			}
			last_seq = le64_to_cpu(h->seq);

			if (type == ZNS_MD_CKPT) {
				struct zns_md_ent *e = blk + sizeof(*h);

				if (ckpt_done || cnt > ZNS_ENTS_PER_BLK ||
				    le64_to_cpu(h->aux) != ckpt_n ||
				    ckpt_n + cnt > ck_cap) {
					ret = -EAGAIN;
					goto out;
				}
				for (j = 0; j < cnt; j++) {
					ck[ckpt_n + j].lba_blk = le32_to_cpu(e[j].lba);
					ck[ckpt_n + j].pba =
						(sector_t)le32_to_cpu(e[j].pba) << BLOCK_SHIFT;
				}
				crc_all = crc32_le(crc_all, (void *)e, cnt * sizeof(*e));
				ckpt_n += cnt;
				ckpt_blocks++;
			} else if (type == ZNS_MD_CKPT_END) {
				struct zns_run *run;

				if (ckpt_done || cnt != ckpt_blocks ||
				    le64_to_cpu(h->aux) != ckpt_n ||
				    le32_to_cpu(*(__le32 *)(blk + sizeof(*h))) != crc_all) {
					ret = -EAGAIN;
					goto out;
				}
				ckpt_done = true;
				st->ckpt = ckpt_n;
				if (!ckpt_n)
					continue;
				run = kzalloc(sizeof(*run), GFP_KERNEL);
				if (!run) {
					ret = -ENOMEM;
					goto out;
				}
				run->ents = ck;
				run->n    = ckpt_n;
				run->cap  = (unsigned int)ck_cap;
				list_add_tail(&run->list, &c->runs);	/* 가장 오래됨 */
				c->nr_runs++;
				ck = NULL;
			} else if (type == ZNS_MD_LOG) {
				struct zns_md_rec *rr = blk + sizeof(*h);

				if (!ckpt_done || cnt > ZNS_RECS_PER_BLK) {
					ret = -EAGAIN;
					goto out;
				}
				for (j = 0; j < cnt; j++) {
					u64 lba = le32_to_cpu(rr[j].lba);
					u64 pba = le32_to_cpu(rr[j].pba);
					u64 len = le32_to_cpu(rr[j].len);
					u64 b;

					if (!len || lba + len > c->total_blocks ||
					    ((pba + len) << BLOCK_SHIFT) > data_end) {
						st->bad++;
						continue;
					}
					for (b = 0; b < len; b++) {
						ret = zns_replay(c, lba + b,
								 (pba + b) << BLOCK_SHIFT);
						if (ret)
							goto out;
					}
					st->records++;
				}
				st->log_blocks++;
			}
			/* SB 는 고를 때 확인했다 */
		}
		s += (sector_t)nb << BLOCK_SHIFT;
		cond_resched();
	}
	if (!ckpt_done) {
		ret = -EAGAIN;
		goto out;
	}

	/*
	 * 이어 쓰기는 하위 wp 에서 — 끝나지 못한 블록 뒤로. wp 가 4 KiB 블록
	 * 중간이면(512 B 장치의 전원 차단) 이후 블록이 전부 어긋나 다음 복구에서
	 * 버려지므로, 이 zone 은 다 쓴 것으로 쳐서 첫 커밋이 체크포인트로 옮겨
	 * 가게 한다.
	 */
	c->md_cur        = idx;
	c->md_gen        = gen;
	c->md_seq        = last_seq + 1;
	c->md_wp         = (end & (SECTORS_PER_BLOCK - 1))
			   ? c->zones[mz].start + c->zone_sectors : end;
	c->md_log_blocks = st->log_blocks;
out:
	kvfree(ck);
	return ret;
}

/*
 * 메타 zone 에서 인덱스를 되살리고, valid 와 zone 상태를 하위 장치의 wp 로
 * 다시 만든다. 반환: 0 복구, 1 메타데이터 없음, 음수 실패.
 */
static int zns_recover(struct zns_base_c *c, const struct zns_zinfo *zi)
{
	ktime_t t0 = ktime_get();
	u64 gen[ZNS_META_ZONES] = { 0 };
	unsigned int order[ZNS_META_ZONES] = { 0, 1 };
	unsigned int i, mapped = 0, bad_map = 0;
	struct zns_recov st = { 0 };
	unsigned long flags;
	struct zns_run *r;
	bool flushed = true;
	int ret;

	for (i = 0; i < ZNS_META_ZONES; i++) {
		unsigned int mz = zns_md_zone(c, i);
		struct zns_md_sb *sb;
		void *blk;

		if (zns_zi_end(c, zi, mz) == c->zones[mz].start)
			continue;
		ret = zns_md_io(c, c->zones[mz].start, 1, REQ_OP_READ);
		if (ret)
			return ret;
		blk = zns_md_blk(c, 0);
		if (zns_md_check(blk, 0) != ZNS_MD_SB)
			continue;
		sb = blk + sizeof(struct zns_md_hdr);
		if (le32_to_cpu(sb->version) != ZNS_MD_VERSION ||
		    le32_to_cpu(sb->nr_zones) != c->nr_zones ||
		    le64_to_cpu(sb->zone_sectors) != c->zone_sectors) {
			DMERR("recover: metadata in zone %u is for another geometry", mz);
			return -EINVAL;
		}
		if (le64_to_cpu(sb->logical_blocks) != c->total_blocks) {
			DMERR("recover: formatted for %llu sectors, table has %llu",
			      (unsigned long long)le64_to_cpu(sb->logical_blocks) << BLOCK_SHIFT,
			      (unsigned long long)c->total_blocks << BLOCK_SHIFT);
			return -EINVAL;
		}
		gen[i] = le64_to_cpu(((struct zns_md_hdr *)blk)->gen);
	}
	if (!gen[0] && !gen[1])
		return 1;

	if (gen[1] > gen[0]) {
		order[0] = 1;
		order[1] = 0;
	}
	ret = -EAGAIN;
	for (i = 0; i < ZNS_META_ZONES && ret == -EAGAIN; i++) {
		if (!gen[order[i]])
			continue;
		ret = zns_load_gen(c, order[i], gen[order[i]], zi, &st);
		if (ret == -EAGAIN)
			DMWARN("recover: generation %llu in zone %u is incomplete",
			       (unsigned long long)gen[order[i]],
			       zns_md_zone(c, order[i]));
	}
	if (ret) {
		zns_index_reset(c);
		if (ret == -EAGAIN) {
			DMERR("recover: no complete checkpoint");
			ret = -EINVAL;
		}
		return ret;
	}

	/* 인덱스를 run 하나로 */
	spin_lock_irqsave(&c->map_lock, flags);
	if (c->mem_n)
		flushed = mem_flush_locked(c);
	spin_unlock_irqrestore(&c->map_lock, flags);
	zns_refill_spares(c);
	if (!flushed || zns_squash_runs(c, false))
		return -ENOMEM;

	/*
	 * valid 는 매핑에서, zone 상태는 하위 wp 에서. 쓰이지 않은 위치를 가리키는
	 * 매핑은 크래시 일관성이 깨진 흔적이다 — 세어서 알리고 인덱스에서 뺀다
	 * (남겨 두면 그 위치에 새로 쓰인 다른 LBA 와 겹친다). 그 LBA 는 읽으면 0.
	 */
	r = list_first_entry_or_null(&c->runs, struct zns_run, list);
	if (r) {
		unsigned int keep = 0;

		for (i = 0; i < r->n; i++) {
			sector_t pba = r->ents[i].pba;
			unsigned int z = zidx_of(c, pba);

			if (z >= c->data_zones ||
			    pba + SECTORS_PER_BLOCK > zns_zi_end(c, zi, z)) {
				bad_map++;
				continue;
			}
			c->zones[z].valid++;
			r->ents[keep++] = r->ents[i];
		}
		r->n = keep;
		mapped = keep;
	}

	c->free_zones = 0;
	c->active_user_idx = -1;
	c->active_gc_idx   = -1;
	for (i = 0; i < c->data_zones; i++) {
		struct zns_zone *z = &c->zones[i];
		sector_t end = zns_zi_end(c, zi, i);

		z->wp     = end;
		z->sub_wp = end;
		if (end == z->start) {
			z->state = ZNS_ZONE_FREE;
			c->free_zones++;
		} else if (end >= z->start + c->zone_sectors ||
			   (end & (SECTORS_PER_BLOCK - 1))) {
			/* 꽉 찼거나, 4 KiB 블록 중간에서 끊겼다(512 B 장치의 전원 차단)
			 * — 블록 단위 할당을 이어 갈 수 없으니 마감한다 */
			z->state = ZNS_ZONE_FULL;
		} else if (c->active_user_idx < 0) {
			z->state = ZNS_ZONE_ACTIVE;	/* 하위 wp 에서 이어 쓴다 */
			c->active_user_idx = i;
		} else if (c->active_gc_idx < 0) {
			z->state = ZNS_ZONE_ACTIVE;
			c->active_gc_idx = i;
		} else {
			z->state = ZNS_ZONE_FULL;	/* 자투리는 GC 때 회수 */
		}
	}

	c->stat_flushes = 0;
	c->stat_dropped = 0;
	c->recover_ms = (u64)ktime_ms_delta(ktime_get(), t0);
	DMINFO("recover: gen %llu in zone %u, checkpoint %u + log %u blocks (%llu records), %u mappings, %llu ms",
	       (unsigned long long)c->md_gen, zns_md_zone(c, c->md_cur),
	       st.ckpt, st.log_blocks, (unsigned long long)st.records,
	       mapped, (unsigned long long)c->recover_ms);
	if (st.torn || st.bad || bad_map)
		DMWARN("recover: %u torn metadata blocks skipped, %u bad records, %u mappings to unwritten space",
		       st.torn, st.bad, bad_map);
	return 0;
}

/* 장치 전체를 지우고 세대 1 을 연다 */
static int zns_format(struct zns_base_c *c)
{
	unsigned int i;
	int ret;

	ret = zns_zone_mgmt(c->dev->bdev, REQ_OP_ZONE_RESET, 0,
			    c->total_sectors);
	if (ret)
		return ret;
	for (i = 0; i < c->data_zones; i++) {
		c->zones[i].state  = ZNS_ZONE_FREE;
		c->zones[i].wp     = c->zones[i].start;
		c->zones[i].sub_wp = c->zones[i].start;
		c->zones[i].valid  = 0;
	}
	c->free_zones = c->data_zones;
	return zns_md_write_gen(c, zns_md_zone(c, 0), 1, NULL, 0);
}

static void zns_free(struct dm_target *ti, struct zns_base_c *c)
{
	unsigned int i;

	zns_index_reset(c);
	bioset_exit(&c->gc_bs);
	bioset_exit(&c->md_bs);
	bioset_exit(&c->fua_bs);
	for (i = 0; i < ZNS_MD_BATCH; i++)
		if (c->md_pages[i])
			__free_page(c->md_pages[i]);
	kvfree(c->ring);
	kvfree(c->zones);
	kvfree(c->node_pool);
	kvfree(c->spare);
	kfree(c->spare_run);
	if (c->dev)
		dm_put_device(ti, c->dev);
	kfree(c);
}

static int zns_base_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct zns_base_c *c;
	struct zns_zinfo *zi = NULL;
	unsigned int max_util = ZNS_DEFAULT_MAX_UTIL;
	bool format = false, recovered = false;
	u64 max_blocks;
	sector_t max_len;
	unsigned int i;
	char dummy;
	int ret;

	if (argc < 1) {
		ti->error = "usage: <dev> [format] [max_util:<permille>]";
		return -EINVAL;
	}
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "format"))
			continue;
		if (sscanf(argv[i], "max_util:%u%c", &max_util, &dummy) != 1 ||
		    max_util < 1 || max_util > 999) {
			ti->error = "bad argument (format | max_util:<1..999>)";
			return -EINVAL;
		}
	}
	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "format"))
			format = true;

	if (ti->len & (SECTORS_PER_BLOCK - 1)) {
		ti->error = "length must be a multiple of 4 KiB";
		return -EINVAL;
	}

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		ti->error = "out of memory";
		return -ENOMEM;
	}

	spin_lock_init(&c->map_lock);
	mutex_init(&c->gc_lock);
	mutex_init(&c->log_lock);
	INIT_WORK(&c->gc_work, zns_gc_workfn);
	INIT_WORK(&c->sub_work, zns_sub_workfn);
	INIT_WORK(&c->compact_work, zns_compact_workfn);
	INIT_WORK(&c->log_work, zns_log_workfn);
	INIT_DELAYED_WORK(&c->log_tick, zns_log_tickfn);
	init_waitqueue_head(&c->compact_done);
	init_waitqueue_head(&c->admit_wq);
	init_waitqueue_head(&c->idle_wq);
	init_waitqueue_head(&c->ep_wq);
	init_waitqueue_head(&c->ring_wq);
	bio_list_init(&c->sub_list);
	bio_list_init(&c->flush_list);
	bio_list_init(&c->fua_list);
	atomic64_set(&c->queued_bytes, 0);
	atomic_set(&c->ep_inflight[0], 0);
	atomic_set(&c->ep_inflight[1], 0);
	c->memtable = RB_ROOT;
	INIT_LIST_HEAD(&c->runs);
	c->active_user_idx = -1;
	c->active_gc_idx   = -1;

	/* 쓰기 진행이 이 워커들에 달려 있으므로 메모리 압박에서도 굴러야 한다 */
	c->gc_wq      = alloc_workqueue("zns-gc", WQ_MEM_RECLAIM, 1);
	c->sub_wq     = alloc_ordered_workqueue("zns-sub", WQ_MEM_RECLAIM | WQ_HIGHPRI);
	c->compact_wq = alloc_ordered_workqueue("zns-compact", WQ_MEM_RECLAIM);
	c->log_wq     = alloc_ordered_workqueue("zns-log", WQ_MEM_RECLAIM | WQ_HIGHPRI);
	if (!c->gc_wq || !c->sub_wq || !c->compact_wq || !c->log_wq) {
		ti->error = "cannot create workqueues";
		ret = -ENOMEM;
		goto err;
	}

	ret = bioset_init(&c->gc_bs, BIO_POOL_SIZE, 0, BIOSET_NEED_BVECS);
	if (!ret)
		ret = bioset_init(&c->md_bs, BIO_POOL_SIZE, 0, BIOSET_NEED_BVECS);
	if (!ret)
		ret = bioset_init(&c->fua_bs, BIO_POOL_SIZE, 0, 0);
	if (ret) {
		ti->error = "cannot create biosets";
		goto err;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &c->dev);
	if (ret) {
		c->dev = NULL;
		ti->error = "failed to open underlying device";
		goto err;
	}

	c->zone_sectors = bdev_zone_sectors(c->dev->bdev);
	if (!c->zone_sectors || (c->zone_sectors & (SECTORS_PER_BLOCK - 1))) {
		ti->error = "underlying device is not zoned";
		ret = -EINVAL;
		goto err;
	}

	if (bdev_logical_block_size(c->dev->bdev) > (SECTORS_PER_BLOCK << SECTOR_SHIFT)) {
		ti->error = "underlying logical block size is larger than 4 KiB";
		ret = -EINVAL;
		goto err;
	}

	if (!zns_zone_order_backend_ok(c->dev->bdev)) {
		ti->error = "set the zoned device's I/O scheduler to mq-deadline";
		ret = -EINVAL;
		goto err;
	}

	/* 하위 zone 전부를 쓴다 — 앞은 데이터, 뒤 ZNS_META_ZONES 개는 메타 */
	c->nr_zones = (unsigned int)(bdev_nr_sectors(c->dev->bdev) / c->zone_sectors);
	if (c->nr_zones < ZNS_META_ZONES + 4) {
		ti->error = "underlying device needs at least 6 zones";
		ret = -EINVAL;
		goto err;
	}
	c->data_zones    = c->nr_zones - ZNS_META_ZONES;
	c->total_sectors = (sector_t)c->nr_zones * c->zone_sectors;
	if ((c->total_sectors >> BLOCK_SHIFT) > U32_MAX) {
		ti->error = "underlying device is larger than 16 TiB";
		ret = -EINVAL;
		goto err;
	}

	/* 노출 용량 상한 — 머리 주석의 '노출 용량' */
	c->total_blocks = ti->len >> BLOCK_SHIFT;
	max_blocks = (u64)(c->data_zones - 2) * (c->zone_sectors >> BLOCK_SHIFT) *
		     max_util / 1000;
	if (!c->total_blocks || c->total_blocks > max_blocks) {
		DMERR("ctr: %llu sectors requested, at most %llu with max_util:%u",
		      (unsigned long long)ti->len,
		      (unsigned long long)max_blocks << BLOCK_SHIFT, max_util);
		ti->error = "logical size exceeds the over-provisioning limit";
		ret = -EINVAL;
		goto err;
	}

	/*
	 * 메타 zone 하나에 최대 체크포인트(노출 블록 전부)와 링 하나 분량의 로그가
	 * 들어가야 한다. 안 들어가면 체크포인트가 zone 을 넘치거나, 로그 자리가
	 * 모자라 커밋마다 체크포인트를 쓰게 된다.
	 */
	if (2 + DIV_ROUND_UP_ULL(c->total_blocks, ZNS_ENTS_PER_BLK) +
	    DIV_ROUND_UP_ULL(1ULL << ZNS_RING_ORDER, ZNS_RECS_PER_BLK) >
	    (c->zone_sectors >> BLOCK_SHIFT)) {
		ti->error = "a checkpoint of this size does not fit in one metadata zone";
		ret = -EINVAL;
		goto err;
	}

	c->ring_cap  = 1ULL << ZNS_RING_ORDER;
	c->zones     = kvcalloc(c->nr_zones, sizeof(*c->zones), GFP_KERNEL);
	c->node_pool = kvmalloc_array(NODE_POOL_MAX, sizeof(*c->node_pool), GFP_KERNEL);
	c->spare     = kvmalloc_array(NODE_POOL_MAX, sizeof(*c->spare), GFP_KERNEL);
	c->spare_run = kzalloc(sizeof(*c->spare_run), GFP_KERNEL);
	c->ring      = kvmalloc_array(c->ring_cap, sizeof(*c->ring), GFP_KERNEL);
	zi           = kvcalloc(c->nr_zones, sizeof(*zi), GFP_KERNEL);
	if (!c->zones || !c->node_pool || !c->spare || !c->spare_run ||
	    !c->ring || !zi) {
		ti->error = "cannot allocate index/zone state";
		ret = -ENOMEM;
		goto err;
	}
	for (i = 0; i < ZNS_MD_BATCH; i++) {
		c->md_pages[i] = alloc_page(GFP_KERNEL);
		if (!c->md_pages[i]) {
			ti->error = "cannot allocate metadata buffer";
			ret = -ENOMEM;
			goto err;
		}
	}

	for (i = 0; i < c->nr_zones; i++) {
		c->zones[i].start  = (sector_t)i * c->zone_sectors;
		c->zones[i].wp     = c->zones[i].start;
		c->zones[i].sub_wp = c->zones[i].start;
		c->zones[i].state  = (i < c->data_zones) ? ZNS_ZONE_FREE
							 : ZNS_ZONE_META;
		atomic_set(&c->zones[i].wr_inflight, 0);
		atomic_set(&c->zones[i].rd_inflight, 0);
	}
	c->free_zones = c->data_zones;

	ret = blkdev_report_zones(c->dev->bdev, 0, c->nr_zones, zns_report_cb, zi);
	if (ret != (int)c->nr_zones) {
		ti->error = "cannot report zones";
		ret = ret < 0 ? ret : -EIO;
		goto err;
	}

	if (!format) {
		ret = zns_recover(c, zi);
		if (ret == 1) {
			/* 빈 장치는 그냥 연다. 무언가 있는데 메타데이터가 없으면 지우지 않는다. */
			for (i = 0; i < c->nr_zones; i++) {
				if (zns_zi_end(c, zi, i) != c->zones[i].start) {
					ti->error = "no dm-zns metadata (add 'format' to initialize; erases the device)";
					ret = -EINVAL;
					goto err;
				}
			}
			format = true;
		} else if (ret) {
			ti->error = "metadata recovery failed";
			goto err;
		} else {
			recovered = true;
		}
	}
	if (format) {
		ret = zns_format(c);
		if (ret) {
			ti->error = "format failed";
			goto err;
		}
	}
	kvfree(zi);
	zi = NULL;

	ti->private          = c;
	ti->num_flush_bios   = 1;
	/* 하위가 write-through 여도 flush/FUA 를 받아 매핑 커밋의 시점으로 쓴다 */
	ti->flush_supported  = true;
	ti->per_io_data_size = sizeof(struct zns_pio);
	zns_set_accounts_remapped(ti);

	/* discard 미지원 — 하위로 넘기면 안 되고, 매핑 무효화로 구현해야 한다 */
	ti->num_discard_bios = 0;

	/*
	 * bio 하나가 (a) MemTable 한 세대, (b) zone 하나를 넘지 않게 한다.
	 * (a) 를 넘으면 flush 를 해도 자리가 안 나 영영 되밀리고,
	 * (b) 를 넘으면 단일 할당이 zone 에 들어갈 수 없다.
	 */
	max_len = min_t(sector_t, MEMTABLE_MAX << BLOCK_SHIFT, c->zone_sectors);
	ret = dm_set_target_max_io_len(ti, max_len);
	if (ret) {
		ti->error = "cannot set max io len";
		goto err;
	}

	queue_delayed_work(system_wq, &c->log_tick, ZNS_LOG_TICK);

	DMINFO("ctr: '%s', %u data + %u metadata zones x %llu sectors, %llu sectors exposed (max_util %u), %s",
	       argv[0], c->data_zones, ZNS_META_ZONES,
	       (unsigned long long)c->zone_sectors, (unsigned long long)ti->len,
	       max_util, recovered ? "recovered" : "formatted");
	return 0;

err:
	kvfree(zi);
	if (c->log_wq)
		destroy_workqueue(c->log_wq);
	if (c->compact_wq)
		destroy_workqueue(c->compact_wq);
	if (c->sub_wq)
		destroy_workqueue(c->sub_wq);
	if (c->gc_wq)
		destroy_workqueue(c->gc_wq);
	zns_free(ti, c);
	return ret;
}

static void zns_base_dtr(struct dm_target *ti)
{
	struct zns_base_c *c = ti->private;

	cancel_delayed_work_sync(&c->log_tick);

	/* dtr 시점엔 진행 중 I/O 가 없다(DM 보장). 남은 작업만 끝내고 내린다. */
	destroy_workqueue(c->sub_wq);
	destroy_workqueue(c->gc_wq);
	destroy_workqueue(c->compact_wq);	/* GC 뒤 — GC 가 컴팩션을 기다릴 수 있다 */

	/* 마지막 커밋 — 다음 생성 때 여기까지 복구된다. crash 흉내 뒤에는 쓰지 않는다. */
	zns_log_commit(c, false);
	destroy_workqueue(c->log_wq);

	zns_free(ti, c);
	DMINFO("dtr: target detached");
}

/* ==================================================================
 * Write I/O
 * ================================================================== */

static void zns_fua_child_endio(struct bio *child);

/*
 * 사용자 쓰기의 유일한 제출자.
 *
 * .map 은 PBA 할당과 같은 map_lock 구간에서 bio 를 sub_list 에 넣으므로
 * 목록 순서가 곧 할당 순서다. 이 워커는 목록을 그 순서대로 하위에 보낸다.
 * 워커 컨텍스트에는 current->bio_list 가 없어 submit 이 블록 계층 입구
 * (5.15: mq-deadline 삽입, 6.10+: zone write plug)까지 즉시 진행되므로,
 * 장치에 도착하는 순서도 할당 순서와 같다. .map 에서 바로 제출하면 제출이
 * .map 이 끝날 때까지 미뤄져, 발행자가 여럿일 때 나중에 할당받은 쓰기가
 * 먼저 도착한다. FUA 쓰기는 DM bio 대신 그 복제본이 목록에 있다.
 */
static void zns_sub_workfn(struct work_struct *w)
{
	struct zns_base_c *c = container_of(w, struct zns_base_c, sub_work);
	struct bio_list list;
	struct blk_plug plug;
	unsigned long flags;
	struct bio *bio;

	for (;;) {
		spin_lock_irqsave(&c->map_lock, flags);
		list = c->sub_list;
		bio_list_init(&c->sub_list);
		spin_unlock_irqrestore(&c->map_lock, flags);

		if (bio_list_empty(&list))
			return;

		blk_start_plug(&plug);
		while ((bio = bio_list_pop(&list))) {
			bool clone = bio->bi_end_io == zns_fua_child_endio;
			struct bio *dm_bio = clone ? bio->bi_private : bio;
			struct zns_zone *z = &c->zones[zns_pio(dm_bio)->zidx];

			/* 제출 후에는 bio 를 건드리지 않는다 — 완료가 먼저 올 수 있다 */
			if (unlikely(bio->bi_iter.bi_sector != z->sub_wp)) {
				c->stat_order_viol++;
				DMERR_LIMIT("submit order: sector %llu, expected %llu",
					    (unsigned long long)bio->bi_iter.bi_sector,
					    (unsigned long long)z->sub_wp);
			}
			z->sub_wp = bio_end_sector(bio);
			if (clone)
				zns_submit_clone(dm_bio, bio);
			else
				zns_submit_remapped(bio);
		}
		blk_finish_plug(&plug);
		cond_resched();
	}
}

/* 입장 제어. 할당 전에, 락 없이 부른다. */
static void zns_admit(struct zns_base_c *c, struct bio *bio)
{
	u64 limit = ZNS_QUEUE_MAX_BYTES;

	if (bio->bi_opf & (REQ_SYNC | REQ_META | REQ_PRIO))
		limit *= 2;
	wait_event(c->admit_wq, (u64)atomic64_read(&c->queued_bytes) < limit);
}

/* 새 할당을 해도 되는가: 체크포인트 스냅샷 중이 아니고 레코드 링에 자리가 있다 */
static bool zns_may_alloc(struct zns_base_c *c)
{
	return !READ_ONCE(c->frozen) &&
	       READ_ONCE(c->rec_next) - READ_ONCE(c->rec_committed) +
	       ZNS_RING_GC_HEADROOM < c->ring_cap;
}

static int zns_handle_write(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c = ti->private;
	struct zns_pio *pio  = zns_pio(bio);
	sector_t lba         = dm_target_offset(ti, bio->bi_iter.bi_sector);
	sector_t len_sectors = bio_sectors(bio);
	sector_t block_idx   = lba >> BLOCK_SHIFT;
	sector_t nr_blocks;
	sector_t pba;
	sector_t i;
	unsigned long flags;
	bool fua = bio->bi_opf & REQ_FUA;
	bool need_refill, low_free, ring_busy;
	struct bio *clone = NULL;
	unsigned int gc_tries = 0, mem_tries = 0;
	blk_status_t err;

	if (len_sectors == 0) {
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* 매핑 단위는 4 KiB 블록이다. 위쪽에 4 KiB 논리 블록으로 광고하므로
	 * 정상이라면 여기 오지 않는다. */
	if ((lba | len_sectors) & (SECTORS_PER_BLOCK - 1)) {
		DMERR_LIMIT("unaligned write: sector %llu, %llu sectors",
			    (unsigned long long)lba, (unsigned long long)len_sectors);
		err = BLK_STS_IOERR;
		goto err_out;
	}
	nr_blocks = len_sectors >> BLOCK_SHIFT;
	if (block_idx + nr_blocks > c->total_blocks) {
		err = BLK_STS_IOERR;
		goto err_out;
	}

	/* 메타 기록이 멈췄으면(fail-stop) 더 받지 않는다 — zns_write_done 참고 */
	if (unlikely(READ_ONCE(c->md_err))) {
		err = BLK_STS_IOERR;
		goto err_out;
	}

	/*
	 * FUA 쓰기는 자기 레코드가 커밋된 뒤에 완료해야 한다. 데이터는 복제본
	 * 으로 보내고, 복제본이 끝나면 DM bio 를 커밋 워커에 넘긴다.
	 */
	if (fua) {
		clone = zns_bio_clone(bio, GFP_NOIO, &c->fua_bs);
		if (!clone) {
			err = BLK_STS_RESOURCE;
			goto err_out;
		}
	}

	zns_admit(c, bio);

again:
	spin_lock_irqsave(&c->map_lock, flags);

	/* 체크포인트 스냅샷 중이거나 레코드 링이 차면 커밋을 기다린다 */
	if (c->frozen || zns_ring_used(c) + ZNS_RING_GC_HEADROOM >= c->ring_cap) {
		c->commit_req = true;
		spin_unlock_irqrestore(&c->map_lock, flags);
		queue_work(c->log_wq, &c->log_work);
		wait_event(c->ring_wq, zns_may_alloc(c) ||
			   READ_ONCE(c->crashed) || READ_ONCE(c->md_err));
		if (READ_ONCE(c->crashed) || READ_ONCE(c->md_err)) {
			err = BLK_STS_IOERR;
			goto err_out;
		}
		goto again;
	}

	/* 인덱스 자리 확인 — 부분 갱신 방지를 위해 통째로 미리 */
	if (c->node_used + nr_blocks > NODE_POOL_MAX &&
	    !mem_flush_locked(c)) {
		spin_unlock_irqrestore(&c->map_lock, flags);
		if (++mem_tries > 8) {
			DMERR_LIMIT("memtable full and cannot flush");
			err = BLK_STS_RESOURCE;
			goto err_out;
		}
		zns_refill_spares(c);
		goto again;
	}
	if (c->mem_n + nr_blocks > MEMTABLE_MAX)
		mem_flush_locked(c);

	pba = zone_alloc_locked(c, len_sectors, false);
	if (pba == PBA_UNMAPPED) {
		u64 runs_before = c->stat_gc_runs;

		spin_unlock_irqrestore(&c->map_lock, flags);

		/*
		 * free zone 이 없다 — 워커에 GC 를 맡기고 완료를 기다린 뒤
		 * 재시도. 여기서 zns_gc_once() 를 직접 부르면 안 되는 이유는
		 * 파일 머리 주석(current->bio_list 자기 교착) 참고.
		 *
		 * GC 가 zone 을 회수하고 있는 동안은 계속 기다린다. 쓰는 쪽이
		 * 여럿이면 회수된 공간을 다른 쪽이 먼저 가져갈 수 있어, 고정
		 * 횟수로 끊으면 공간이 생기고 있는데도 ENOSPC 가 난다. 회수가
		 * 연달아 WRITE_GC_RETRIES 번 없을 때만 포기한다.
		 */
		if (gc_tries >= WRITE_GC_RETRIES) {
			DMERR_LIMIT("out of space: no reclaimable zone (valid data ~%u zones)",
				    c->data_zones - c->free_zones);
			err = BLK_STS_NOSPC;
			goto err_out;
		}
		queue_work(c->gc_wq, &c->gc_work);
		flush_work(&c->gc_work);

		spin_lock_irqsave(&c->map_lock, flags);
		gc_tries = (c->stat_gc_runs == runs_before) ? gc_tries + 1 : 0;
		spin_unlock_irqrestore(&c->map_lock, flags);
		goto again;
	}

	for (i = 0; i < nr_blocks; i++) {
		sector_t idx = block_idx + i;
		sector_t new_blk_pba = pba + (i << BLOCK_SHIFT);
		sector_t old = PBA_UNMAPPED;
		int up;

		up = mem_upsert(c, idx, new_blk_pba, &old);
		if (up < 0) {
			WARN_ON_ONCE(1);	/* 자리 미리 확인했음 */
			break;
		}
		if (up == 0)
			old = runs_lookup(c, idx);

		/*
		 * valid 회계 — GC 의 판단 근거.
		 * 덮어쓰기로 죽는 옛 블록의 zone 은 그 자리에서 감소시킨다.
		 * (컴팩션 때까지 미루면 GC 가 victim 을 잘못 고른다.)
		 */
		if (old != PBA_UNMAPPED)
			zone_of(c, old)->valid--;
		zone_of(c, new_blk_pba)->valid++;
	}

	need_refill = (!c->spare || !c->spare_run);
	low_free = (c->free_zones <= GC_RESERVED_ZONES + 1);

	/*
	 * 제출 대기열에 넣는다 — 할당과 같은 락 구간이라 목록 순서 = 할당 순서.
	 * 락을 푸는 순간 워커가 제출하고 완료까지 갈 수 있으므로, bio 와 pio 는
	 * 여기서 전부 채우고 이후로는 건드리지 않는다.
	 *
	 * 매핑 레코드와 epoch 도 같은 구간에서 붙인다. 커밋은 이 쓰기가 끝난
	 * 뒤에야 이 레코드를 기록한다.
	 */
	pio->c       = c;
	pio->zidx    = (int)zidx_of(c, pba);
	pio->write   = true;
	pio->fua     = fua;
	pio->bytes   = (unsigned int)(len_sectors << SECTOR_SHIFT);
	pio->rec_seq = zns_rec_append_locked(c, block_idx, pba, nr_blocks);
	pio->epoch   = (u8)(c->epoch & 1);
	atomic_inc(&c->ep_inflight[pio->epoch]);
	atomic_inc(&c->zones[pio->zidx].wr_inflight);
	atomic64_add(pio->bytes, &c->queued_bytes);
	ring_busy = zns_ring_used(c) > c->ring_cap / 2;
	if (ring_busy)
		c->commit_req = true;

	if (clone) {
		bio_set_dev(clone, c->dev->bdev);
		clone->bi_iter.bi_sector = pba;
		/* 내구성은 커밋이 맡는다. 5.15 에서 FUA 는 zone 순서도 깨뜨린다. */
		clone->bi_opf &= ~(REQ_FUA | REQ_PREFLUSH);
		clone->bi_ioprio = 0;
		clone->bi_end_io = zns_fua_child_endio;
		clone->bi_private = bio;
		bio_list_add(&c->sub_list, clone);
	} else {
		bio_set_dev(bio, c->dev->bdev);
		bio->bi_iter.bi_sector = pba;
#if ZNS_SCHED_ZONE_LOCK
		/*
		 * 5.15 의 mq-deadline 이 zone 순서를 지키는 것은 같은 우선순위 큐
		 * 안에서뿐이다. 우선순위가 섞이면(ionice, prioclass) 높은 쪽이 먼저
		 * 나간다.
		 */
		bio->bi_ioprio = 0;
#endif
		bio_list_add(&c->sub_list, bio);
	}

	spin_unlock_irqrestore(&c->map_lock, flags);

	queue_work(c->sub_wq, &c->sub_work);

	/* sleepable 뒷정리 — 제출을 늦추지 않도록 큐에 넣은 뒤에 */
	if (ring_busy)
		queue_work(c->log_wq, &c->log_work);
	if (need_refill)
		zns_refill_spares(c);
	if (low_free)
		queue_work(c->gc_wq, &c->gc_work);	/* 선제 GC — 비동기 */
	zns_kick_compact(c);

	return DM_MAPIO_SUBMITTED;

err_out:
	if (clone)
		bio_put(clone);
	bio->bi_status = err;
	bio_endio(bio);
	return DM_MAPIO_SUBMITTED;
}

/*
 * 쓰기 하나가 끝났을 때의 뒷정리 (DM bio 의 .end_io 또는 FUA 복제본의 완료).
 *
 * 쓰기가 거절되면 그 zone 의 하위 wp 는 우리 할당 위치와 어긋난다. 그대로
 * 두면 그 zone 의 이후 쓰기가 전부 거절되므로 활성 zone 이면 바로 마감한다.
 * 마감을 inflight 감소보다 먼저 해야, 그 사이에 zone 이 비워지고 다시 열린
 * 뒤 엉뚱한 zone 을 마감하는 일이 없다. 레코드의 VOID 표시도 epoch 감소보다
 * 먼저다 — 감소 뒤에는 커밋이 이 레코드를 읽을 수 있다.
 *
 * 데이터 쓰기가 실패하면 메타 기록을 멈춘다(fail-stop). 실패한 쓰기의
 * 메모리 매핑은 쓰이지 않은 위치를 가리키고 옛 위치의 valid 는 이미 줄었다.
 * 그대로 커밋·체크포인트·GC reset 을 이어 가면 장치의 매핑이 쓰이지 않은
 * 위치나 reset 된 zone 을 가리키게 된다. 멈추면 장치의 매핑은 마지막 커밋에
 * 머물러 일관적이고, 이후 쓰기와 flush 는 EIO 다. md_err 도 epoch 감소보다
 * 먼저 세운다 — 진행 중인 커밋이 대기에서 깨어나 이것을 본다.
 */
static void zns_write_done(struct zns_base_c *c, struct zns_pio *pio,
			   blk_status_t error)
{
	struct zns_zone *z = &c->zones[pio->zidx];
	unsigned long flags;

	if (error) {
		spin_lock_irqsave(&c->map_lock, flags);
		c->stat_write_err++;
		c->ring[pio->rec_seq & (c->ring_cap - 1)].flags |= ZNS_REC_VOID;
		if (c->active_user_idx == pio->zidx) {
			z->state = ZNS_ZONE_FULL;
			c->active_user_idx = -1;
		}
		WRITE_ONCE(c->md_err, true);
		spin_unlock_irqrestore(&c->map_lock, flags);
		wake_up_all(&c->ring_wq);
		DMERR_LIMIT("write error %d in zone %d: metadata frozen at the last commit, writes and flushes will fail",
			    blk_status_to_errno(error), pio->zidx);
	}
	atomic64_sub(pio->bytes, &c->queued_bytes);
	wake_up(&c->admit_wq);
	if (atomic_dec_and_test(&z->wr_inflight))
		wake_up_all(&c->idle_wq);
	if (atomic_dec_and_test(&c->ep_inflight[pio->epoch]))
		wake_up_all(&c->ep_wq);
	pio->zidx = -1;
}

/* FUA 쓰기의 데이터가 끝났다. 레코드가 커밋되면 커밋 워커가 DM bio 를 완료한다. */
static void zns_fua_child_endio(struct bio *child)
{
	struct bio *bio = child->bi_private;
	struct zns_pio *pio = zns_pio(bio);
	struct zns_base_c *c = pio->c;
	blk_status_t error = child->bi_status;
	unsigned long flags;

	zns_write_done(c, pio, error);
	bio_put(child);

	if (error) {
		bio->bi_status = error;
		bio_endio(bio);
		return;
	}
	spin_lock_irqsave(&c->map_lock, flags);
	bio_list_add(&c->fua_list, bio);
	spin_unlock_irqrestore(&c->map_lock, flags);
	queue_work(c->log_wq, &c->log_work);
}

/*
 * 모든 bio 의 완료가 여기를 지난다. .map 이 zone 에 걸어 둔 것만 되돌린다.
 * (FUA 쓰기는 복제본의 완료에서 이미 되돌려 zidx 가 -1 이다.)
 */
static int zns_base_end_io(struct dm_target *ti, struct bio *bio,
			   blk_status_t *error)
{
	struct zns_base_c *c = ti->private;
	struct zns_pio *pio  = zns_pio(bio);
	struct zns_zone *z;

	if (pio->zidx < 0)
		return DM_ENDIO_DONE;
	z = &c->zones[pio->zidx];

	if (pio->write) {
		zns_write_done(c, pio, *error);
		return DM_ENDIO_DONE;
	} else {
		if (atomic_dec_and_test(&z->rd_inflight))
			wake_up_all(&c->idle_wq);
	}
	pio->zidx = -1;
	return DM_ENDIO_DONE;
}

/* ==================================================================
 * Read I/O
 * ================================================================== */

static int zns_handle_read(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c  = ti->private;
	sector_t lba          = dm_target_offset(ti, bio->bi_iter.bi_sector);
	sector_t block_idx    = lba >> BLOCK_SHIFT;
	sector_t off_in_block = lba & (SECTORS_PER_BLOCK - 1);
	sector_t want         = bio_sectors(bio);
	sector_t run, pba;
	sector_t i = 1;
	unsigned long flags;

	if (want == 0) {
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	spin_lock_irqsave(&c->map_lock, flags);

	pba = (block_idx < c->total_blocks)
		? map_lookup(c, block_idx)
		: PBA_UNMAPPED;

	run = SECTORS_PER_BLOCK - off_in_block;
	while (run < want && i < READ_SCAN_MAX_BLOCKS &&
	       block_idx + i < c->total_blocks) {
		sector_t next = map_lookup(c, block_idx + i);

		if (pba == PBA_UNMAPPED) {
			if (next != PBA_UNMAPPED)
				break;
		} else if (next != pba + (i << BLOCK_SHIFT) ||
			   zidx_of(c, next) != zidx_of(c, pba)) {
			/* 한 bio 는 zone 하나에만 걸리게 한다 (rd_inflight 가 zone 별) */
			break;
		}

		run += SECTORS_PER_BLOCK;
		i++;
	}

	/* GC 가 이 zone 을 reset 하기 전에 이 읽기가 끝나기를 기다릴 수 있게 */
	if (pba != PBA_UNMAPPED) {
		struct zns_pio *pio = zns_pio(bio);

		pio->zidx  = (int)zidx_of(c, pba);
		pio->write = false;
		atomic_inc(&c->zones[pio->zidx].rd_inflight);
	}

	spin_unlock_irqrestore(&c->map_lock, flags);

	if (run < want)
		dm_accept_partial_bio(bio, (unsigned int)run);

	if (pba != PBA_UNMAPPED) {
		bio_set_dev(bio, c->dev->bdev);
		bio->bi_iter.bi_sector = pba + off_in_block;
		return DM_MAPIO_REMAPPED;
	}

	zero_fill_bio(bio);
	bio->bi_status = BLK_STS_OK;
	bio_endio(bio);
	return DM_MAPIO_SUBMITTED;
}

/* ==================================================================
 * .map — 메인 진입점
 * ================================================================== */

static int zns_base_map(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c = ti->private;
	unsigned long flags;

	zns_pio(bio)->zidx = -1;	/* .end_io 가 되돌릴 것이 없음 */

	/* crash 흉내 뒤에는 전원이 나간 장치처럼 아무것도 받지 않는다 */
	if (unlikely(READ_ONCE(c->crashed)))
		return DM_MAPIO_KILL;

	if (bio->bi_opf & REQ_PREFLUSH) {
		/* DM 이 데이터와 떼어 빈 flush 로 보낸다. 커밋 뒤에 완료한다. */
		WARN_ON_ONCE(bio_sectors(bio));
		spin_lock_irqsave(&c->map_lock, flags);
		bio_list_add(&c->flush_list, bio);
		spin_unlock_irqrestore(&c->map_lock, flags);
		queue_work(c->log_wq, &c->log_work);
		return DM_MAPIO_SUBMITTED;
	}

	switch (bio_op(bio)) {
	case REQ_OP_WRITE:
		return zns_handle_write(ti, bio);
	case REQ_OP_READ:
		return zns_handle_read(ti, bio);
	default:
		/* DISCARD / WRITE_ZEROES 등 미지원 — LBA 를 PBA 로 착각해
		 * 하위의 남의 데이터를 지우는 경로가 되므로 받지 않는다. */
		DMERR_LIMIT("unsupported bio op %u", bio_op(bio));
		return DM_MAPIO_KILL;
	}
}

/* ==================================================================
 * io_hints — 위쪽을 conventional 블록 디바이스로 광고
 * ================================================================== */

static void zns_base_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct zns_base_c *c = ti->private;

	zns_limits_clear_zoned(limits);
	limits->chunk_sectors = c->zone_sectors;

	/* 매핑 단위가 4 KiB 블록이므로 그보다 작은 I/O 는 받지 않는다 */
	limits->logical_block_size  = SECTORS_PER_BLOCK << SECTOR_SHIFT;
	limits->physical_block_size = SECTORS_PER_BLOCK << SECTOR_SHIFT;
	limits->io_min              = SECTORS_PER_BLOCK << SECTOR_SHIFT;
}

/*
 * 하위 장치의 한도를 위로 쌓게 한다. 이게 없으면 DM 코어가 io_hints 를
 * 부르지 않는 커널이 있다(5.15). 위쪽의 zoned 광고는 io_hints 가 지운다.
 * 보고하는 범위는 실제로 쓰는 zone 정수배 구간이다 — DM 은 zoned 장치의
 * 범위가 zone 경계에 맞지 않으면 테이블을 거부한다.
 */
static int zns_base_iterate_devices(struct dm_target *ti,
				    iterate_devices_callout_fn fn, void *data)
{
	struct zns_base_c *c = ti->private;

	return fn(ti, c->dev, 0, c->total_sectors, data);
}

/* ==================================================================
 * status / message
 *
 *   $ dmsetup status X
 *   0 <len> zns-base <used> <total> <free_zones> <data_zones> <valid_blk>
 *                    <gc_runs> <migrated> <mem_n> <nr_runs> <flushes>
 *                    <compactions> <dropped>
 *                    <write_err> <order_viol> <queued_kib>
 *                    <md_gen> <md_zone> <log_blocks> <commits> <ckpts>
 *                    <pending_recs> <index_kib> <ring_kib> <recover_ms>
 *                    <state>
 *
 *   used/total: 데이터 zone 의 점유 / 전체 섹터 (FULL 은 zone 통째, ACTIVE
 *         는 wp 까지). GC 가 zone 을 회수하면 used 가 줄어든다.
 *   write_err: 하위가 거절한 사용자 쓰기.  order_viol: 0 이어야 한다.
 *   queued_kib: 할당됐지만 아직 완료되지 않은 사용자 쓰기.
 *   log_blocks: 마지막 체크포인트 뒤 로그 블록.  pending_recs: 커밋 전 레코드.
 *   index_kib: 매핑 인덱스 메모리 (MemTable 풀 + run 배열).
 *   recover_ms: 생성 때 복구에 걸린 시간 (format 이면 0).
 *   state: 0 정상, 1 crash 흉내 뒤, 2 fail-stop (쓰기 또는 메타 기록 실패).
 *
 *   $ dmsetup message X 0 gc <zone|auto>   — GC 강제 실행 (테스트용)
 *   $ dmsetup message X 0 zones            — zone 별 state:valid 덤프
 *   $ dmsetup message X 0 commit           — 커밋
 *   $ dmsetup message X 0 checkpoint       — 체크포인트 강제
 *   $ dmsetup message X 0 crash            — 전원 차단 흉내: 이후 I/O 와
 *                                            커밋을 모두 버린다
 * ================================================================== */

static void zns_base_status(struct dm_target *ti, status_type_t type,
			    unsigned int status_flags, char *result,
			    unsigned int maxlen)
{
	struct zns_base_c *c = ti->private;
	unsigned int sz = 0;
	unsigned long flags;
	sector_t used = 0;
	u64 valid = 0, index_bytes, pending;
	unsigned int i, mem_n, nr_runs, free_zones;
	u64 fl, cp, dr, gc, mig, werr, oviol;
	struct zns_run *r;

	switch (type) {
	case STATUSTYPE_INFO:
		spin_lock_irqsave(&c->map_lock, flags);
		for (i = 0; i < c->data_zones; i++) {
			struct zns_zone *z = &c->zones[i];

			if (z->state == ZNS_ZONE_FULL)
				used += c->zone_sectors;
			else if (z->state == ZNS_ZONE_ACTIVE)
				used += z->wp - z->start;
			valid += z->valid;
		}
		index_bytes = (u64)NODE_POOL_MAX * sizeof(struct zns_mnode);
		if (c->spare)
			index_bytes += (u64)NODE_POOL_MAX * sizeof(struct zns_ent);
		list_for_each_entry(r, &c->runs, list)
			index_bytes += (u64)r->cap * sizeof(struct zns_ent);
		pending = zns_ring_used(c);
		free_zones = c->free_zones;
		mem_n = c->mem_n;
		nr_runs = c->nr_runs;
		fl = c->stat_flushes;
		cp = c->stat_compactions;
		dr = c->stat_dropped;
		gc = c->stat_gc_runs;
		mig = c->stat_migrated;
		werr = c->stat_write_err;
		oviol = READ_ONCE(c->stat_order_viol);
		spin_unlock_irqrestore(&c->map_lock, flags);

		DMEMIT("%llu %llu %u %u %llu %llu %llu %u %u %llu %llu %llu %llu %llu %llu",
		       (unsigned long long)used,
		       (unsigned long long)c->data_zones * c->zone_sectors,
		       free_zones, c->data_zones,
		       (unsigned long long)valid,
		       (unsigned long long)gc,
		       (unsigned long long)mig,
		       mem_n, nr_runs,
		       (unsigned long long)fl,
		       (unsigned long long)cp,
		       (unsigned long long)dr,
		       (unsigned long long)werr,
		       (unsigned long long)oviol,
		       (unsigned long long)(atomic64_read(&c->queued_bytes) >> 10));
		/* md_* 는 log_lock 소관이지만 통계라 느슨하게 읽는다 */
		DMEMIT(" %llu %u %u %llu %llu %llu %llu %llu %llu %d",
		       (unsigned long long)READ_ONCE(c->md_gen),
		       zns_md_zone(c, READ_ONCE(c->md_cur)),
		       READ_ONCE(c->md_log_blocks),
		       (unsigned long long)READ_ONCE(c->stat_commits),
		       (unsigned long long)READ_ONCE(c->stat_ckpts),
		       (unsigned long long)pending,
		       (unsigned long long)(index_bytes >> 10),
		       (unsigned long long)((c->ring_cap * sizeof(struct zns_rec)) >> 10),
		       (unsigned long long)c->recover_ms,
		       READ_ONCE(c->crashed) ? 1 : READ_ONCE(c->md_err) ? 2 : 0);
		break;
	case STATUSTYPE_TABLE:
		DMEMIT("%s", c->dev->name);
		break;
	default:
		break;
	}
}

static int zns_base_message(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	struct zns_base_c *c = ti->private;
	unsigned int sz = 0;
	unsigned long flags;
	unsigned int i;
	int ret;

	if (argc == 2 && !strcmp(argv[0], "gc")) {
		int vidx = -1;

		if (strcmp(argv[1], "auto")) {
			if (kstrtoint(argv[1], 10, &vidx))
				return -EINVAL;
		}
		mutex_lock(&c->gc_lock);
		ret = zns_gc_once(c, vidx);
		mutex_unlock(&c->gc_lock);
		DMEMIT("gc %s -> %d", argv[1], ret);
		return 1;	/* result 를 채웠음 */
	}

	if (argc == 1 && !strcmp(argv[0], "zones")) {
		/* Free/Active/fUll/Meta */
		static const char st[4] = { 'F', 'A', 'U', 'M' };

		spin_lock_irqsave(&c->map_lock, flags);
		for (i = 0; i < c->nr_zones && sz + 16 < maxlen; i++)
			DMEMIT("%u:%c:%u ", i, st[c->zones[i].state],
			       c->zones[i].valid);
		spin_unlock_irqrestore(&c->map_lock, flags);
		return 1;
	}

	if (argc == 1 && (!strcmp(argv[0], "commit") ||
			  !strcmp(argv[0], "checkpoint"))) {
		ret = zns_log_commit(c, argv[0][1] == 'h');
		DMEMIT("%s -> %d", argv[0], ret);
		return 1;
	}

	if (argc == 1 && !strcmp(argv[0], "crash")) {
		/*
		 * 전원 차단 흉내. 커밋되지 않은 매핑 변경은 사라지고, 이미 보낸
		 * 쓰기는 장치에 닿을 수도 있다 — 실제 전원 차단과 같다. 기다리던
		 * flush/FUA 는 실패로 끝낸다.
		 */
		WRITE_ONCE(c->crashed, true);
		wake_up_all(&c->ring_wq);
		queue_work(c->log_wq, &c->log_work);
		DMWARN("crash: metadata commits stopped, all I/O will fail");
		DMEMIT("crash -> 0");
		return 1;
	}

	return -EINVAL;
}

/* ==================================================================
 * target_type
 * ================================================================== */

static struct target_type zns_base_target = {
	.name            = "zns-base",
	.version         = {0, 6, 0},
	/*
	 * zoned 하위 장치를 받되 위쪽은 zoned 가 아니다(io_hints 가 지운다).
	 * 이 플래그가 없으면 DM 이 zoned 모델 불일치로 테이블을 거부한다.
	 */
	.features        = DM_TARGET_MIXED_ZONED_MODEL,
	.module          = THIS_MODULE,
	.ctr             = zns_base_ctr,
	.dtr             = zns_base_dtr,
	.map             = zns_base_map,
	.end_io          = zns_base_end_io,
	.io_hints        = zns_base_io_hints,
	.iterate_devices = zns_base_iterate_devices,
	.status          = zns_base_status,
	.message         = zns_base_message,
};

static int __init zns_base_init(void)
{
	int ret = dm_register_target(&zns_base_target);

	if (ret < 0)
		DMERR("target registration failed: %d", ret);
	else
		DMINFO("target registered (LSM index + GC + metadata log)");
	return ret;
}

static void __exit zns_base_exit(void)
{
	dm_unregister_target(&zns_base_target);
	DMINFO("target unregistered");
}

module_init(zns_base_init);
module_exit(zns_base_exit);

MODULE_DESCRIPTION("ZNS target: random-to-sequential translation, LSM mapping, GC, crash recovery");
MODULE_AUTHOR("SPLAB");
MODULE_LICENSE("GPL");
