#include "mask_renderer.h"

#include <graphics/graphics.h>
#include <graphics/matrix4.h>

static void draw_solid_rect(float x, float y, float w, float h,
			    struct vec4 *color)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *cparam = gs_effect_get_param_by_name(solid, "color");

	gs_effect_set_vec4(cparam, color);

	while (gs_effect_loop(solid, "Solid")) {
		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0.0f);
		gs_draw_sprite(NULL, 0, (uint32_t)w, (uint32_t)h);
		gs_matrix_pop();
	}
}

static void draw_texture_simple(gs_texture_t *tex, uint32_t w, uint32_t h)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, tex);

	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, w, h);
}

/* Fallback: draw clear texture with black rect overlays (no shader) */
static void fallback_render(gs_texture_t *clear_tex,
			    const pii_mask_reader_t *reader,
			    bool full_mask,
			    uint32_t w, uint32_t h)
{
	draw_texture_simple(clear_tex, w, h);

	struct vec4 black;
	vec4_zero(&black);
	black.w = 1.0f;

	if (full_mask) {
		draw_solid_rect(0, 0, (float)w, (float)h, &black);
		return;
	}

	float sx = (reader->screen_width > 0)
		? (float)w / reader->screen_width : 1.0f;
	float sy = (reader->screen_height > 0)
		? (float)h / reader->screen_height : 1.0f;

	for (uint32_t i = 0; i < reader->rect_count; i++) {
		const pii_mask_rect_t *r = &reader->rects[i];
		if (!(r->flags & PII_RECT_FLAG_UNSAFE))
			continue;
		draw_solid_rect(r->x * sx, r->y * sy,
				r->width * sx, r->height * sy, &black);
	}
}

void pii_mask_composite(gs_texture_t *clear_tex,
			gs_texture_t *obfuscated_tex,
			gs_effect_t *effect,
			const pii_mask_reader_t *reader,
			const pii_rect_texture_t *rect_tex,
			bool full_mask,
			uint32_t base_width,
			uint32_t base_height,
			float feather)
{
	if (!clear_tex)
		return;

	/* Full mask = show obfuscated everywhere */
	if (full_mask) {
		if (obfuscated_tex)
			draw_texture_simple(obfuscated_tex,
					    base_width, base_height);
		else {
			struct vec4 black;
			vec4_zero(&black);
			black.w = 1.0f;
			draw_texture_simple(clear_tex,
					    base_width, base_height);
			draw_solid_rect(0, 0, (float)base_width,
					(float)base_height, &black);
		}
		return;
	}

	/* No effect or no rect texture: fallback to black rects */
	if (!effect || !rect_tex || !rect_tex->texture) {
		fallback_render(clear_tex, reader, false,
				base_width, base_height);
		return;
	}

	/* No obfuscated texture: just show clear */
	if (!obfuscated_tex) {
		draw_texture_simple(clear_tex, base_width, base_height);
		return;
	}

	/* SDF composite via custom effect */
	gs_eparam_t *p;

	p = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(p, clear_tex);

	p = gs_effect_get_param_by_name(effect, "image_obfuscated");
	gs_effect_set_texture(p, obfuscated_tex);

	p = gs_effect_get_param_by_name(effect, "rect_data");
	gs_effect_set_texture(p, rect_tex->texture);

	p = gs_effect_get_param_by_name(effect, "num_rects");
	gs_effect_set_int(p, (int)reader->rect_count);

	struct vec2 res;
	res.x = (float)base_width;
	res.y = (float)base_height;
	p = gs_effect_get_param_by_name(effect, "resolution");
	gs_effect_set_vec2(p, &res);

	p = gs_effect_get_param_by_name(effect, "feather");
	gs_effect_set_float(p, feather);

	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(clear_tex, 0, base_width, base_height);
}
