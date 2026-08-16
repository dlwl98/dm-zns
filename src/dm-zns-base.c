// SPDX-License-Identifier: GPL-2.0
/*
 * dm-zns-base: M1 Random-to-Sequential Translation Target
 *
 * 위(upper)쪽: conventional 블록 디바이스로 광고 — ext4 등 임의 I/O 허용.
 * 아래(lower)쪽: host-managed ZNS 디바이스의 sequential-write 제약을 우리가 처리.
 *
 * 핵심 동작:
 *   WRITE  — 임의 LBA 로 들어온 쓰기를 wp 에 순차 append.
 *             LBA → PBA 매핑을 LSM-Tree 인덱스에 기록.
 *             bio 를 clone 해 실제 ZNS I/O 를 제출하고 원본은 완료 콜백에서 끝냄.
 *   READ   — 인덱스를 조회해 PBA 가 있으면 그 위치로 remapping.
 *             아직 쓰인 적 없는 LBA 는 zero-fill 후 완료.
 *   FLUSH  — 하위로 pass-through. 그 외 op 는 지원하지 않음.
 *
 * ── LSM-Tree 인덱스 ─────────────────────────────────────────────────
 *
 *   조회:   MemTable(rbtree) → run[0](최신) → run[1] → ... → 없으면 unmapped
 *   flush:  MemTable 이 차면 중위 순회로 "정렬된 불변 run" 하나를 만든다
 *   컴팩션: run 이 쌓이면 하나로 병합. 같은 LBA 는 최신 것만 남는다.
 *
 * 평면 배열 대신 이 구조를 쓰는 이유는 두 가지다.
 *
 *   1) 규모. 평면 배열은 디바이스 전체 블록 수만큼 자리를 미리 잡는다.
 *      2 GiB 면 4 MiB 로 되지만 32 GiB(FEMU) 면 64 MiB, 4 TB 실장비면
 *      8 GiB 다. 커널 메모리에 올릴 수 없다. LSM 은 실제로 쓰인 만큼만
 *      든다.
 *
 *   2) GC. 평면 배열에는 "이 PBA 는 이제 죽었다" 는 정보가 없다. 덮어쓰면
 *      옛 값이 그냥 사라진다. LSM 은 컴팩션에서 같은 LBA 의 옛 항목을
 *      밀어내는데, 그 밀려난 항목이 곧 회수 대상 물리 공간이다.
 *      M3 의 valid/invalid 판정이 컴팩션 부산물로 나온다.
 *
 * 잠금: 인덱스 변경·조회는 spinlock(map_lock) 으로 보호한다. 다만 컴팩션은
 *       수만 개 엔트리를 병합하므로 락 안에서 할 수 없다. run 이 불변이고
 *       리스트 앞쪽에만 추가된다는 성질을 이용해 스냅샷 → 락 밖 병합 →
 *       락 안 교체 순으로 처리한다.
 *
 * 한계(stretch 항목):
 *   - 매핑 영속화 / crash recovery 없음.
 *   - GC / zone reset 사이클 없음 (M3 예정). 용량을 넘기면 ENOSPC.
 *   - 단일 append log (활성 zone 하나).
 *   - 컴팩션 정책은 tiered 한 단계. 레벨 구조는 없음.
 *
 * See docs/07-milestones.md — M1, M2.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/rbtree.h>
#include <linux/list.h>

#define DM_MSG_PREFIX "zns-base"

/* 4 KiB = 8 섹터 = 1 블록 */
#define SECTORS_PER_BLOCK	8U
#define BLOCK_SHIFT		3U   /* log2(SECTORS_PER_BLOCK) */

/* 매핑되지 않은 블록의 sentinel 값 */
#define PBA_UNMAPPED		((sector_t)-1)

