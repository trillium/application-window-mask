#ifndef SEQLOCK_H
#define SEQLOCK_H

#include <stdatomic.h>
#include <stdint.h>
#include <sys/mman.h>
#include <fcntl.h>

/// Begin a seqlock write — increments sequence to odd.
/// Takes void* so Swift can pass UnsafeMutableRawPointer without type mismatch.
static inline void seqlock_begin_write(void *seq) {
    atomic_fetch_add_explicit((_Atomic uint32_t *)seq, 1, memory_order_release);
}

/// End a seqlock write — increments sequence to even.
static inline void seqlock_end_write(void *seq) {
    atomic_fetch_add_explicit((_Atomic uint32_t *)seq, 1, memory_order_release);
}

/// Wrapper for shm_open (unavailable from Swift due to variadic signature).
static inline int shm_open_wrapper(const char *name, int oflag, mode_t mode) {
    return shm_open(name, oflag, mode);
}

#endif /* SEQLOCK_H */
