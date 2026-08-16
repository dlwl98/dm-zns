// SPDX-License-Identifier: GPL-2.0
/*
 * dm-zns-base: M1 Random-to-Sequential Translation Target
 *
 * 위(upper)쪽: conventional 블록 디바이스로 광고 — ext4 등 임의 I/O 허용.
 * 아래(lower)쪽: host-managed ZNS 디바이스의 sequential-write 제약을 우리가 처리.
 *
 * 핵심 동작:
 *   WRITE  — 임의 LBA로 들어온 쓰기를 wp(write pointer)에 순차 append.
 *             LBA → PBA 매핑을 in-memory 테이블에 기록.
 *             bio를 clone해 실제 ZNS I/O를 제출하고 원본 bio는 그 완료 콜백에서 끝냄.
 *   READ   — 매핑 테이블을 lookup해 PBA가 있으면 해당 위치로 remapping.
 *             아직 쓰인 적 없는 LBA는 zero-fill 후 완료.
 *   FLUSH/DISCARD — underlying으로 pass-through.
 *
 * 동시성: mapping_table 갱신과 wp_offset 전진은 spinlock(map_lock)으로 보호.
 *          실제 bio 제출(submit_bio_noacct)은 lock 밖에서 수행해 blocking 방지.
 *
 * 한계(stretch 항목):
 *   - 매핑 영속화 / crash recovery 없음.
 *   - GC / zone reset 없음 (M3 예정).
 *   - 단일 append log (활성 zone 하나).
 *
 * See docs/07-milestones.md — M1.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define DM_MSG_PREFIX "zns-base"

/* 4 KiB = 8 섹터 = 1 블록 */
#define SECTORS_PER_BLOCK	8U
#define BLOCK_SHIFT		3U   /* log2(SECTORS_PER_BLOCK) */

/* 매핑되지 않은 블록의 sentinel 값 */
#define PBA_UNMAPPED		((sector_t)-1)

/* ------------------------------------------------------------------ *
 * 쓰기 완료 콜백을 위한 context.                                      *
 * bio_clone_fast()로 만든 clone이 ZNS에 도달한 뒤 이 구조체를 통해   *
 * 원본(orig) bio를 완료시킨다.                                         *
 * ------------------------------------------------------------------ */
struct zns_write_io {
	struct bio *orig;   /* 상위 레이어에서 내려온 원본 bio */
};

/* ------------------------------------------------------------------ *
 * Per-target 상태                                                      *
 * ------------------------------------------------------------------ */
struct zns_base_c {
	struct dm_dev  *dev;

	spinlock_t      map_lock;       /* mapping_table + wp_offset 보호 */
	sector_t        wp_offset;      /* ZNS 장치 내 다음 쓰기 위치(섹터) */
	sector_t       *mapping_table;  /* [block_idx] = PBA (섹터 단위) */
	sector_t        total_sectors;
	sector_t        total_blocks;   /* total_sectors >> BLOCK_SHIFT */
};

/* ==================================================================
 * Constructor / Destructor
 * ================================================================== */

static int zns_base_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct zns_base_c *c;
	sector_t i;
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

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			    &c->dev);
	if (ret) {
		ti->error = "failed to open underlying device";
		kfree(c);
		return ret;
	}

	/* 4 KiB 블록 단위 매핑 테이블 할당 */
	c->total_sectors = ti->len;
	c->total_blocks  = c->total_sectors >> BLOCK_SHIFT;
	c->wp_offset     = 0;

	c->mapping_table = kvcalloc(c->total_blocks, sizeof(sector_t),
				    GFP_KERNEL);
	if (!c->mapping_table) {
		ti->error = "cannot allocate mapping table";
		dm_put_device(ti, c->dev);
		kfree(c);
		return -ENOMEM;
	}

	/* 전체 테이블을 PBA_UNMAPPED(-1)로 초기화 */
	for (i = 0; i < c->total_blocks; i++)
		c->mapping_table[i] = PBA_UNMAPPED;

	ti->private          = c;
	ti->num_flush_bios   = 1;
	ti->num_discard_bios = 1;

	DMINFO("ctr: M1 target attached on '%s', %llu blocks (%llu sectors)",
	       argv[0],
	       (unsigned long long)c->total_blocks,
	       (unsigned long long)c->total_sectors);
	return 0;
}

