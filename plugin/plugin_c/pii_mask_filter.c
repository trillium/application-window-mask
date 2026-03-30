#include <obs-module.h>
#include <util/platform.h>
#include "shm_reader.h"
#include "mask_renderer.h"
#include "rect_texture.h"
#include "blur_kawase.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("pii-mask-filter", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "PII Mask: real-time privacy masking via shared memory";
}

enum obfuscation_method {
	OBF_SOLID = 0,
	OBF_BLUR = 1,
	OBF_PIXELATE = 2,
};

/* Benchmark: log interval in frames */
#define BENCH_INTERVAL 300

struct pii_bench {
	uint64_t capture_ns;
	uint64_t obfuscate_ns;
	uint64_t rect_upload_ns;
	uint64_t composite_ns;
	uint64_t total_ns;
};

struct pii_mask_filter {
	obs_source_t *source;
	pii_mask_reader_t reader;
	gs_texrender_t *texrender_clear;
	gs_texrender_t *texrender_obfuscated;
	pii_rect_texture_t rect_tex;
	gs_effect_t *effect_composite;
	gs_effect_t *effect_pixelate;
	pii_blur_kawase_t blur;
	int connect_retry_frames;
	bool force_full_mask;
	float feather;
	enum obfuscation_method method;
	int blur_levels;
	float pixel_block_size;
	uint32_t cx;
	uint32_t cy;

	/* Benchmark state */
	struct pii_bench bench_accum;
	struct pii_bench bench_max;
	int bench_frame;
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
	f->method = (enum obfuscation_method)obs_data_get_int(settings,
							      "method");
	f->blur_levels = (int)obs_data_get_int(settings, "blur_levels");
	f->pixel_block_size = (float)obs_data_get_double(settings,
							 "pixel_block_size");
}

