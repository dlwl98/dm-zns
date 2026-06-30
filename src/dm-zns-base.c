// SPDX-License-Identifier: GPL-2.0
/*
 * dm-zns-base: M1 Random-to-Sequential Translation Target
 *
 * Exposes a conventional block device interface to the upper layers while
 * handling sequential-only constraints of the underlying ZNS device internally.
 * See docs/07-milestones.md.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>

#include <linux/vmalloc.h>
#include <linux/slab.h>       
#include <linux/workqueue.h> 

struct zns_complete_work {
	struct work_struct work;
	struct bio *bio;
};

static void zns_base_complete_bio_async(struct work_struct *work)
{
	struct zns_complete_work *zw = container_of(work, struct zns_complete_work, work);
	
	zw->bio->bi_status = BLK_STS_OK;
	bio_endio(zw->bio); // 호출 스택이 분리된 안전한 곳에서 완료 통보
	kfree(zw);          // 할당했던 메모리 해제
}

#define DM_MSG_PREFIX "zns-base"

struct zns_base_c {
	struct dm_dev *dev;
	
	sector_t wp_offset;         // ZNS의 현재 쓰기 포인터 (Sector 단위)
	sector_t *mapping_table;    // 4KB 블록 단위 LBA -> 물리 Sector 매핑 배열
	sector_t total_sectors;     // 가상 디바이스 전체 섹터 수
	sector_t total_blocks;      // 4KB 단위 전체 블록 수 (total_sectors / 8)
	
	char *memtable_buf;         // 차후 단계용 MemTable 버퍼
	size_t memtable_size;       // 현재 MemTable 버퍼 크기
};

static int zns_base_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct zns_base_c *c;
	int ret;
	sector_t i;

	if (argc != 1) {
		ti->error = "expected one argument: underlying device";
		return -EINVAL;
	}

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		ti->error = "out of memory";
		return -ENOMEM;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			    &c->dev);
	if (ret) {
		ti->error = "failed to open underlying device";
		kfree(c);
		return ret;
	}

	/* 4KB 블록 단위 매핑 테이블 할당 */
	c->total_sectors = ti->len;
	c->total_blocks = c->total_sectors >> 3; // 1 블록 = 8 섹터(4KB)
	c->wp_offset = 0;

	c->mapping_table = kvmalloc(c->total_blocks * sizeof(sector_t), GFP_KERNEL);
	if (!c->mapping_table) {
		ti->error = "Cannot allocate in-memory mapping table";
		dm_put_device(ti, c->dev);
		kfree(c);
		return -ENOMEM;
	}

	for (i = 0; i < c->total_blocks; i++) {
		c->mapping_table[i] = (sector_t)-1;
	}

	c->memtable_buf = NULL;
	c->memtable_size = 0;

	ti->private = c;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;

	DMINFO("ctr: M1 target attached on top of '%s'", argv[0]);
	return 0;
}

static void zns_base_dtr(struct dm_target *ti)
{
	struct zns_base_c *c = ti->private;

	if (c->mapping_table)
		kvfree(c->mapping_table);
	if (c->memtable_buf)
		kvfree(c->memtable_buf);

	dm_put_device(ti, c->dev);
	kfree(c);
	DMINFO("dtr: target detached");
}

static int zns_base_map(struct dm_target *ti, struct bio *bio)
{
	struct zns_base_c *c = ti->private;
	sector_t lba = bio->bi_iter.bi_sector;
	sector_t len_sectors = bio_sectors(bio);
	sector_t block_idx = lba >> 3; // 4 KiB block index

	if (len_sectors == 0) {
		return DM_MAPIO_REMAPPED;
	}

	/* 1. 임의 쓰기(Random Write) 요청 처리 */
	if (bio_op(bio) == REQ_OP_WRITE) {
		sector_t nr_blocks = len_sectors >> 3;
		sector_t i;
		struct zns_complete_work *zw;

		if (nr_blocks == 0 && len_sectors > 0)
			nr_blocks = 1; 

		// 매핑 테이블에 가상 주소 등록
		for (i = 0; i < nr_blocks; i++) {
			if (block_idx + i < c->total_blocks) {
				c->mapping_table[block_idx + i] = c->wp_offset + (i << 3);
			}
		}

		c->wp_offset += len_sectors;
		zw = kmalloc(sizeof(*zw), GFP_ATOMIC); // map 내부이므로 GFP_ATOMIC 메모리 할당
		if (!zw) {
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		INIT_WORK(&zw->work, zns_base_complete_bio_async);
		zw->bio = bio;
		schedule_work(&zw->work); // 시스템 전역 워크큐에 등록
		
		return DM_MAPIO_SUBMITTED; 
	} 

	/* 2. 읽기(Read) 요청 처리 */
	if (bio_op(bio) == REQ_OP_READ) {
		if (block_idx < c->total_blocks) {
			sector_t pba = c->mapping_table[block_idx];
			
			if (pba != (sector_t)-1) {
				bio->bi_iter.bi_sector = pba;
				return DM_MAPIO_REMAPPED;
			} else {
				zero_fill_bio(bio);
				bio->bi_status = BLK_STS_OK;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}
		}
	}

	return DM_MAPIO_REMAPPED;
}

static int zns_base_report_zones(struct dm_target *ti,
				 struct dm_report_zones_args *args,
				 unsigned int nr_zones)
{
	struct zns_base_c *c = ti->private;

	return dm_report_zones(c->dev->bdev, ti->begin,
			       args->next_sector, args, nr_zones);
}

static int zns_base_iterate_devices(struct dm_target *ti,
				    iterate_devices_callout_fn fn, void *data)
{
	struct zns_base_c *c = ti->private;

	return fn(ti, c->dev, 0, ti->len, data);
}


static void zns_base_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	limits->zoned = BLK_ZONED_NONE;
}

static struct target_type zns_base_target = {
	.name            = "zns-base",
	.version         = {0, 1, 0},
	.features        = DM_TARGET_ZONED_HM, 
	.module          = THIS_MODULE,
	.ctr             = zns_base_ctr,
	.dtr             = zns_base_dtr,
	.map             = zns_base_map,
	.report_zones    = zns_base_report_zones, 
	.iterate_devices = zns_base_iterate_devices,
	.io_hints        = zns_base_io_hints, 
};

static int __init zns_base_init(void)
{
	int ret = dm_register_target(&zns_base_target);

	if (ret < 0)
		DMERR("target registration failed: %d", ret);
	else
		DMINFO("target registered");
	return ret;
}

static void __exit zns_base_exit(void)
{
	dm_unregister_target(&zns_base_target);
	DMINFO("target unregistered");
}

module_init(zns_base_init);
module_exit(zns_base_exit);

MODULE_DESCRIPTION("ZNS M1 target (Random-to-Sequential Conventional Virtual Device)");
MODULE_AUTHOR("SPLAB");
MODULE_LICENSE("GPL");