static void zns_base_dtr(struct dm_target *ti)
{
	struct zns_base_c *c = ti->private;

	kvfree(c->mapping_table);
	dm_put_device(ti, c->dev);
	kfree(c);
	DMINFO("dtr: target detached");
}

/* ==================================================================
 * Write I/O — ZNS에 실제 순차 쓰기를 제출
 * ================================================================== */

/*
 * clone bio의 완료 콜백.
 * ZNS 장치에서 I/O가 끝나면 원본 bio를 같은 상태로 완료시킨다.
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
 * 1. spinlock 안에서:
 *      a) LBA → block_idx 계산.
 *      b) 블록마다 mapping_table[block_idx] = wp_offset 기록.
 *      c) wp_offset을 len_sectors만큼 전진.
 *      d) pba = 이 요청의 시작 PBA를 저장.
 * 2. lock 밖에서:
 *      bio를 clone해 ZNS 장치로 순차 제출.
 *
 * 반환: DM_MAPIO_SUBMITTED (원본 bio는 콜백에서 완료).
 */
static int zns_handle_write(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c   = ti->private;
	sector_t lba           = bio->bi_iter.bi_sector;
	sector_t len_sectors   = bio_sectors(bio);
	sector_t block_idx     = lba >> BLOCK_SHIFT;
	sector_t nr_blocks;
	sector_t pba;
	sector_t i;
	struct zns_write_io *io;
	struct bio *clone;
	unsigned long flags;

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

	/* ---- 매핑 테이블 갱신 (atomic하게) ---- */
	spin_lock_irqsave(&c->map_lock, flags);

	pba = c->wp_offset; /* 이 요청이 실제로 기록될 ZNS상의 시작 섹터 */

	for (i = 0; i < nr_blocks; i++) {
		sector_t idx = block_idx + i;

		if (idx < c->total_blocks)
			c->mapping_table[idx] = pba + (i << BLOCK_SHIFT);
	}

	c->wp_offset += len_sectors; /* wp를 이 요청 크기만큼 전진 */

	spin_unlock_irqrestore(&c->map_lock, flags);

	/* ---- ZNS에 순차 쓰기 제출 ---- */
	io = kmalloc(sizeof(*io), GFP_NOIO);
	if (!io)
		goto err_io;

	/*
	 * bio_clone_fast()는 5.18에서 제거됐다. 대체 API인 bio_alloc_clone()은
	 * 대상 bdev를 직접 인자로 받으므로 뒤따르던 bio_set_dev()가 필요 없다.
	 */
	clone = bio_alloc_clone(c->dev->bdev, bio, GFP_NOIO, &fs_bio_set);
	if (!clone) {
		kfree(io);
		goto err_io;
	}

	io->orig = bio;

	/* 순차 쓰기 위치 지정 */
	clone->bi_iter.bi_sector = pba;
	clone->bi_end_io         = zns_write_end_io;
	clone->bi_private        = io;

	submit_bio_noacct(clone);
	return DM_MAPIO_SUBMITTED;

err_io:
	bio->bi_status = BLK_STS_RESOURCE;
	bio_endio(bio);
	return DM_MAPIO_SUBMITTED;
}

/* ==================================================================
 * Read I/O — 매핑 테이블 lookup 후 remapping 또는 zero-fill
 * ================================================================== */