/* MemTable 이 이만큼 차면 run 으로 내린다 */
#define MEMTABLE_MAX		4096U
/* 노드 풀은 여유를 둔다. flush 용 버퍼를 다시 채우기 전에 동시 쓰기가
 * 임계를 넘어설 수 있기 때문. 이 여유마저 차면 BLK_STS_RESOURCE 로 되민다. */
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

/* ------------------------------------------------------------------ *
 * LSM 인덱스 자료구조                                                  *
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

/* ------------------------------------------------------------------ *
 * 쓰기 완료 콜백을 위한 context.                                       *
 * ------------------------------------------------------------------ */
struct zns_write_io {
	struct bio *orig;	/* 상위 레이어에서 내려온 원본 bio */
};

/* ------------------------------------------------------------------ *
 * Per-target 상태                                                      *
 * ------------------------------------------------------------------ */
struct zns_base_c {
	struct dm_dev	*dev;

	spinlock_t	 map_lock;	/* 인덱스 + wp_offset 보호 */
	sector_t	 wp_offset;	/* ZNS 장치 내 다음 쓰기 위치(섹터) */
	sector_t	 total_sectors;
	sector_t	 total_blocks;

	/* --- LSM 인덱스 --- */
	struct rb_root		 memtable;
	unsigned int		 mem_n;
	struct zns_mnode	*node_pool;	/* 미리 잡아둔 노드 배열 */
	unsigned int		 node_used;	/* bump 포인터 */
	struct zns_ent		*spare;		/* 다음 flush 가 쓸 버퍼 */
	struct zns_run		*spare_run;	/* 다음 flush 가 쓸 run 헤더 */
	struct list_head	 runs;		/* 최신이 head */
	unsigned int		 nr_runs;
	bool			 compacting;	/* 컴팩션은 한 번에 하나만 */

	/* --- 통계 (dmsetup status) --- */
	u64	stat_flushes;
	u64	stat_compactions;
	u64	stat_dropped;	/* 컴팩션에서 밀려난 = 죽은 매핑 = M3 회수 대상 */
};

/* ==================================================================
 * MemTable — rbtree
 * ================================================================== */

/* 호출자가 map_lock 을 잡고 있어야 한다. 자리가 없으면 false. */
static bool mem_upsert(struct zns_base_c *c, sector_t lba_blk, sector_t pba)
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
			/*
			 * 같은 블록을 이 세대 안에서 다시 썼다. 제자리 갱신하면
			 * 옛 PBA 는 run 에 닿지도 못하고 죽는다 — 그것도 회수 대상.
			 */
			n->ent.pba = pba;
			c->stat_dropped++;
			return true;
		}
	}

	if (c->node_used >= NODE_POOL_MAX)
		return false;

	n = &c->node_pool[c->node_used++];
	n->ent.lba_blk = lba_blk;
	n->ent.pba     = pba;

	rb_link_node(&n->rb, parent, p);
	rb_insert_color(&n->rb, &c->memtable);
	c->mem_n++;
	return true;
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

/* 전체 조회. map_lock 필요. 최신부터 보므로 처음 맞는 것이 정답이다. */
static sector_t map_lookup(struct zns_base_c *c, sector_t lba_blk)
{
	struct zns_run *r;
	sector_t pba;

	pba = mem_lookup(c, lba_blk);
	if (pba != PBA_UNMAPPED)
		return pba;

	list_for_each_entry(r, &c->runs, list) {
		pba = run_lookup(r, lba_blk);
		if (pba != PBA_UNMAPPED)
			return pba;
	}
	return PBA_UNMAPPED;
}

/* ==================================================================
 * flush — MemTable → 불변 run
 * ================================================================== */

/*
 * map_lock 필요. 미리 잡아둔 spare/spare_run 을 소비하므로 할당이 없다.
 * (스핀락 안에서 할당할 수 없기 때문에 이렇게 한다.)
 */
