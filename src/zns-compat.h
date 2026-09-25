/* SPDX-License-Identifier: GPL-2.0 */
/*
 * zns-compat.h — 커널 버전별 블록 계층 API 차이 흡수
 *
 * 빌드 확인: 5.15, 6.17. 5.18~6.10 은 아래 API 가 섞여 있어 빌드를 막는다.
 *
 * 경계:
 *   5.18  bio_alloc_bioset() 가 (bdev, nr_vecs, opf, gfp, bs) 로 바뀜,
 *         dm_submit_bio_remap() 도입,
 *         kvmalloc() 이 GFP_NOIO 에서도 vmalloc 폴백 (5.18 이전은 우회 경로)
 *   6.0   enum req_opf → enum req_op
 *   6.9   blkdev_zone_mgmt() 에서 gfp_mask 인자 제거
 *   6.11  queue_limits.zoned → features 비트 BLK_FEAT_ZONED
 */
#ifndef ZNS_COMPAT_H
#define ZNS_COMPAT_H

#include <linux/version.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/string.h>
#include <linux/device-mapper.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0)
#include <linux/elevator.h>	/* 5.16 에 block/ 안으로 옮겨져 모듈에 안 보인다 */
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0) && \
    LINUX_VERSION_CODE <  KERNEL_VERSION(6, 11, 0)
#error "dm-zns-base: unsupported kernel (5.18 - 6.10)"
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
typedef enum req_opf zns_req_op_t;
#else
typedef enum req_op zns_req_op_t;
#endif

/* bdev 로 향하는 빈 bio 를 주어진 bio_set 에서 할당 */
static inline struct bio *zns_bio_alloc_bs(struct block_device *bdev,
					   unsigned short nr_vecs,
					   unsigned int opf, gfp_t gfp,
					   struct bio_set *bs)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
	struct bio *b = bio_alloc_bioset(gfp, nr_vecs, bs);

	if (b) {
		bio_set_dev(b, bdev);
		b->bi_opf = opf;
	}
	return b;
#else
	return bio_alloc_bioset(bdev, nr_vecs, opf, gfp, bs);
#endif
}

/*
 * .map 에서 DM_MAPIO_SUBMITTED 로 받아 둔 DM bio 를 나중에(워커에서) 하위로
 * 보낸다. 6.x 는 DM 의 I/O 회계를 위해 dm_submit_bio_remap() 을 거쳐야 한다.
 */
static inline void zns_submit_remapped(struct bio *bio)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
	submit_bio_noacct(bio);
#else
	dm_submit_bio_remap(bio, NULL);
#endif
}

/*
 * zone reset. GC 는 쓰기 진행이 걸린 경로라 reclaim 이 I/O 를 내면 안 된다.
 * 6.9+ 는 gfp 인자가 없어지고 내부에서 GFP_KERNEL 로 할당하므로 scope API 로
 * NOIO 를 건다.
 */
static inline int zns_zone_mgmt(struct block_device *bdev, zns_req_op_t op,
				sector_t sector, sector_t nr_sectors)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
	return blkdev_zone_mgmt(bdev, op, sector, nr_sectors, GFP_NOIO);
#else
	unsigned int noio = memalloc_noio_save();
	int ret = blkdev_zone_mgmt(bdev, op, sector, nr_sectors);

	memalloc_noio_restore(noio);
	return ret;
#endif
}

/*
 * zone 별 쓰기 순서를 하위 블록 계층이 지켜주는가.
 *
 * 6.10+ 는 zone write plugging 이 스케줄러와 무관하게 zone 당 쓰기 하나씩,
 * 도착 순서대로 내보낸다. 그 전(5.15)에는 mq-deadline 의 zone write lock 이
 * 유일한 장치라, 다른 스케줄러(none/kyber/bfq)면 순서대로 보낸 쓰기도 하드웨어
 * 큐 사이에서 뒤바뀔 수 있다.
 */
#define ZNS_SCHED_ZONE_LOCK	(LINUX_VERSION_CODE < KERNEL_VERSION(6, 10, 0))

/*
 * 이 타깃은 bio 를 .map 에서 받아 두었다가 나중에 제출한다. 5.18+ 에서는
 * 그 경우 제출 시점(dm_submit_bio_remap)에만 I/O 회계를 시작하도록 알려야
 * 이중 계산이 없다.
 */
static inline void zns_set_accounts_remapped(struct dm_target *ti)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
	ti->accounts_remapped_io = true;
#endif
}

static inline bool zns_zone_order_backend_ok(struct block_device *bdev)
{
#if ZNS_SCHED_ZONE_LOCK
	struct request_queue *q = bdev_get_queue(bdev);
	bool ok;

	mutex_lock(&q->sysfs_lock);
	ok = q->elevator &&
	     !strcmp(q->elevator->type->elevator_name, "mq-deadline");
	mutex_unlock(&q->sysfs_lock);
	return ok;
#else
	return true;
#endif
}

/*
 * I/O 경로에서 쓰는 큰 배열 할당.
 *
 * 5.15 의 kvmalloc() 은 플래그가 GFP_KERNEL 의 상위집합이 아니면(GFP_NOIO 등)
 * vmalloc 폴백 없이 kmalloc 만 시도한다. 그래서 MAX_ORDER 를 넘는 배열
 * (예: 2 GiB zone 의 GC 후보 524,288개 × 16B = 8 MiB)은 __alloc_pages 경고와
 * 함께 항상 실패한다.
 *
 * 우회는 커널이 권하는 scope API: NOIO 는 memalloc_noio_save() 로 표현하고
 * 할당 자체는 GFP_KERNEL 로 부른다. 그러면 vmalloc 폴백이 살아나면서도
 * reclaim 이 I/O 를 내지 않는다(우리 디바이스로 재귀하지 않는다).
 * 잠들 수 없는 플래그(GFP_ATOMIC)는 vmalloc 을 못 쓰므로 그대로 둔다.
 */
static inline void *zns_kvmalloc_array(size_t n, size_t size, gfp_t gfp)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
	if (gfpflags_allow_blocking(gfp) && (gfp & GFP_KERNEL) != GFP_KERNEL) {
		unsigned int noio = memalloc_noio_save();
		void *p = kvmalloc_array(n, size, gfp | GFP_KERNEL);

		memalloc_noio_restore(noio);
		return p;
	}
#endif
	return kvmalloc_array(n, size, gfp);
}

/* 위쪽 큐를 conventional(비-zoned) 로 광고 */
static inline void zns_limits_clear_zoned(struct queue_limits *limits)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	limits->zoned = BLK_ZONED_NONE;
	limits->max_zone_append_sectors = 0;
#else
	limits->features &= ~BLK_FEAT_ZONED;
#endif
}

#endif /* ZNS_COMPAT_H */
