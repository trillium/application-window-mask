#ifndef PII_MASK_PROTOCOL_H
#define PII_MASK_PROTOCOL_H

#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

#define PII_MASK_SHM_NAME "/pii_mask"
#define PII_MASK_MAGIC    0x50494D53  /* "PIMS" */
#define PII_MASK_VERSION  1
#define PII_MASK_MAX_RECTS 32

#define PII_MASK_FLAG_DAEMON_ALIVE  (1 << 0)
#define PII_MASK_FLAG_FULL_MASK     (1 << 1)

#define PII_RECT_FLAG_UNSAFE        (1 << 0)

typedef struct {
	float x;
	float y;
	float width;
	float height;
	float corner_radius;
	uint32_t flags;
} pii_mask_rect_t;

typedef struct {
	uint32_t magic;
	uint32_t version;
	_Atomic uint32_t sequence;
	uint32_t rect_count;
	uint64_t timestamp_ns;
	uint32_t flags;
	uint16_t screen_width;   /* logical screen width for coordinate scaling */
	uint16_t screen_height;  /* logical screen height for coordinate scaling */
	pii_mask_rect_t rects[PII_MASK_MAX_RECTS];
} pii_mask_shm_t;

_Static_assert(sizeof(pii_mask_rect_t) == 24, "rect must be 24 bytes");
_Static_assert(sizeof(pii_mask_shm_t) == 800, "shm must be 800 bytes");

/* --- Seqlock reader (plugin side) --- */

static inline int pii_mask_read_begin(const pii_mask_shm_t *shm, uint32_t *seq)
{
	*seq = atomic_load_explicit(&shm->sequence, memory_order_acquire);
	return (*seq & 1) == 0;  /* even = ready, odd = writing */
}

static inline int pii_mask_read_valid(const pii_mask_shm_t *shm, uint32_t seq)
{
	atomic_thread_fence(memory_order_acquire);
	return atomic_load_explicit(&shm->sequence, memory_order_relaxed) == seq;
}

/* --- Staleness check --- */

static inline int pii_mask_is_stale(const pii_mask_shm_t *shm,
                                     uint64_t timeout_ns)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	return (now_ns - shm->timestamp_ns) > timeout_ns;
}

#define PII_MASK_STALE_TIMEOUT_NS (5ULL * 1000000000ULL)  /* 5 seconds */

#endif /* PII_MASK_PROTOCOL_H */