static bool mem_flush_locked(struct zns_base_c *c)
{
	struct zns_run *run = c->spare_run;
	struct rb_node *node;
	unsigned int i = 0;

	if (!run || !c->spare || c->mem_n == 0)
		return false;

	/* rbtree 중위 순회 = lba_blk 오름차순 */
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

	list_add(&run->list, &c->runs);	/* 최신이 head */
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
		ents = kvmalloc_array(NODE_POOL_MAX, sizeof(*ents), GFP_NOIO);
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

	/* 경쟁에서 졌으면 반납 */
	kvfree(ents);
	kfree(run);
}

/* ==================================================================
 * 컴팩션 — run 여러 개를 하나로 병합
 *
 * 같은 lba_blk 가 여러 run 에 있으면 가장 최신(snap 인덱스가 작은 쪽)만
 * 남기고 나머지를 버린다. 버려진 수가 stat_dropped 로 쌓이는데, 이것이
 * 곧 "이제 아무도 안 읽는 물리 공간" 의 양이다 — M3 의 GC 가 회수할 몫.
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

	out = kvmalloc_array(total, sizeof(*out), GFP_NOIO);
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

		/* 남은 것들 중 가장 작은 lba_blk. 동률이면 인덱스가 작은
		 * 쪽(= 더 최신 run)이 이긴다 — 비교가 strict less 이므로. */
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

		/* 같은 lba 를 가진 나머지는 밀려난다 = 죽은 매핑 */
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
	LIST_HEAD(dead);

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
	 * run 은 만들어진 뒤 내용이 바뀌지 않고 리스트 앞쪽에만 추가된다.
	 * 따라서 여기서 앞에서부터 k 개를 적어두면, 병합하는 동안 새 run 이
	 * 앞에 붙더라도 적어둔 것들은 그대로다. 조회 경로도 이 사이에
	 * 리스트를 그대로 훑으므로 데이터가 잠시라도 사라지지 않는다.
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
	/* 스냅샷 구간 바로 다음(더 오래된) 자리를 기억해 둔다 */
	anchor = snap[k - 1]->list.next;
	for (i = 0; i < k; i++) {
		list_del(&snap[i]->list);
		c->nr_runs--;
	}
	/* 원래 스냅샷이 있던 자리에 넣는다 — 그 뒤 run 들보다는 최신이다 */
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
 * Constructor / Destructor
 * ================================================================== */

static int zns_base_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct zns_base_c *c;
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
	c->memtable = RB_ROOT;
	INIT_LIST_HEAD(&c->runs);

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			    &c->dev);
	if (ret) {
		ti->error = "failed to open underlying device";
		kfree(c);
		return ret;
	}

	c->total_sectors = ti->len;
	c->total_blocks  = c->total_sectors >> BLOCK_SHIFT;
	c->wp_offset     = 0;

	/*
	 * LSM 인덱스는 실제로 쓰인 만큼만 자란다. 다만 스핀락 안에서 할당할
	 * 수 없으므로 MemTable 노드 풀과 첫 flush 용 버퍼는 미리 잡아 둔다.
	 */
	c->node_pool = kvmalloc_array(NODE_POOL_MAX, sizeof(*c->node_pool),
				      GFP_KERNEL);
	c->spare     = kvmalloc_array(NODE_POOL_MAX, sizeof(*c->spare),
				      GFP_KERNEL);
	c->spare_run = kzalloc(sizeof(*c->spare_run), GFP_KERNEL);
	if (!c->node_pool || !c->spare || !c->spare_run) {
		ti->error = "cannot allocate LSM index";
		ret = -ENOMEM;
		goto err_free;
	}

	/*
	 * 하위 디바이스의 zone 을 전부 reset 해 wp 를 0 으로 맞춘다.
	 *
	 * 인덱스가 in-memory 라 dtr 에서 통째로 사라진다. 즉 타깃을 새로
	 * 붙이는 시점에 하위에 남아 있던 데이터는 어차피 도달할 수 없다.
	 * 지워도 잃을 것이 없고, 대신 nullb0 를 살려둔 채 타깃만 재생성해도
	 * wp_offset(0) 과 하위의 실제 wp 가 어긋나지 않는다.
	 *
	 * 매핑 영속화(stretch)를 하게 되면 이 결정을 되돌려야 한다.
	 */
	ret = blkdev_zone_mgmt(c->dev->bdev, REQ_OP_ZONE_RESET, 0,
			       bdev_nr_sectors(c->dev->bdev));
	if (ret) {
		ti->error = "failed to reset zones on underlying device";
		goto err_free;
	}

	ti->private        = c;
	ti->num_flush_bios = 1;

	/*
	 * bio 하나가 MemTable 한 세대에 들어갈 수 있는 크기를 넘지 않게 한다.
	 * 넘으면 flush 를 해도 자리가 안 나서 영영 되밀리게 된다.
	 * MEMTABLE_MAX 블록 = 16 MiB 로, 실제 bio 는 보통 이보다 훨씬 작다.
	 */
	ret = dm_set_target_max_io_len(ti, MEMTABLE_MAX << BLOCK_SHIFT);
	if (ret) {
		ti->error = "cannot set max io len";
		goto err_free;
	}

	/*
	 * discard 는 광고하지 않는다. LBA→PBA 매핑을 거치므로 상위가 보낸
	 * 범위를 하위에 그대로 적용하면 남의 데이터를 날린다. 매핑 무효화로
	 * 구현하는 것이 맞고, 그건 M3 의 GC 와 함께 온다.
	 */
	ti->num_discard_bios = 0;

	DMINFO("ctr: attached on '%s', %llu blocks (%llu sectors), LSM index",
	       argv[0],
	       (unsigned long long)c->total_blocks,
	       (unsigned long long)c->total_sectors);
	return 0;

