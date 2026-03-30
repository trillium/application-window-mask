#include <obs-module.h>
#include "shm_reader.h"
#include "mask_renderer.h"
#include "rect_texture.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("pii-mask-filter", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "PII Mask: real-time privacy masking via shared memory";
}

struct pii_mask_filter {
	obs_source_t *source;
	pii_mask_reader_t reader;
	gs_texrender_t *texrender_clear;
	gs_texrender_t *texrender_obfuscated;
	pii_rect_texture_t rect_tex;
	gs_effect_t *effect_composite;
	int connect_retry_frames;
	bool force_full_mask;
	float feather;
	uint32_t cx;
	uint32_t cy;
};

static const char *pii_mask_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "PII Mask";
}

static void pii_mask_update(void *data, obs_data_t *settings)
{
	struct pii_mask_filter *f = data;
	f->force_full_mask = obs_data_get_bool(settings, "force_full_mask");
	f->feather = (float)obs_data_get_double(settings, "feather");
}

static void *pii_mask_create(obs_data_t *settings, obs_source_t *source)
{
	struct pii_mask_filter *f = bzalloc(sizeof(struct pii_mask_filter));
	f->source = source;
	f->connect_retry_frames = 0;

	/* Load custom composite effect */
	char *path = obs_module_file("pii_composite.effect");
	if (path) {
		obs_enter_graphics();
		f->effect_composite =
			gs_effect_create_from_file(path, NULL);
		obs_leave_graphics();
		bfree(path);
	}

	obs_enter_graphics();
	f->texrender_clear = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->texrender_obfuscated = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	pii_rect_texture_create(&f->rect_tex);
	obs_leave_graphics();

	pii_mask_reader_open(&f->reader);
	pii_mask_update(f, settings);

	return f;
}

static void pii_mask_destroy(void *data)
{
	struct pii_mask_filter *f = data;

	obs_enter_graphics();
	gs_texrender_destroy(f->texrender_clear);
	gs_texrender_destroy(f->texrender_obfuscated);
	pii_rect_texture_destroy(&f->rect_tex);
	if (f->effect_composite)
		gs_effect_destroy(f->effect_composite);
	obs_leave_graphics();

	pii_mask_reader_close(&f->reader);
	bfree(f);
}

static obs_properties_t *pii_mask_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "status_text", "Status",
				OBS_TEXT_INFO);
	obs_properties_add_float_slider(props, "feather",
					"Edge softness (px)", 0.0, 20.0, 0.5);
	obs_properties_add_bool(props, "force_full_mask",
				"Force full mask (emergency)");

	return props;
}

static void pii_mask_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "force_full_mask", false);
	obs_data_set_default_double(settings, "feather", 2.0);
}

static void pii_mask_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct pii_mask_filter *f = data;

	/* Try to connect if not yet connected */
	if (!f->reader.connected) {
		f->connect_retry_frames++;
		if (f->connect_retry_frames >= 30) {
			pii_mask_reader_open(&f->reader);
			f->connect_retry_frames = 0;
		}
		return;
	}

	pii_mask_reader_update(&f->reader);

	/* Update status text in properties */
	const char *status;
	char buf[128];
	if (!f->reader.connected)
		status = "Disconnected";
	else if (f->reader.stale)
		status = "Stale (daemon not responding)";
	else {
		uint32_t unsafe_count = 0;
		for (uint32_t i = 0; i < f->reader.rect_count; i++) {
			if (f->reader.rects[i].flags & PII_RECT_FLAG_UNSAFE)
				unsafe_count++;
		}
		snprintf(buf, sizeof(buf), "Connected — %u mask rects",
			 unsafe_count);
		status = buf;
	}

	obs_data_t *settings = obs_source_get_settings(f->source);
	obs_data_set_string(settings, "status_text", status);
	obs_data_release(settings);
}

/* Render obfuscated frame (solid black for now — blur/pixelate added later) */
static gs_texture_t *render_obfuscated(struct pii_mask_filter *f,
				       uint32_t width, uint32_t height)
{
	gs_texrender_reset(f->texrender_obfuscated);

	if (gs_texrender_begin(f->texrender_obfuscated, width, height)) {
		struct vec4 black;
		vec4_zero(&black);
		black.w = 1.0f;
		gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
		gs_texrender_end(f->texrender_obfuscated);
	}

	return gs_texrender_get_texture(f->texrender_obfuscated);
}

static void pii_mask_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	struct pii_mask_filter *f = data;
	obs_source_t *target = obs_filter_get_target(f->source);
	if (!target)
		return;

	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);
	if (width == 0 || height == 0)
		return;

	f->cx = width;
	f->cy = height;

	bool full_mask = !f->reader.connected ||
			 f->reader.stale ||
			 f->force_full_mask ||
			 (f->reader.flags & PII_MASK_FLAG_FULL_MASK);

	/* Pass 1: Capture clear frame */
	gs_texrender_reset(f->texrender_clear);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(f->texrender_clear, width, height)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height,
			 -100.0f, 100.0f);
		obs_source_video_render(target);
		gs_texrender_end(f->texrender_clear);
	}

	gs_blend_state_pop();

	gs_texture_t *clear_tex =
		gs_texrender_get_texture(f->texrender_clear);
	if (!clear_tex)
		return;

	/* Pass 2: Render obfuscated frame */
	gs_texture_t *obf_tex = render_obfuscated(f, width, height);

	/* Update rect data texture */
	float sx = (f->reader.screen_width > 0)
		? (float)width / f->reader.screen_width : 1.0f;
	float sy = (f->reader.screen_height > 0)
		? (float)height / f->reader.screen_height : 1.0f;
	pii_rect_texture_update(&f->rect_tex, &f->reader, sx, sy);

	/* Pass 3: SDF composite */
	pii_mask_composite(clear_tex, obf_tex, f->effect_composite,
			   &f->reader, &f->rect_tex, full_mask,
			   width, height, f->feather);
}

static struct obs_source_info pii_mask_filter_info = {
	.id = "pii_mask_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = pii_mask_get_name,
	.create = pii_mask_create,
	.destroy = pii_mask_destroy,
	.update = pii_mask_update,
	.get_properties = pii_mask_properties,
	.get_defaults = pii_mask_defaults,
	.video_tick = pii_mask_tick,
	.video_render = pii_mask_render,
};

bool obs_module_load(void)
{
	obs_register_source(&pii_mask_filter_info);
	return true;
}
