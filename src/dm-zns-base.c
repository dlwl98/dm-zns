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
 *
 * ── LSM-Tree 인덱스 ─────────────────────────────────────────────────
 *
 *   조회:   MemTable(rbtree) → run[0](최신) → run[1] → ... → 없으면 unmapped
 *   flush:  MemTable 이 차면 중위 순회로 "정렬된 불변 run" 하나를 만든다
 *   컴팩션: run 이 쌓이면 하나로 병합. 같은 LBA 는 최신 것만 남는다.
 *
 * ── zone 관리 ───────────────────────────────────────────────────────
 *
 *   zone 상태: FREE → ACTIVE(할당 중) → FULL → (GC) → FREE
 *
 *   활성 zone 은 사용자용/GC용 두 개를 분리한다. 하위 ZNS 는 zone 별
 *   wp 위치의 쓰기만 받는데, 할당 순서와 제출 순서는 발행자가 둘이면
 *   달라질 수 있다 — 사용자 쓰기는 .map 에서 할당 후 bio_list 를 거쳐
 *   지연 제출되고, GC 워커는 즉시 제출하므로, 같은 zone 을 공유하면
 *   나중에 할당받은 GC 쓰기가 먼저 도착해 SEQ 위반(EIO)이 난다.
 *   zone 을 발행자별로 나누면 zone 내 순서는 각 발행자가 스스로 지킨다.
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
 *   - 매핑 영속화 / crash recovery 없음.
 *   - GC 이주는 4 KiB 동기 I/O 페어(submit_bio_wait) — 정확성 우선,
 *     배치·비동기화는 성능 단계에서.
 *   - 사용자 쓰기 발행자가 여럿이면(다중 스레드, ext4 의 writeback 과
 *     jbd2 등) 사용자 zone 안에서도 같은 순서 역전이 가능하다. 해법은
 *     할당 순서대로 제출하는 단일 제출 경로, 또는 REQ_OP_ZONE_APPEND
 *     (디바이스가 위치를 정하므로 순서 무관).
 *   - in-flight read 와 zone reset 의 경합: read 가 옛 PBA 로 remap 되어
 *     하위로 내려가는 사이 그 zone 이 이주 완료 → reset 되면 낡은 데이터를
 *     읽을 수 있다. 창이 극히 좁고(제출~완료 사이) MVP 범위 밖 — dm-zoned
 *     는 bio 추적으로 푼다.
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
};

enum zns_zone_state {
	ZNS_ZONE_FREE = 0,
	ZNS_ZONE_ACTIVE,
	ZNS_ZONE_FULL,
};

struct zns_zone {
	sector_t	start;	/* zone 시작 (절대 섹터) */
	sector_t	wp;	/* 다음 쓰기 위치 (절대 섹터) — 하위 wp 와 일치 */
	u32		valid;	/* 이 zone 을 가리키는 살아있는 매핑 수 (블록) */
	u8		state;
};

/* 쓰기 완료 콜백 context */
struct zns_write_io {
	struct bio *orig;
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

	spinlock_t	 map_lock;	/* 인덱스 + zone 배열 보호 */
	sector_t	 total_sectors;	/* nr_zones * zone_sectors */
	sector_t	 total_blocks;
	sector_t	 zone_sectors;
	unsigned int	 nr_zones;

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

	/* --- GC --- */
	struct mutex	 gc_lock;	/* GC 는 한 번에 하나 */
	struct workqueue_struct *gc_wq;	/* GC 전용 — .map 에서 직접 못 도는 이유는
					 * 파일 머리 주석 참고 */
	struct work_struct	 gc_work;

	/* --- 통계 (dmsetup status) --- */
	u64	stat_flushes;
	u64	stat_compactions;
	u64	stat_dropped;	/* 인덱스에서 제거된 중복 엔트리 수 */
	u64	stat_gc_runs;	/* 회수 완료한 zone 수 */
	u64	stat_migrated;	/* GC 가 이주시킨 블록 수 */
};

