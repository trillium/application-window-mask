#ifndef PII_MASK_RENDERER_H
#define PII_MASK_RENDERER_H

#include "shm_reader.h"
#include "rect_texture.h"
#include <obs-module.h>

/*
 * Composite clear and obfuscated textures using SDF mask rects.
 *
 * Uses a custom effect shader that evaluates SDF rounded rects
 * from the rect data texture. Unsafe regions show obfuscated,
 * safe regions show clear.
 *
 * If full_mask is true, draws the obfuscated texture over the
 * entire frame (fail-safe). If effect is NULL, falls back to
 * drawing the clear texture with black rect overlays.
 */
void pii_mask_composite(gs_texture_t *clear_tex,
			gs_texture_t *obfuscated_tex,
			gs_effect_t *effect,
			const pii_mask_reader_t *reader,
			const pii_rect_texture_t *rect_tex,
			bool full_mask,
			uint32_t base_width,
			uint32_t base_height,
			float feather);

#endif /* PII_MASK_RENDERER_H */