err_free:
	kvfree(c->node_pool);
	kvfree(c->spare);
	kfree(c->spare_run);
	dm_put_device(ti, c->dev);
	kfree(c);
	return ret;
}

static void zns_base_dtr(struct dm_target *ti)
{
	struct zns_base_c *c = ti->private;
	struct zns_run *r, *tmp;

	list_for_each_entry_safe(r, tmp, &c->runs, list) {
		list_del(&r->list);
		kvfree(r->ents);
		kfree(r);
	}

	kvfree(c->node_pool);
	kvfree(c->spare);
	kfree(c->spare_run);
	dm_put_device(ti, c->dev);
	kfree(c);
	DMINFO("dtr: target detached");
}

/* ==================================================================
 * Write I/O — ZNS 에 실제 순차 쓰기를 제출
 * ================================================================== */

/*
 * clone bio 의 완료 콜백.
 * ZNS 장치에서 I/O 가 끝나면 원본 bio 를 같은 상태로 완료시킨다.
 */
static void zns_write_end_io(struct bio *clone)
{
	struct zns_write_io *io = clone->bi_private;
	struct bio *orig        = io->orig;

	orig->bi_status = clone->bi_status;
	bio_endio(orig);

	kfree(io);
	bio_put(clone);
}

/*
 * 임의 LBA 쓰기를 처리한다.
 *
 * 1. spinlock 안에서: 용량 확인 → 인덱스 갱신 → wp 전진 → 시작 PBA 확보.
 * 2. lock 밖에서: bio 를 clone 해 ZNS 로 순차 제출하고, 소비된 flush
 *    버퍼를 다시 채우고, 필요하면 컴팩션을 돌린다.
 *
 * 반환: DM_MAPIO_SUBMITTED (원본 bio 는 콜백에서 완료).
 */
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
	bool need_refill;
	blk_status_t err;

	/* 길이 0 요청은 즉시 완료 */
	if (len_sectors == 0) {
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	/*
	 * 요청이 블록 경계에 정렬되지 않을 수도 있으므로 올림 처리.
	 * 예: 12섹터 쓰기 → 2블록을 점유.
	 */
	nr_blocks = (len_sectors + SECTORS_PER_BLOCK - 1) >> BLOCK_SHIFT;

	/*
	 * clone 을 먼저 잡아 둔다.
	 *
	 * 인덱스를 갱신한 뒤에 할당이 실패하면, 실제로 쓰이지도 않은 PBA 를
	 * 가리키는 매핑이 남는다. 그 블록을 읽으면 기록된 적 없는 물리 위치의
	 * 내용이 돌아온다. 실패할 수 있는 일을 전부 앞으로 몰아두면 잠금
	 * 구간은 실패하지 않는다.
	 */
	io = kmalloc(sizeof(*io), GFP_NOIO);
	if (!io) {
		err = BLK_STS_RESOURCE;
		goto err_out;
	}

	/*
	 * bio_clone_fast() 는 5.18 에서 제거됐다. 대체 API 인 bio_alloc_clone()
	 * 은 대상 bdev 를 직접 인자로 받으므로 뒤따르던 bio_set_dev() 가 필요 없다.
	 */
	clone = bio_alloc_clone(c->dev->bdev, bio, GFP_NOIO, &fs_bio_set);
	if (!clone) {
		kfree(io);
		err = BLK_STS_RESOURCE;
		goto err_out;
	}

	/* ---- 인덱스 갱신 (atomic 하게) ---- */
	spin_lock_irqsave(&c->map_lock, flags);

	/*
	 * GC 가 없으므로(M3) 한 번 전진한 wp 는 되돌아오지 않는다.
	 * 용량을 넘기면 조용히 디바이스 밖으로 나가는 대신 여기서 끊는다.
	 */
	if (c->wp_offset + len_sectors > c->total_sectors) {
		spin_unlock_irqrestore(&c->map_lock, flags);
		DMERR_LIMIT("out of space: wp=%llu + %llu > %llu (GC 미구현 — M3)",
			    (unsigned long long)c->wp_offset,
			    (unsigned long long)len_sectors,
			    (unsigned long long)c->total_sectors);
		err = BLK_STS_NOSPC;
		goto err_free;
	}

	/* MemTable 에 이번 요청이 들어갈 자리가 없으면 먼저 run 으로 내린다 */
	if (c->mem_n + nr_blocks > MEMTABLE_MAX)
		mem_flush_locked(c);

	/*
	 * 자리를 먼저 통째로 확인한다. 넣다가 중간에 모자라면 반쯤 갱신된
	 * 인덱스가 남고, 그 블록들은 쓰이지 않은 PBA 를 가리키게 된다.
	 * (같은 블록 재기록은 노드를 새로 쓰지 않으므로 이 검사는 보수적이다.)
	 */
	if (c->node_used + nr_blocks > NODE_POOL_MAX) {
		spin_unlock_irqrestore(&c->map_lock, flags);
		zns_refill_spares(c);	/* flush 버퍼가 비어 있어서 생긴 상황 */
		DMERR_LIMIT("memtable full, retrying");
		err = BLK_STS_RESOURCE;
		goto err_free;
	}

	pba = c->wp_offset;	/* 이 요청이 기록될 ZNS 상의 시작 섹터 */

	for (i = 0; i < nr_blocks; i++) {
		sector_t idx = block_idx + i;

		if (idx >= c->total_blocks)
			break;
		if (!mem_upsert(c, idx, pba + (i << BLOCK_SHIFT))) {
			/* 위에서 자리를 확인했으므로 여기 올 수 없다 */
			WARN_ON_ONCE(1);
			break;
		}
	}

	c->wp_offset += len_sectors;	/* wp 를 이 요청 크기만큼 전진 */
	need_refill = (!c->spare || !c->spare_run);

	spin_unlock_irqrestore(&c->map_lock, flags);

	/* ---- ZNS 에 순차 쓰기 제출 ---- */
	io->orig = bio;
	clone->bi_iter.bi_sector = pba;
	clone->bi_end_io         = zns_write_end_io;
	clone->bi_private        = io;

	submit_bio_noacct(clone);

	/* 여기는 sleepable 이므로 뒷정리는 I/O 를 띄운 뒤에 한다 */
	if (need_refill)
		zns_refill_spares(c);
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
 * Read I/O — 인덱스 조회 후 remapping 또는 zero-fill
 * ================================================================== */

/*
 * 매핑은 4 KiB 블록 단위인데 bio 는 그보다 크다 (readahead 는 흔히 128 KiB).
 * 그리고 임의 쓰기로 만들어진 매핑이라 인접한 논리 블록이 물리적으로도
 * 인접하다는 보장이 전혀 없다.
 *
 * 따라서 요청 전체를 첫 블록의 PBA 로 remap 하면 안 된다 — 첫 블록만 맞고
 * 나머지는 다른 LBA 의 데이터가 돌아온다. M1 의 성공 기준(fio randwrite 의
 * blk_update_request delta)은 읽기를 아예 하지 않으므로 이 결함을 잡지 못한다.
 *
 * PBA 가 연속으로 이어지는 구간(run)만큼만 처리하고, 남는 부분은
 * dm_accept_partial_bio() 로 DM 에 돌려줘 .map 이 다시 불리게 한다.
 */
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

	/*
	 * 첫 블록에서 쓸 수 있는 만큼으로 시작해, 뒤따르는 블록이 PBA 상
	 * 연속인 동안 구간을 늘린다. 매핑이 없는 블록끼리도 묶어서 한 번에
	 * zero-fill 한다.
	 */
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

	/* 연속 구간이 요청보다 짧으면 그만큼만 받는다. 나머지는 DM 이 다시 부른다. */
	if (run < want)
		dm_accept_partial_bio(bio, (unsigned int)run);

	if (pba != PBA_UNMAPPED) {
		bio_set_dev(bio, c->dev->bdev);
		bio->bi_iter.bi_sector = pba + off_in_block;
		return DM_MAPIO_REMAPPED;
	}

	/* 아직 쓰인 적 없는 구간 → 0으로 채워서 즉시 완료 */
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

	/*
	 * DM 이 데이터와 분리해 내려보내는 빈 flush bio. 길이가 0 이라
	 * 주소 변환할 것이 없으므로 그대로 하위로 넘긴다.
	 */
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
		/*
		 * DISCARD / WRITE_ZEROES 등은 지원하지 않는다.
		 *
		 * 하위로 그냥 넘기면 LBA 를 PBA 로 착각해 엉뚱한 물리 영역을
		 * 날린다 — 그 자리에는 다른 LBA 의 살아 있는 데이터가 있다.
		 * 제대로 하려면 매핑 무효화로 처리해야 하고 그건 M3 의 GC 와
		 * 같이 온다. 그때까지는 광고도 하지 않고(ctr 참고) 받지도 않는다.
		 */
		DMERR_LIMIT("unsupported bio op %u", bio_op(bio));
		return DM_MAPIO_KILL;
	}
}

