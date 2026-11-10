#ifndef MRSW_LOCK_H
#define MRSW_LOCK_H
struct mrsw_lock { int count; };
static inline void mrsw_init(struct mrsw_lock *l) { l->count = 0; }
static inline void mrsw_read_acquire(struct mrsw_lock *l) { (void)l; }
static inline void mrsw_read_release(struct mrsw_lock *l) { (void)l; }
static inline void mrsw_write_acquire(struct mrsw_lock *l) { (void)l; }
static inline void mrsw_write_release(struct mrsw_lock *l) { (void)l; }
#endif
