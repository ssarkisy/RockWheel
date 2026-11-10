/* One thread: the locks of the file system do nothing. */
#ifndef MUTEX_H
#define MUTEX_H
struct mutex { int recursion; };
static inline void mutex_init(struct mutex *m) { m->recursion = 0; }
static inline void mutex_lock(struct mutex *m) { m->recursion++; }
static inline void mutex_unlock(struct mutex *m) { m->recursion--; }
#endif