/* ==================================================================
 * io_hints — 위쪽을 conventional 블록 디바이스로 광고
 *
 * M1 의 핵심: 위쪽(ext4 등)에는 zoned 제약을 완전히 숨긴다.
 *
 * 커널 6.11 전후로 queue_limits.zoned(enum blk_zoned_model)가 사라지고
 * features 비트필드의 BLK_FEAT_ZONED 로 바뀌었다.
 *   구: limits->zoned = BLK_ZONED_NONE;
 *   신: limits->features &= ~BLK_FEAT_ZONED;
 * ================================================================== */

static void zns_base_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct zns_base_c *c = ti->private;

	limits->features &= ~BLK_FEAT_ZONED;
	limits->chunk_sectors = bdev_zone_sectors(c->dev->bdev);
}

/* ==================================================================
 * status — dmsetup status 로 소모량과 인덱스 상태를 들여다본다
 *
 *   $ dmsetup status my-m1-device
 *   0 4194304 zns-base 301056 4194304 1234 3 12 5 40960
 *                      ^wp    ^전체   ^mem ^runs ^flush ^compact ^dropped
 *
 * dropped 는 컴팩션에서 밀려난 매핑 수 = 이제 아무도 안 읽는 물리 블록.
 * GC 가 없는 동안 wp 는 되돌아오지 않으므로, 이 값이 M3 이 회수해야 할
 * 몫의 하한이다.
 * ================================================================== */

