#include "mask_renderer.h"

#include <graphics/graphics.h>
#include <graphics/matrix4.h>

static void draw_black_rect(float x, float y, float w, float h)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");

	struct vec4 black;
	vec4_zero(&black);
	black.w = 1.0f; /* fully opaque */

	gs_effect_set_vec4(color_param, &black);

	while (gs_effect_loop(solid, "Solid")) {
		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0.0f);
		gs_draw_sprite(NULL, 0, (uint32_t)w, (uint32_t)h);
		gs_matrix_pop();
	}
}

void pii_mask_draw_masks(obs_source_t *source,
		     const pii_mask_reader_t *reader,
		     bool full_mask,
		     uint32_t base_width,
		     uint32_t base_height)
{
	/* Draw the source first */
	obs_source_t *target = obs_filter_get_target(source);
	if (!target)
		return;

	obs_source_video_render(target);

	if (full_mask) {
		draw_black_rect(0, 0, (float)base_width, (float)base_height);
		return;
	}

	/* Scale from screen coords to source coords */
	float sx = (reader->screen_width > 0)
		? (float)base_width / reader->screen_width
		: 1.0f;
	float sy = (reader->screen_height > 0)
		? (float)base_height / reader->screen_height
		: 1.0f;

	/* Draw black rects over unsafe regions */
	for (uint32_t i = 0; i < reader->rect_count; i++) {
		const pii_mask_rect_t *r = &reader->rects[i];
		if (!(r->flags & PII_RECT_FLAG_UNSAFE))
			continue;

		draw_black_rect(r->x * sx, r->y * sy,
				r->width * sx, r->height * sy);
	}
}
