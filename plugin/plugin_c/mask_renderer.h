#ifndef PII_MASK_RENDERER_H
#define PII_MASK_RENDERER_H

#include "shm_reader.h"
#include <obs-module.h>

/*
 * Render mask rects over the current video frame.
 *
 * For each unsafe rect in the reader's data, draws a solid black
 * rectangle at the given position. The source is the parent source
 * being filtered.
 *
 * If full_mask is true, blacks out the entire frame.
 */
void pii_mask_draw_masks(obs_source_t *source,
		     const pii_mask_reader_t *reader,
		     bool full_mask,
		     uint32_t base_width,
		     uint32_t base_height);

#endif /* PII_MASK_RENDERER_H */