static void zns_base_status(struct dm_target *ti, status_type_t type,
			    unsigned int status_flags, char *result,
			    unsigned int maxlen)
{
	struct zns_base_c *c = ti->private;
	unsigned int sz = 0;
	unsigned long flags;
	sector_t wp;
	unsigned int mem_n, nr_runs;
	u64 flushes, compactions, dropped;

	switch (type) {
	case STATUSTYPE_INFO:
		spin_lock_irqsave(&c->map_lock, flags);
		wp          = c->wp_offset;
		mem_n       = c->mem_n;
		nr_runs     = c->nr_runs;
		flushes     = c->stat_flushes;
		compactions = c->stat_compactions;
		dropped     = c->stat_dropped;
		spin_unlock_irqrestore(&c->map_lock, flags);

		DMEMIT("%llu %llu %u %u %llu %llu %llu",
		       (unsigned long long)wp,
		       (unsigned long long)c->total_sectors,
		       mem_n, nr_runs,
		       (unsigned long long)flushes,
		       (unsigned long long)compactions,
		       (unsigned long long)dropped);
		break;
	case STATUSTYPE_TABLE:
		DMEMIT("%s", c->dev->name);
		break;
	default:
		break;
	}
}

/* ==================================================================
 * iterate_devices — underlying 속성을 DM 큐 내부로 전파
 *
 * 지금은 등록하지 않는다. 등록하면 하위의 zoned 속성이 DM 큐로 stack 되어
 * 위쪽을 conventional 로 보이게 하려는 io_hints 와 부딪힐 수 있다.
 * (docs/00-overview.md 의 ".iterate_devices 함정" 참고 — M0 는 반대로
 *  이것이 있어야 zone 이 위로 노출된다.)
 * ================================================================== */