static inline struct zns_zone *zone_of(struct zns_base_c *c, sector_t pba)
{
	return &c->zones[pba / c->zone_sectors];
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
 * 컴팩션 — run 여러 개를 하나로 병합
 * ================================================================== */

static struct zns_ent *zns_merge_runs(struct zns_run **snap, unsigned int k,
				      unsigned int *out_n, u64 *dropped)
{
	struct zns_ent *out;
	unsigned int *pos;
	unsigned int total = 0, n = 0, i;
	u64 drop = 0;

	for (i = 0; i < k; i++)
		total += snap[i]->n;

	if (!total) {
		*out_n = 0;
		*dropped = 0;
		return NULL;
	}

	out = zns_kvmalloc_array(total, sizeof(*out), GFP_NOIO);
	if (!out)
		return NULL;

	pos = kcalloc(k, sizeof(*pos), GFP_NOIO);
	if (!pos) {
		kvfree(out);
		return NULL;
	}

	for (;;) {
		sector_t best_lba = 0;
		int best = -1;

		/* 남은 것들 중 가장 작은 lba_blk. 동률이면 더 최신 run 이 이긴다. */
		for (i = 0; i < k; i++) {
			if (pos[i] >= snap[i]->n)
				continue;
			if (best < 0 || snap[i]->ents[pos[i]].lba_blk < best_lba) {
				best = i;
				best_lba = snap[i]->ents[pos[i]].lba_blk;
			}
		}
		if (best < 0)
			break;

		out[n++] = snap[best]->ents[pos[best]];

		for (i = 0; i < k; i++) {
			if (pos[i] < snap[i]->n &&
			    snap[i]->ents[pos[i]].lba_blk == best_lba) {
				if (i != (unsigned int)best)
					drop++;
				pos[i]++;
			}
		}
	}

	kfree(pos);
	*out_n = n;
	*dropped = drop;
	return out;
}

/* 락 밖(sleepable)에서 호출한다. */
static void zns_maybe_compact(struct zns_base_c *c)
{
	struct zns_run **snap = NULL;
	struct zns_run *merged = NULL;
	struct zns_run *r;
	struct zns_ent *ents;
	struct list_head *anchor;
	unsigned int k = 0, i, n = 0;
	unsigned long flags;
	u64 dropped = 0;

	spin_lock_irqsave(&c->map_lock, flags);
	if (c->compacting || c->nr_runs < RUNS_COMPACT_AT) {
		spin_unlock_irqrestore(&c->map_lock, flags);
		return;
	}
	c->compacting = true;
	k = c->nr_runs;
	spin_unlock_irqrestore(&c->map_lock, flags);

	snap   = kcalloc(k, sizeof(*snap), GFP_NOIO);
	merged = kzalloc(sizeof(*merged), GFP_NOIO);
	if (!snap || !merged)
		goto out_abort;

	/*
	 * run 은 불변이고 리스트 앞쪽에만 추가된다. 여기서 앞에서부터 k 개를
	 * 적어두면 병합하는 동안 새 run 이 앞에 붙어도 적어둔 것은 그대로다.
	 * compacting 플래그가 GC 스캔과의 상호 배제도 겸한다 — GC 도 run
	 * 포인터를 락 밖에서 참조하므로, 컴팩션의 kvfree 와 겹치면 안 된다.
	 */
	spin_lock_irqsave(&c->map_lock, flags);
	i = 0;
	list_for_each_entry(r, &c->runs, list) {
		if (i >= k)
			break;
		snap[i++] = r;
	}
	k = i;
	spin_unlock_irqrestore(&c->map_lock, flags);

	if (k < 2)
		goto out_abort;

	ents = zns_merge_runs(snap, k, &n, &dropped);
	if (!ents)
		goto out_abort;

	merged->ents = ents;
	merged->n    = n;

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

	DMDEBUG("compact: %u runs -> %u entries, %llu dropped",
		k, n, (unsigned long long)dropped);

	for (i = 0; i < k; i++) {
		kvfree(snap[i]->ents);
		kfree(snap[i]);
	}
	kfree(snap);
	return;

out_abort:
	spin_lock_irqsave(&c->map_lock, flags);
	c->compacting = false;
	spin_unlock_irqrestore(&c->map_lock, flags);
	kfree(snap);
	kfree(merged);
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
				z->state = ZNS_ZONE_ACTIVE;
				z->wp = z->start;
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
 * GC — victim 의 살아있는 블록을 이주시키고 zone 을 reset 한다
 * ================================================================== */

/* 4 KiB 동기 I/O 한 건. sleepable. */
static int zns_rw_block(struct zns_base_c *c, sector_t sect, struct page *pg,
			zns_req_op_t op)
{
	struct bio *b;
	int ret;

	b = zns_bio_alloc(c->dev->bdev, 1, op | REQ_SYNC, GFP_NOIO);
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
 */
static int zns_pick_victim_locked(struct zns_base_c *c)
{
	int best = -1;
	u32 best_valid = U32_MAX;
	unsigned int i;

	for (i = 0; i < c->nr_zones; i++) {
		struct zns_zone *z = &c->zones[i];

		if (z->state != ZNS_ZONE_FULL)
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
	int vidx, ret = 0, tries;

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
	 * 스캔하면 use-after-free 다.
	 */
	for (tries = 0; ; tries++) {
		spin_lock_irqsave(&c->map_lock, flags);
		if (!c->compacting)
			break;
		spin_unlock_irqrestore(&c->map_lock, flags);
		if (tries > 1000) {
			ret = -EBUSY;
			goto out;
		}
		usleep_range(100, 200);
	}
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
		if (ret)
			goto out;

		/*
		 * CAS: 복사하는 동안 사용자가 같은 LBA 를 덮어썼다면 매핑이
		 * 이미 다른 곳을 가리킨다 — 그 경우 복사본은 그냥 버린다
		 * (new_pba 는 valid 에 안 잡힌 죽은 블록으로 남고, 그 zone 이
		 * 나중에 GC 될 때 자연히 회수된다).
		 */
		spin_lock_irqsave(&c->map_lock, flags);
		if (map_lookup(c, lba_blk) == pba) {
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
		}
		spin_unlock_irqrestore(&c->map_lock, flags);
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
	return ret;
}

static void zns_gc_workfn(struct work_struct *w)
{
	struct zns_base_c *c = container_of(w, struct zns_base_c, gc_work);

	mutex_lock(&c->gc_lock);
	zns_gc_once(c, -1);
	mutex_unlock(&c->gc_lock);
}

/* ==================================================================
 * Constructor / Destructor
 * ================================================================== */

static int zns_base_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct zns_base_c *c;
	sector_t max_len;
	unsigned int i;
	int ret;

	if (argc != 1) {
		ti->error = "expected one argument: underlying device";
		return -EINVAL;
	}

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		ti->error = "out of memory";
		return -ENOMEM;
	}

	spin_lock_init(&c->map_lock);
	mutex_init(&c->gc_lock);
	INIT_WORK(&c->gc_work, zns_gc_workfn);
	c->memtable = RB_ROOT;
	INIT_LIST_HEAD(&c->runs);
	c->active_user_idx = -1;
	c->active_gc_idx   = -1;

	/* 쓰기 진행이 GC 에 달려 있으므로 메모리 압박에서도 굴러야 한다 */
	c->gc_wq = alloc_workqueue("zns-gc", WQ_MEM_RECLAIM, 1);
	if (!c->gc_wq) {
		ti->error = "cannot create gc workqueue";
		kfree(c);
		return -ENOMEM;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			    &c->dev);
	if (ret) {
		ti->error = "failed to open underlying device";
		kfree(c);
		return ret;
	}

	c->zone_sectors = bdev_zone_sectors(c->dev->bdev);
	if (!c->zone_sectors) {
		ti->error = "underlying device is not zoned";
		ret = -EINVAL;
		goto err_dev;
	}

	/* zone 정수배만 쓴다. ti->len 의 자투리는 무시. */
	c->nr_zones      = (unsigned int)(ti->len / c->zone_sectors);
	c->total_sectors = (sector_t)c->nr_zones * c->zone_sectors;
	c->total_blocks  = c->total_sectors >> BLOCK_SHIFT;
	if (!c->nr_zones) {
		ti->error = "device smaller than one zone";
		ret = -EINVAL;
		goto err_dev;
	}

	c->zones = kvcalloc(c->nr_zones, sizeof(*c->zones), GFP_KERNEL);
	c->node_pool = kvmalloc_array(NODE_POOL_MAX, sizeof(*c->node_pool),
				      GFP_KERNEL);
	c->spare     = kvmalloc_array(NODE_POOL_MAX, sizeof(*c->spare),
				      GFP_KERNEL);
	c->spare_run = kzalloc(sizeof(*c->spare_run), GFP_KERNEL);
	if (!c->zones || !c->node_pool || !c->spare || !c->spare_run) {
		ti->error = "cannot allocate index/zone state";
		ret = -ENOMEM;
		goto err_free;
	}

	for (i = 0; i < c->nr_zones; i++) {
		c->zones[i].start = (sector_t)i * c->zone_sectors;
		c->zones[i].wp    = c->zones[i].start;
		c->zones[i].state = ZNS_ZONE_FREE;
	}
	c->free_zones = c->nr_zones;

	/*
	 * 하위 zone 을 전부 reset 해 wp 를 0 으로 맞춘다. 인덱스가 in-memory
	 * 라 dtr 에서 사라지므로, 하위에 남아 있던 데이터는 어차피 도달
	 * 불가 — 지워도 잃을 것이 없다. (영속화 도입 시 되돌릴 결정.)
	 */
	ret = zns_zone_mgmt(c->dev->bdev, REQ_OP_ZONE_RESET, 0,
			    bdev_nr_sectors(c->dev->bdev));
	if (ret) {
		ti->error = "failed to reset zones on underlying device";
		goto err_free;
	}

	ti->private        = c;
	ti->num_flush_bios = 1;

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
		goto err_free;
	}

	DMINFO("ctr: '%s', %u zones x %llu sectors, LSM index, GC reserve %u",
	       argv[0], c->nr_zones,
	       (unsigned long long)c->zone_sectors, GC_RESERVED_ZONES);
	return 0;

err_free:
	kvfree(c->zones);
	kvfree(c->node_pool);
	kvfree(c->spare);
	kfree(c->spare_run);
err_dev:
	dm_put_device(ti, c->dev);
	destroy_workqueue(c->gc_wq);
	kfree(c);
	return ret;
}

static void zns_base_dtr(struct dm_target *ti)
{
	struct zns_base_c *c = ti->private;
	struct zns_run *r, *tmp;

	destroy_workqueue(c->gc_wq);	/* pending GC 를 끝내고 내린다 */

	list_for_each_entry_safe(r, tmp, &c->runs, list) {
		list_del(&r->list);
		kvfree(r->ents);
		kfree(r);
	}

	kvfree(c->zones);
	kvfree(c->node_pool);
	kvfree(c->spare);
	kfree(c->spare_run);
	dm_put_device(ti, c->dev);
	kfree(c);
	DMINFO("dtr: target detached");
}

/* ==================================================================
 * Write I/O
 * ================================================================== */

static void zns_write_end_io(struct bio *clone)
{
	struct zns_write_io *io = clone->bi_private;
	struct bio *orig        = io->orig;

	orig->bi_status = clone->bi_status;
	bio_endio(orig);

	kfree(io);
	bio_put(clone);
}

static int zns_handle_write(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c = ti->private;
	sector_t lba         = bio->bi_iter.bi_sector;
	sector_t len_sectors = bio_sectors(bio);
	sector_t block_idx   = lba >> BLOCK_SHIFT;
	sector_t nr_blocks;
	sector_t pba;
	sector_t i;
	struct zns_write_io *io;
	struct bio *clone;
	unsigned long flags;
	bool need_refill, low_free;
	unsigned int gc_tries = 0, mem_tries = 0;
	blk_status_t err;

	if (len_sectors == 0) {
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	nr_blocks = (len_sectors + SECTORS_PER_BLOCK - 1) >> BLOCK_SHIFT;

	/*
	 * 실패할 수 있는 일(할당)은 전부 잠금 구간 앞에. 인덱스를 갱신한
	 * 뒤에 실패하면 쓰이지 않은 PBA 를 가리키는 매핑이 남는다.
	 */
	io = kmalloc(sizeof(*io), GFP_NOIO);
	if (!io) {
		err = BLK_STS_RESOURCE;
		goto err_out;
	}

	clone = zns_bio_clone(c->dev->bdev, bio, GFP_NOIO, &fs_bio_set);
	if (!clone) {
		kfree(io);
		err = BLK_STS_RESOURCE;
		goto err_out;
	}

again:
	spin_lock_irqsave(&c->map_lock, flags);

	/* 인덱스 자리 확인 — 부분 갱신 방지를 위해 통째로 미리 */
	if (c->node_used + nr_blocks > NODE_POOL_MAX &&
	    !mem_flush_locked(c)) {
		spin_unlock_irqrestore(&c->map_lock, flags);
		if (++mem_tries > 8) {
			DMERR_LIMIT("memtable full and cannot flush");
			err = BLK_STS_RESOURCE;
			goto err_free;
		}
		zns_refill_spares(c);
		goto again;
	}
	if (c->mem_n + nr_blocks > MEMTABLE_MAX)
		mem_flush_locked(c);

	pba = zone_alloc_locked(c, len_sectors, false);
	if (pba == PBA_UNMAPPED) {
		spin_unlock_irqrestore(&c->map_lock, flags);

		/* free zone 이 없다 — 워커에 GC 를 맡기고 완료를 기다린 뒤
		 * 재시도. 여기서 zns_gc_once() 를 직접 부르면 안 되는 이유는
		 * 파일 머리 주석(current->bio_list 자기 교착) 참고. */
		if (++gc_tries > WRITE_GC_RETRIES) {
			DMERR_LIMIT("out of space: no reclaimable zone (valid data ~%u zones)",
				    c->nr_zones - c->free_zones);
			err = BLK_STS_NOSPC;
			goto err_free;
		}
		queue_work(c->gc_wq, &c->gc_work);
		flush_work(&c->gc_work);
		goto again;
	}

	for (i = 0; i < nr_blocks; i++) {
		sector_t idx = block_idx + i;
		sector_t new_blk_pba = pba + (i << BLOCK_SHIFT);
		sector_t old = PBA_UNMAPPED;
		int up;

		if (idx >= c->total_blocks)
			break;

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

	spin_unlock_irqrestore(&c->map_lock, flags);

	io->orig = bio;
	clone->bi_iter.bi_sector = pba;
	clone->bi_end_io         = zns_write_end_io;
	clone->bi_private        = io;

	submit_bio_noacct(clone);

	/* sleepable 뒷정리 */
	if (need_refill)
		zns_refill_spares(c);
	if (low_free)
		queue_work(c->gc_wq, &c->gc_work);	/* 선제 GC — 비동기 */
	zns_maybe_compact(c);

	return DM_MAPIO_SUBMITTED;

err_free:
	bio_put(clone);
	kfree(io);
err_out:
	bio->bi_status = err;
	bio_endio(bio);
	return DM_MAPIO_SUBMITTED;
}

/* ==================================================================
 * Read I/O
 * ================================================================== */

static int zns_handle_read(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c  = ti->private;
	sector_t lba          = bio->bi_iter.bi_sector;
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
		} else if (next != pba + (i << BLOCK_SHIFT)) {
			break;
		}

		run += SECTORS_PER_BLOCK;
		i++;
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

	if (bio->bi_opf & REQ_PREFLUSH) {
		bio_set_dev(bio, c->dev->bdev);
		return DM_MAPIO_REMAPPED;
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
	limits->chunk_sectors = bdev_zone_sectors(c->dev->bdev);
}

/* ==================================================================
 * status / message
 *
 *   $ dmsetup status X
 *   0 <len> zns-base <used> <total> <free_zones> <nr_zones> <valid_blk>
 *                    <gc_runs> <migrated> <mem_n> <nr_runs> <flushes>
 *                    <compactions> <dropped>
 *
 *   used: FREE 가 아닌 zone 의 점유 섹터 합 (FULL 은 zone 통째,
 *         ACTIVE 는 wp 까지). GC 가 zone 을 회수하면 줄어든다.
 *
 *   $ dmsetup message X 0 gc <zone|auto>   — GC 강제 실행 (테스트용)
 *   $ dmsetup message X 0 zones            — zone 별 state:valid 덤프
 * ================================================================== */

static void zns_base_status(struct dm_target *ti, status_type_t type,
			    unsigned int status_flags, char *result,
			    unsigned int maxlen)
{
	struct zns_base_c *c = ti->private;
	unsigned int sz = 0;
	unsigned long flags;
	sector_t used = 0;
	u64 valid = 0;
	unsigned int i, mem_n, nr_runs, free_zones;
	u64 fl, cp, dr, gc, mig;

	switch (type) {
	case STATUSTYPE_INFO:
		spin_lock_irqsave(&c->map_lock, flags);
		for (i = 0; i < c->nr_zones; i++) {
			struct zns_zone *z = &c->zones[i];

			if (z->state == ZNS_ZONE_FULL)
				used += c->zone_sectors;
			else if (z->state == ZNS_ZONE_ACTIVE)
				used += z->wp - z->start;
			valid += z->valid;
		}
		free_zones = c->free_zones;
		mem_n = c->mem_n;
		nr_runs = c->nr_runs;
		fl = c->stat_flushes;
		cp = c->stat_compactions;
		dr = c->stat_dropped;
		gc = c->stat_gc_runs;
		mig = c->stat_migrated;
		spin_unlock_irqrestore(&c->map_lock, flags);

		DMEMIT("%llu %llu %u %u %llu %llu %llu %u %u %llu %llu %llu",
		       (unsigned long long)used,
		       (unsigned long long)c->total_sectors,
		       free_zones, c->nr_zones,
		       (unsigned long long)valid,
		       (unsigned long long)gc,
		       (unsigned long long)mig,
		       mem_n, nr_runs,
		       (unsigned long long)fl,
		       (unsigned long long)cp,
		       (unsigned long long)dr);
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
		static const char st[3] = { 'F', 'A', 'U' }; /* Free/Active/fUll */

		spin_lock_irqsave(&c->map_lock, flags);
		for (i = 0; i < c->nr_zones && sz + 16 < maxlen; i++)
			DMEMIT("%u:%c:%u ", i, st[c->zones[i].state],
			       c->zones[i].valid);
		spin_unlock_irqrestore(&c->map_lock, flags);
		return 1;
	}

	return -EINVAL;
}

/* ==================================================================
 * target_type
 * ================================================================== */

static struct target_type zns_base_target = {
	.name            = "zns-base",
	.version         = {0, 4, 0},
	.features        = 0,
	.module          = THIS_MODULE,
	.ctr             = zns_base_ctr,
	.dtr             = zns_base_dtr,
	.map             = zns_base_map,
	.io_hints        = zns_base_io_hints,
	.status          = zns_base_status,
	.message         = zns_base_message,
};

static int __init zns_base_init(void)
{
	int ret = dm_register_target(&zns_base_target);

	if (ret < 0)
		DMERR("target registration failed: %d", ret);
	else
		DMINFO("target registered (LSM index + GC)");
	return ret;
}

static void __exit zns_base_exit(void)
{
	dm_unregister_target(&zns_base_target);
	DMINFO("target unregistered");
}

module_init(zns_base_init);
module_exit(zns_base_exit);

MODULE_DESCRIPTION("ZNS target: random-to-sequential translation, LSM mapping, GC");
MODULE_AUTHOR("SPLAB");
MODULE_LICENSE("GPL");
