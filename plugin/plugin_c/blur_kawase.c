#include "blur_kawase.h"

#include <graphics/matrix4.h>
#include <string.h>

bool pii_blur_create(pii_blur_kawase_t *blur, int levels)
{
	memset(blur, 0, sizeof(*blur));

	if (levels < 1)
		levels = 1;
	if (levels > PII_BLUR_MAX_LEVELS)
		levels = PII_BLUR_MAX_LEVELS;
	blur->levels = levels;

	/* Load effect files */
	char *path_down = obs_module_file("dual_kawase_down.effect");
	char *path_up = obs_module_file("dual_kawase_up.effect");

	if (path_down)
		blur->effect_down =
			gs_effect_create_from_file(path_down, NULL);
	if (path_up)
		blur->effect_up =
			gs_effect_create_from_file(path_up, NULL);

	bfree(path_down);
	bfree(path_up);

	if (!blur->effect_down || !blur->effect_up)
		return false;

	/* Create texrender targets: levels for down, levels for up */
	for (int i = 0; i < levels * 2; i++) {
		blur->targets[i] =
			gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!blur->targets[i])
			return false;
	}

	return true;
}

static void render_pass(gs_effect_t *effect, gs_texture_t *tex,
			gs_texrender_t *target,
			uint32_t src_w, uint32_t src_h,
			uint32_t dst_w, uint32_t dst_h)
{
	gs_texrender_reset(target);
	if (!gs_texrender_begin(target, dst_w, dst_h))
		return;

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, (float)dst_w, 0.0f, (float)dst_h, -100.0f, 100.0f);

	gs_eparam_t *p;

	p = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(p, tex);

	struct vec2 texel;
	texel.x = 1.0f / (float)src_w;
	texel.y = 1.0f / (float)src_h;
	p = gs_effect_get_param_by_name(effect, "texel_size");
	gs_effect_set_vec2(p, &texel);

	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, dst_w, dst_h);

	gs_texrender_end(target);
}

gs_texture_t *pii_blur_render(pii_blur_kawase_t *blur,
			      gs_texture_t *source_tex,
			      uint32_t width, uint32_t height)
{
	if (!blur->effect_down || !blur->effect_up || !source_tex)
		return source_tex;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	gs_texture_t *current = source_tex;
	uint32_t w = width;
	uint32_t h = height;

	/* Downsample passes */
	for (int i = 0; i < blur->levels; i++) {
		uint32_t nw = w / 2;
		uint32_t nh = h / 2;
		if (nw < 1) nw = 1;
		if (nh < 1) nh = 1;

		render_pass(blur->effect_down, current, blur->targets[i],
			    w, h, nw, nh);

		current = gs_texrender_get_texture(blur->targets[i]);
		if (!current)
			break;
		w = nw;
		h = nh;
	}

	/* Upsample passes */
	for (int i = blur->levels - 1; i >= 0; i--) {
		uint32_t nw, nh;
		if (i == 0) {
			nw = width;
			nh = height;
		} else {
			nw = width >> (uint32_t)i;
			nh = height >> (uint32_t)i;
			if (nw < 1) nw = 1;
			if (nh < 1) nh = 1;
		}

		int up_idx = blur->levels + (blur->levels - 1 - i);
		render_pass(blur->effect_up, current, blur->targets[up_idx],
			    w, h, nw, nh);

		current = gs_texrender_get_texture(blur->targets[up_idx]);
		if (!current)
			break;
		w = nw;
		h = nh;
	}

	gs_blend_state_pop();

	return current;
}

void pii_blur_destroy(pii_blur_kawase_t *blur)
{
	for (int i = 0; i < PII_BLUR_MAX_LEVELS * 2; i++) {
		if (blur->targets[i]) {
			gs_texrender_destroy(blur->targets[i]);
			blur->targets[i] = NULL;
		}
	}
	if (blur->effect_down) {
		gs_effect_destroy(blur->effect_down);
		blur->effect_down = NULL;
	}
	if (blur->effect_up) {
		gs_effect_destroy(blur->effect_up);
		blur->effect_up = NULL;
	}
}
