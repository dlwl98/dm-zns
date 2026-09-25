/* SPDX-License-Identifier: GPL-2.0 */
/*
 * zns-compat.h — 커널 버전별 블록 계층 API 차이 흡수
 *
 * 빌드 확인: 5.15, 6.17. 5.18~6.10 은 아래 API 가 섞여 있어 빌드를 막는다.
 *
 * 경계:
 *   5.18  bio_alloc_clone() 도입 / bio_clone_fast() 제거,
 *         bio_alloc() 가 (bdev, nr_vecs, opf, gfp) 로 바뀜,
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0) && \
    LINUX_VERSION_CODE <  KERNEL_VERSION(6, 11, 0)
#error "dm-zns-base: unsupported kernel (5.18 - 6.10)"
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
typedef enum req_opf zns_req_op_t;
#else
typedef enum req_op zns_req_op_t;
#endif

/* 원본 bio 를 bdev 로 향하는 clone 으로 복제 */
static inline struct bio *zns_bio_clone(struct block_device *bdev,
					struct bio *src, gfp_t gfp,
					struct bio_set *bs)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
	struct bio *b = bio_clone_fast(src, gfp, bs);

	if (b)
		bio_set_dev(b, bdev);
	return b;
#else
	return bio_alloc_clone(bdev, src, gfp, bs);
#endif
}

/* bdev 로 향하는 빈 bio 할당 (fs_bio_set) */
static inline struct bio *zns_bio_alloc(struct block_device *bdev,
					unsigned short nr_vecs,
					unsigned int opf, gfp_t gfp)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
	struct bio *b = bio_alloc(gfp, nr_vecs);

	if (b) {
		bio_set_dev(b, bdev);
		b->bi_opf = opf;
	}
	return b;
#else
	return bio_alloc(bdev, nr_vecs, opf, gfp);
#endif
}

static inline int zns_zone_mgmt(struct block_device *bdev, zns_req_op_t op,
				sector_t sector, sector_t nr_sectors)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 0)
	return blkdev_zone_mgmt(bdev, op, sector, nr_sectors, GFP_NOIO);
#else
	return blkdev_zone_mgmt(bdev, op, sector, nr_sectors);
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
#else
	limits->features &= ~BLK_FEAT_ZONED;
#endif
}

#endif /* ZNS_COMPAT_H */
