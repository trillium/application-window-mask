#ifndef PII_MASK_SHM_READER_H
#define PII_MASK_SHM_READER_H

#include "pii_mask_protocol_c.h"
#include <stdbool.h>

typedef struct {
	int fd;
	pii_mask_shm_t *shm;
	pii_mask_rect_t rects[PII_MASK_MAX_RECTS];
	uint32_t rect_count;
	uint32_t flags;
	uint16_t screen_width;
	uint16_t screen_height;
	bool connected;
	bool stale;
} pii_mask_reader_t;

/* Open shared memory. Returns true on success. */
bool pii_mask_reader_open(pii_mask_reader_t *reader);

/* Read current rects from shm via seqlock. Returns true if new data read. */
bool pii_mask_reader_update(pii_mask_reader_t *reader);

/* Close shared memory. */
void pii_mask_reader_close(pii_mask_reader_t *reader);

#endif /* PII_MASK_SHM_READER_H */