static int __maybe_unused zns_base_iterate_devices(struct dm_target *ti,
						   iterate_devices_callout_fn fn,
						   void *data)
{
	struct zns_base_c *c = ti->private;

	return fn(ti, c->dev, 0, ti->len, data);
}

/* ==================================================================
 * target_type 등록
 *
 * M0 대비 변경점:
 *   - DM_TARGET_ZONED_HM 제거  → 위쪽에 zoned 광고 안 함
 *   - .report_zones 제거       → 위쪽에 zone 정보 노출 안 함
 *   - .io_hints 추가           → BLK_FEAT_ZONED 를 걷어냄
 *   - .status 추가             → wp 소모량과 LSM 인덱스 상태
 * ================================================================== */

static struct target_type zns_base_target = {
	.name            = "zns-base",
	.version         = {0, 3, 0},
	.features        = 0,
	.module          = THIS_MODULE,
	.ctr             = zns_base_ctr,
	.dtr             = zns_base_dtr,
	.map             = zns_base_map,
	.io_hints        = zns_base_io_hints,
	.status          = zns_base_status,
};

static int __init zns_base_init(void)
{
	int ret = dm_register_target(&zns_base_target);

	if (ret < 0)
		DMERR("target registration failed: %d", ret);
	else
		DMINFO("target registered (LSM index)");
	return ret;
}

static void __exit zns_base_exit(void)
{
	dm_unregister_target(&zns_base_target);
	DMINFO("target unregistered");
}

module_init(zns_base_init);
module_exit(zns_base_exit);

MODULE_DESCRIPTION("ZNS target: random-to-sequential translation, LSM mapping");
MODULE_AUTHOR("SPLAB");
MODULE_LICENSE("GPL");
