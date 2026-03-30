#include "rect_texture.h"
#include <string.h>

bool pii_rect_texture_create(pii_rect_texture_t *rt)
{
	memset(rt, 0, sizeof(*rt));

	rt->texture = gs_texture_create(PII_MASK_MAX_RECTS * 2, 1,
					GS_RGBA32F, 1, NULL,
					GS_DYNAMIC);
	return rt->texture != NULL;
}

void pii_rect_texture_update(pii_rect_texture_t *rt,
			     const pii_mask_reader_t *reader,
			     float scale_x, float scale_y)
{
	if (!rt->texture)
		return;

	memset(rt->data, 0, sizeof(rt->data));

	for (uint32_t i = 0; i < reader->rect_count; i++) {
		const pii_mask_rect_t *r = &reader->rects[i];
		float *t0 = &rt->data[i * 2 * 4];     /* texel 0: geometry */
		float *t1 = &rt->data[(i * 2 + 1) * 4]; /* texel 1: params */

		t0[0] = r->x * scale_x;
		t0[1] = r->y * scale_y;
		t0[2] = r->width * scale_x;
		t0[3] = r->height * scale_y;

		t1[0] = r->corner_radius;
		t1[1] = (float)r->flags;
		t1[2] = 0.0f;
		t1[3] = 0.0f;
	}

	gs_texture_set_image(rt->texture, (const uint8_t *)rt->data,
			     PII_MASK_MAX_RECTS * 2 * 4 * sizeof(float),
			     false);
}

void pii_rect_texture_destroy(pii_rect_texture_t *rt)
{
	if (rt->texture) {
		gs_texture_destroy(rt->texture);
		rt->texture = NULL;
	}
}
