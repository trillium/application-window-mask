#include "shm_reader.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

bool pii_mask_reader_open(pii_mask_reader_t *reader)
{
	memset(reader, 0, sizeof(*reader));
	reader->fd = -1;

	int fd = shm_open(PII_MASK_SHM_NAME, O_RDONLY, 0);
	if (fd < 0)
		return false;

	void *ptr = mmap(NULL, sizeof(pii_mask_shm_t), PROT_READ,
			 MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED) {
		close(fd);
		return false;
	}

	pii_mask_shm_t *shm = (pii_mask_shm_t *)ptr;
	if (shm->magic != PII_MASK_MAGIC || shm->version != PII_MASK_VERSION) {
		munmap(ptr, sizeof(pii_mask_shm_t));
		close(fd);
		return false;
	}

	reader->fd = fd;
	reader->shm = shm;
	reader->connected = true;
	return true;
}

bool pii_mask_reader_update(pii_mask_reader_t *reader)
{
	if (!reader->connected)
		return false;

	pii_mask_shm_t *shm = reader->shm;

	/* Check staleness */
	reader->stale = pii_mask_is_stale(shm, PII_MASK_STALE_TIMEOUT_NS);

	/* Seqlock read */
	uint32_t seq;
	if (!pii_mask_read_begin(shm, &seq))
		return false; /* writer is mid-update, use cached data */

	/* Copy data */
	uint32_t count = shm->rect_count;
	if (count > PII_MASK_MAX_RECTS)
		count = PII_MASK_MAX_RECTS;

	memcpy(reader->rects, shm->rects, count * sizeof(pii_mask_rect_t));
	reader->rect_count = count;
	reader->flags = shm->flags;

	/* Validate read wasn't torn */
	if (!pii_mask_read_valid(shm, seq))
		return false; /* torn read, discard */

	return true;
}

void pii_mask_reader_close(pii_mask_reader_t *reader)
{
	if (reader->shm) {
		munmap(reader->shm, sizeof(pii_mask_shm_t));
		reader->shm = NULL;
	}
	if (reader->fd >= 0) {
		close(reader->fd);
		reader->fd = -1;
	}
	reader->connected = false;
}
