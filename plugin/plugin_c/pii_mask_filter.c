#include <obs-module.h>
#include "shm_reader.h"
#include "mask_renderer.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("pii-mask-filter", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "PII Mask: real-time privacy masking via shared memory";
}

struct pii_mask_filter {
	obs_source_t *source;
	pii_mask_reader_t reader;
	int connect_retry_frames;
};

static const char *pii_mask_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "PII Mask";
}

static void *pii_mask_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	struct pii_mask_filter *f = bzalloc(sizeof(struct pii_mask_filter));
	f->source = source;
	f->connect_retry_frames = 0;

	pii_mask_reader_open(&f->reader);

	return f;
}

static void pii_mask_destroy(void *data)
{
	struct pii_mask_filter *f = data;
	pii_mask_reader_close(&f->reader);
	bfree(f);
}

static void pii_mask_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct pii_mask_filter *f = data;

	/* Try to connect if not yet connected */
	if (!f->reader.connected) {
		f->connect_retry_frames++;
		/* Retry every 30 frames (~1 second at 30fps) */
		if (f->connect_retry_frames >= 30) {
			pii_mask_reader_open(&f->reader);
			f->connect_retry_frames = 0;
		}
		return;
	}

	/* Read latest data from shared memory */
	pii_mask_reader_update(&f->reader);
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

	/*
	 * Determine if we should full-mask:
	 * - Not connected to shm
	 * - Data is stale (daemon crashed)
	 * - FLAG_FULL_MASK is set
	 */
	bool full_mask = !f->reader.connected ||
			 f->reader.stale ||
			 (f->reader.flags & PII_MASK_FLAG_FULL_MASK);

	pii_mask_draw_masks(f->source, &f->reader, full_mask, width, height);
}

static struct obs_source_info pii_mask_filter_info = {
	.id = "pii_mask_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = pii_mask_get_name,
	.create = pii_mask_create,
	.destroy = pii_mask_destroy,
	.video_tick = pii_mask_tick,
	.video_render = pii_mask_render,
};

bool obs_module_load(void)
{
	obs_register_source(&pii_mask_filter_info);
	return true;
}
