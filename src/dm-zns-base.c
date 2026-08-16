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

	/*
	 * 하위 디바이스의 zone 을 전부 reset 해 wp 를 0 으로 맞춘다.
	 *
	 * 매핑 테이블이 in-memory 라 dtr 에서 통째로 사라진다. 즉 타깃을 새로
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
		kvfree(c->mapping_table);
		dm_put_device(ti, c->dev);
		kfree(c);
		return ret;
	}

	ti->private          = c;
	ti->num_flush_bios   = 1;

	/*
	 * discard 는 광고하지 않는다. LBA→PBA 매핑을 거치므로 상위가 보낸
	 * 범위를 하위에 그대로 적용하면 남의 데이터를 날린다. 매핑 무효화로
	 * 구현하는 것이 맞고, 그건 M3 의 GC 와 함께 온다.
	 */
	ti->num_discard_bios = 0;

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
		bio->bi_status = BLK_STS_NOSPC;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

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
		? c->mapping_table[block_idx]
		: PBA_UNMAPPED;

	/*
	 * 첫 블록에서 쓸 수 있는 만큼으로 시작해, 뒤따르는 블록이 PBA 상
	 * 연속인 동안 구간을 늘린다. 매핑이 없는 블록끼리도 묶어서 한 번에
	 * zero-fill 한다.
	 */
	run = SECTORS_PER_BLOCK - off_in_block;
	while (run < want && block_idx + i < c->total_blocks) {
		sector_t next = c->mapping_table[block_idx + i];

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
 * status — dmsetup status 로 소모량을 들여다본다
 *
 * GC 가 없는 동안(M3 이전) wp 는 되돌아오지 않으므로, 논리적으로 같은
 * 자리를 덮어써도 물리 공간은 계속 줄어든다. 그 소모를 눈으로 봐야
 * M2 의 여유가 얼마나 남는지, M3 이 무엇을 회수해야 하는지 알 수 있다.
 *
 *   $ dmsetup status my-m1-device
 *   0 4194304 zns-base 1234567 4194304
 *                      ^wp      ^전체(섹터)
 * ================================================================== */

static void zns_base_status(struct dm_target *ti, status_type_t type,
			    unsigned int status_flags, char *result,
			    unsigned int maxlen)
{
	struct zns_base_c *c = ti->private;
	unsigned int sz = 0;
	unsigned long flags;
	sector_t wp;

	switch (type) {
	case STATUSTYPE_INFO:
		spin_lock_irqsave(&c->map_lock, flags);
		wp = c->wp_offset;
		spin_unlock_irqrestore(&c->map_lock, flags);

		DMEMIT("%llu %llu",
		       (unsigned long long)wp,
		       (unsigned long long)c->total_sectors);
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
	.status          = zns_base_status,
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