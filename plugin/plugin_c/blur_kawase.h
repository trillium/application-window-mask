#ifndef PII_MASK_BLUR_KAWASE_H
#define PII_MASK_BLUR_KAWASE_H

#include <graphics/graphics.h>
#include <obs-module.h>

#define PII_BLUR_MAX_LEVELS 6

typedef struct {
	gs_texrender_t *targets[PII_BLUR_MAX_LEVELS * 2];
	gs_effect_t *effect_down;
	gs_effect_t *effect_up;
	int levels;
} pii_blur_kawase_t;

/* Create blur resources. Call within graphics context. */
bool pii_blur_create(pii_blur_kawase_t *blur, int levels);

/* Run blur passes on source texture. Returns blurred texture.
 * Call within graphics context (during video_render). */
gs_texture_t *pii_blur_render(pii_blur_kawase_t *blur,
			      gs_texture_t *source_tex,
			      uint32_t width, uint32_t height);

/* Destroy blur resources. Call within graphics context. */
void pii_blur_destroy(pii_blur_kawase_t *blur);

#endif /* PII_MASK_BLUR_KAWASE_H */
