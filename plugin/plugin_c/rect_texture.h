#ifndef PII_MASK_RECT_TEXTURE_H
#define PII_MASK_RECT_TEXTURE_H

#include "shm_reader.h"
#include <graphics/graphics.h>

/*
 * Encodes mask rect data into a small 1D GPU texture for shader
 * consumption. Required because OBS .effect uniform arrays are
 * broken on Metal and OpenGL backends.
 *
 * Layout: (PII_MASK_MAX_RECTS * 2, 1) RGBA32F texture.
 * Two texels per rect:
 *   texel[i*2+0] = (x, y, width, height)    -- in source coords
 *   texel[i*2+1] = (corner_radius, flags, 0, 0)
 */

typedef struct {
	gs_texture_t *texture;
	float data[PII_MASK_MAX_RECTS * 2 * 4]; /* CPU-side buffer */
} pii_rect_texture_t;

/* Create the rect data texture. Call within graphics context. */
bool pii_rect_texture_create(pii_rect_texture_t *rt);

/* Update texture contents from reader data. Call within graphics context. */
void pii_rect_texture_update(pii_rect_texture_t *rt,
			     const pii_mask_reader_t *reader,
			     float scale_x, float scale_y);

/* Destroy the texture. Call within graphics context. */
void pii_rect_texture_destroy(pii_rect_texture_t *rt);

#endif /* PII_MASK_RECT_TEXTURE_H */