static void *pii_mask_create(obs_data_t *settings, obs_source_t *source)
{
	struct pii_mask_filter *f = bzalloc(sizeof(struct pii_mask_filter));
	f->source = source;
	f->connect_retry_frames = 0;

	/* Load effects */
	char *path;

	path = obs_module_file("pii_composite.effect");
	blog(LOG_INFO, "[pii-mask] composite effect path: %s",
	     path ? path : "(null)");
	if (path) {
		obs_enter_graphics();
		f->effect_composite =
			gs_effect_create_from_file(path, NULL);
		obs_leave_graphics();
		blog(LOG_INFO, "[pii-mask] composite effect loaded: %s",
		     f->effect_composite ? "yes" : "NO");
		bfree(path);
	}

	path = obs_module_file("pixelate.effect");
	blog(LOG_INFO, "[pii-mask] pixelate effect path: %s",
	     path ? path : "(null)");
	if (path) {
		obs_enter_graphics();
		f->effect_pixelate =
			gs_effect_create_from_file(path, NULL);
		obs_leave_graphics();
		blog(LOG_INFO, "[pii-mask] pixelate effect loaded: %s",
		     f->effect_pixelate ? "yes" : "NO");
		bfree(path);
	}

	obs_enter_graphics();
	f->texrender_clear = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	f->texrender_obfuscated = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	pii_rect_texture_create(&f->rect_tex);
	bool blur_ok = pii_blur_create(&f->blur, 5);
	obs_leave_graphics();
	blog(LOG_INFO, "[pii-mask] blur created: %s (levels=%d)",
	     blur_ok ? "yes" : "NO", f->blur.levels);

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
	pii_blur_destroy(&f->blur);
	if (f->effect_composite)
		gs_effect_destroy(f->effect_composite);
	if (f->effect_pixelate)
		gs_effect_destroy(f->effect_pixelate);
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

	obs_property_t *method = obs_properties_add_list(props, "method",
		"Obfuscation method", OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(method, "Solid black", OBF_SOLID);
	obs_property_list_add_int(method, "Blur (Dual Kawase)", OBF_BLUR);
	obs_property_list_add_int(method, "Pixelate", OBF_PIXELATE);

	obs_properties_add_int_slider(props, "blur_levels",
				      "Blur strength (levels)", 1, 6, 1);
	obs_properties_add_float_slider(props, "pixel_block_size",
					"Pixel block size", 2.0, 64.0, 1.0);
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
	obs_data_set_default_int(settings, "method", OBF_BLUR);
	obs_data_set_default_int(settings, "blur_levels", 4);
	obs_data_set_default_double(settings, "pixel_block_size", 16.0);
}

static void pii_mask_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct pii_mask_filter *f = data;

	if (!f->reader.connected) {
		f->connect_retry_frames++;
		if (f->connect_retry_frames >= 30) {
			pii_mask_reader_open(&f->reader);
			f->connect_retry_frames = 0;
		}
		return;
	}

	pii_mask_reader_update(&f->reader);

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

/* Render obfuscated frame based on selected method */
static gs_texture_t *render_obfuscated(struct pii_mask_filter *f,
				       gs_texture_t *clear_tex,
				       uint32_t width, uint32_t height)
{
	switch (f->method) {
	case OBF_BLUR: {
		/* Use only the configured number of levels */
		int levels = f->blur_levels;
		if (levels < 1) levels = 1;
		if (levels > f->blur.levels) levels = f->blur.levels;

		/* Temporarily adjust levels for this render */
		int saved = f->blur.levels;
		f->blur.levels = levels;
		gs_texture_t *blurred =
			pii_blur_render(&f->blur, clear_tex, width, height);
		f->blur.levels = saved;
		return blurred;
	}

	case OBF_PIXELATE: {
		if (!f->effect_pixelate)
			break;

		gs_texrender_reset(f->texrender_obfuscated);
		if (!gs_texrender_begin(f->texrender_obfuscated,
					width, height))
			break;

		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height,
			 -100.0f, 100.0f);

		gs_eparam_t *p;
		p = gs_effect_get_param_by_name(f->effect_pixelate, "image");
		gs_effect_set_texture(p, clear_tex);

		struct vec2 res;
		res.x = (float)width;
		res.y = (float)height;
		p = gs_effect_get_param_by_name(f->effect_pixelate,
						"resolution");
		gs_effect_set_vec2(p, &res);

		p = gs_effect_get_param_by_name(f->effect_pixelate,
						"block_size");
		gs_effect_set_float(p, f->pixel_block_size);

		while (gs_effect_loop(f->effect_pixelate, "Draw"))
			gs_draw_sprite(clear_tex, 0, width, height);

		gs_texrender_end(f->texrender_obfuscated);
		return gs_texrender_get_texture(f->texrender_obfuscated);
	}

	case OBF_SOLID:
	default:
		break;
	}

	/* Solid black fallback */
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

static void bench_log(struct pii_mask_filter *f)
{
	int n = BENCH_INTERVAL;
	blog(LOG_INFO,
	     "[pii-mask] BENCH avg(us): capture=%llu obf=%llu "
	     "rect_upload=%llu composite=%llu total=%llu | "
	     "max(us): capture=%llu obf=%llu composite=%llu total=%llu | "
	     "rects=%u method=%d",
	     (unsigned long long)(f->bench_accum.capture_ns / n / 1000),
	     (unsigned long long)(f->bench_accum.obfuscate_ns / n / 1000),
	     (unsigned long long)(f->bench_accum.rect_upload_ns / n / 1000),
	     (unsigned long long)(f->bench_accum.composite_ns / n / 1000),
	     (unsigned long long)(f->bench_accum.total_ns / n / 1000),
	     (unsigned long long)(f->bench_max.capture_ns / 1000),
	     (unsigned long long)(f->bench_max.obfuscate_ns / 1000),
	     (unsigned long long)(f->bench_max.composite_ns / 1000),
	     (unsigned long long)(f->bench_max.total_ns / 1000),
	     f->reader.rect_count,
	     (int)f->method);
	memset(&f->bench_accum, 0, sizeof(f->bench_accum));
	memset(&f->bench_max, 0, sizeof(f->bench_max));
	f->bench_frame = 0;
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

	uint64_t t0, t1;
	uint64_t t_total_start = os_gettime_ns();

	/* Pass 1: Capture clear frame */
	t0 = os_gettime_ns();
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
	t1 = os_gettime_ns();
	uint64_t t_capture = t1 - t0;

	gs_texture_t *clear_tex =
		gs_texrender_get_texture(f->texrender_clear);
	if (!clear_tex)
		return;

	/* Pass 2: Render obfuscated frame */
	t0 = os_gettime_ns();
	gs_texture_t *obf_tex =
		render_obfuscated(f, clear_tex, width, height);
	t1 = os_gettime_ns();
	uint64_t t_obfuscate = t1 - t0;

	/* Update rect data texture */
	t0 = os_gettime_ns();
	float sx = (f->reader.screen_width > 0)
		? (float)width / f->reader.screen_width : 1.0f;
	float sy = (f->reader.screen_height > 0)
		? (float)height / f->reader.screen_height : 1.0f;
	pii_rect_texture_update(&f->rect_tex, &f->reader, sx, sy);
	t1 = os_gettime_ns();
	uint64_t t_rect_upload = t1 - t0;

	/* Pass 3: SDF composite */
	t0 = os_gettime_ns();
	pii_mask_composite(clear_tex, obf_tex, f->effect_composite,
			   &f->reader, &f->rect_tex, full_mask,
			   width, height, f->feather);
	t1 = os_gettime_ns();
	uint64_t t_composite = t1 - t0;

	uint64_t t_total = os_gettime_ns() - t_total_start;

	/* Accumulate benchmarks */
	f->bench_accum.capture_ns += t_capture;
	f->bench_accum.obfuscate_ns += t_obfuscate;
	f->bench_accum.rect_upload_ns += t_rect_upload;
	f->bench_accum.composite_ns += t_composite;
	f->bench_accum.total_ns += t_total;

	if (t_capture > f->bench_max.capture_ns)
		f->bench_max.capture_ns = t_capture;
	if (t_obfuscate > f->bench_max.obfuscate_ns)
		f->bench_max.obfuscate_ns = t_obfuscate;
	if (t_composite > f->bench_max.composite_ns)
		f->bench_max.composite_ns = t_composite;
	if (t_total > f->bench_max.total_ns)
		f->bench_max.total_ns = t_total;

	f->bench_frame++;
	if (f->bench_frame >= BENCH_INTERVAL)
		bench_log(f);
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