static int zns_handle_read(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c = ti->private;
	sector_t lba         = bio->bi_iter.bi_sector;
	sector_t block_idx   = lba >> BLOCK_SHIFT;
	sector_t pba;
	unsigned long flags;

	spin_lock_irqsave(&c->map_lock, flags);

	pba = (block_idx < c->total_blocks)
		? c->mapping_table[block_idx]
		: PBA_UNMAPPED;

	spin_unlock_irqrestore(&c->map_lock, flags);

	if (pba != PBA_UNMAPPED) {
		/*
		 * LBA 내의 섹터 오프셋을 PBA에 반영한다.
		 * 블록 정렬되지 않은 읽기도 처리.
		 */
		sector_t sector_in_block = lba & (SECTORS_PER_BLOCK - 1);

		bio_set_dev(bio, c->dev->bdev);
		bio->bi_iter.bi_sector = pba + sector_in_block;
		return DM_MAPIO_REMAPPED;
	}

	/* 아직 쓰인 적 없는 블록 → 0으로 채워서 즉시 완료 */
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

	/* FLUSH / DISCARD는 underlying으로 pass-through */
	if (bio->bi_opf & REQ_PREFLUSH || bio_op(bio) == REQ_OP_DISCARD) {
		bio_set_dev(bio, c->dev->bdev);
		return DM_MAPIO_REMAPPED;
	}

	switch (bio_op(bio)) {
	case REQ_OP_WRITE:
		return zns_handle_write(ti, bio);
	case REQ_OP_READ:
		return zns_handle_read(ti, bio);
	default:
		/* WRITE_ZEROES 등은 pass-through */
		bio_set_dev(bio, c->dev->bdev);
		return DM_MAPIO_REMAPPED;
	}
}

/* ==================================================================
 * io_hints — 위쪽을 conventional 블록 디바이스로 광고
 *
 * M1의 핵심: 위쪽(ext4 등)에는 zoned 제약을 완전히 숨긴다.
 *
 * 커널 6.11 전후로 queue_limits.zoned(enum blk_zoned_model)가 사라지고
 * features 비트필드의 BLK_FEAT_ZONED로 바뀌었다.
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
 * iterate_devices — underlying 속성을 DM 큐 내부로 전파
 *
 * io_hints가 위쪽에 zoned를 숨기더라도, DM 레이어 내부에서
 * chunk_sectors를 알아야 blk-mq 큐 설정이 올바르게 된다.
 * ================================================================== */

static int zns_base_iterate_devices(struct dm_target *ti,
				    iterate_devices_callout_fn fn, void *data)
{
	struct zns_base_c *c = ti->private;

	return fn(ti, c->dev, 0, ti->len, data);
}

/* ==================================================================
 * target_type 등록
 *
 * M1에서 M0 대비 변경점:
 *   - DM_TARGET_ZONED_HM 제거  → 위쪽에 zoned 광고 안 함
 *   - .report_zones 제거       → 위쪽에 zone 정보 노출 안 함
 *   - .io_hints 추가           → BLK_ZONED_NONE 광고
 *   - .version 0.2.0           → M1 식별
 * ================================================================== */

static struct target_type zns_base_target = {
	.name            = "zns-base",
	.version         = {0, 2, 0},
	.features        = 0, //DM_TARGET_ZONED_HM,
	.module          = THIS_MODULE,
	.ctr             = zns_base_ctr,
	.dtr             = zns_base_dtr,
	.map             = zns_base_map,
	/* .report_zones 없음 — 위쪽에 zoned를 노출하지 않음 */
	//.iterate_devices = zns_base_iterate_devices,
	.io_hints        = zns_base_io_hints,
};

static int __init zns_base_init(void)
{
	int ret = dm_register_target(&zns_base_target);

	if (ret < 0)
		DMERR("target registration failed: %d", ret);
	else
		DMINFO("M1 target registered");
	return ret;
}

static void __exit zns_base_exit(void)
{
	dm_unregister_target(&zns_base_target);
	DMINFO("target unregistered");
}

module_init(zns_base_init);
module_exit(zns_base_exit);

MODULE_DESCRIPTION("ZNS M1 target: random-to-sequential translation");
MODULE_AUTHOR("SPLAB");
MODULE_LICENSE("GPL");