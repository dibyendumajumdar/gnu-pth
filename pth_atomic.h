/*
**  GNU Pth - The GNU Portable Threads
**
**  pth_atomic.h: atomic operations, spinlocks and TLS for the
**                multi-scheduler (MP) variant of Pth.
**
**  This is a private header (included via pth_p.h). It provides:
**
**    pth_spin_lock/trylock/unlock  on a plain `volatile int' word
**                                  (so public structs stay plain C)
**    pth_atomic_t / pth_atomic_{load,store,inc,dec,cas}
**    pth_atomic_ptr_{cas,xchg}     for lock-free pointer stacks
**    PTH_TLS                       thread-local storage class
**
**  When PTH_MP is not defined (classic single-scheduler build), all
**  operations degrade to their trivial non-atomic equivalents so that
**  common code can use them unconditionally at zero cost.
*/

#ifndef _PTH_ATOMIC_H_
#define _PTH_ATOMIC_H_

#if defined(PTH_MP)

/*
 * MP build: require real atomics. We use the GCC/Clang __atomic
 * builtins (the CMake build verifies availability; PTH_MP is
 * refused otherwise).
 */
#if !defined(__GNUC__) && !defined(__clang__)
#error "PTH_MP requires GCC/Clang __atomic builtins"
#endif

/* thread-local storage class */
#define PTH_TLS __thread

/* atomic integer */
typedef int pth_atomic_t;
#define PTH_ATOMIC_INIT(v)     (v)
#define pth_atomic_load(p)     __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define pth_atomic_store(p,v)  __atomic_store_n((p),(v), __ATOMIC_SEQ_CST)
#define pth_atomic_inc(p)      __atomic_add_fetch((p),1, __ATOMIC_SEQ_CST)
#define pth_atomic_dec(p)      __atomic_sub_fetch((p),1, __ATOMIC_SEQ_CST)
static inline int pth_atomic_cas(pth_atomic_t *p, int expected, int desired)
{
    return __atomic_compare_exchange_n(p, &expected, desired, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* atomic pointer operations (for the MPSC wakeup inbox) */
static inline int pth_atomic_ptr_cas(void * volatile *p, void *expected, void *desired)
{
    return __atomic_compare_exchange_n((void **)p, &expected, desired, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static inline void *pth_atomic_ptr_xchg(void * volatile *p, void *v)
{
    return __atomic_exchange_n((void **)p, v, __ATOMIC_SEQ_CST);
}
static inline void *pth_atomic_ptr_load(void * volatile *p)
{
    return __atomic_load_n((void **)p, __ATOMIC_ACQUIRE);
}

/*
 * spinlock on a plain `volatile int' word (test-and-test-and-set).
 * Hold times are O(1) list operations and a lightweight thread never
 * context-switches while holding one, so spinning is appropriate.
 */
#define PTH_SPIN_INIT 0
static inline void pth_spin_init(volatile int *l)
{
    *l = 0;
}
static inline int pth_spin_trylock(volatile int *l)
{
    return !__atomic_test_and_set((void *)l, __ATOMIC_ACQUIRE);
}
static inline void pth_spin_lock(volatile int *l)
{
    for (;;) {
        if (pth_spin_trylock(l))
            return;
        while (__atomic_load_n(l, __ATOMIC_RELAXED) != 0)
            /* spin on the (relaxed) read before retrying the RMW */ ;
    }
}
static inline void pth_spin_unlock(volatile int *l)
{
    __atomic_clear((void *)l, __ATOMIC_RELEASE);
}

#else /* !PTH_MP: classic single-scheduler build; everything is trivial */

#define PTH_TLS /* nothing */

typedef int pth_atomic_t;
#define PTH_ATOMIC_INIT(v)     (v)
#define pth_atomic_load(p)     (*(p))
#define pth_atomic_store(p,v)  (*(p) = (v))
#define pth_atomic_inc(p)      (++(*(p)))
#define pth_atomic_dec(p)      (--(*(p)))
static inline int pth_atomic_cas(pth_atomic_t *p, int expected, int desired)
{
    if (*p == expected) { *p = desired; return 1; }
    return 0;
}
static inline int pth_atomic_ptr_cas(void * volatile *p, void *expected, void *desired)
{
    if (*p == expected) { *p = desired; return 1; }
    return 0;
}
static inline void *pth_atomic_ptr_xchg(void * volatile *p, void *v)
{
    void *old = *(void **)p;
    *p = v;
    return old;
}
static inline void *pth_atomic_ptr_load(void * volatile *p)
{
    return *(void **)p;
}

#define PTH_SPIN_INIT 0
static inline void pth_spin_init(volatile int *l)    { *l = 0; }
static inline int  pth_spin_trylock(volatile int *l) { *l = 1; return 1; }
static inline void pth_spin_lock(volatile int *l)    { *l = 1; }
static inline void pth_spin_unlock(volatile int *l)  { *l = 0; }

#endif /* PTH_MP */

#endif /* _PTH_ATOMIC_H_ */
