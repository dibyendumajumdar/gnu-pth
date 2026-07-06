/*
**  GNU Pth - The GNU Portable Threads
**
**  pth_atomic.h: atomic operations, spinlocks and TLS for the
**                multi-scheduler (MP) variant of Pth.
**
**  This is a private header (included via pth_p.h). It provides:
**
**    pth_spin_t / PTH_SPIN_INIT / pth_spin_init/lock/trylock/unlock
**    pth_atomic_t / pth_atomic_{load,store,inc,dec,cas}
**    PTH_TLS  (thread-local storage class for per-OS-thread data)
**
**  When PTH_MP is not defined (classic single-scheduler build), all
**  operations degrade to their trivial non-atomic equivalents so that
**  common code can use them unconditionally at zero cost.
*/

#ifndef _PTH_ATOMIC_H_
#define _PTH_ATOMIC_H_

#if defined(PTH_MP)

/*
 * MP build: require real atomics. We support C11 <stdatomic.h> and the
 * GCC/Clang __atomic builtins. The CMake build verifies availability
 * (PTH_MP is refused otherwise).
 */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#define PTH_ATOMIC_C11 1
#include <stdatomic.h>
#elif defined(__GNUC__) || defined(__clang__)
#define PTH_ATOMIC_GNU 1
#else
#error "PTH_MP requires C11 atomics or GCC/Clang __atomic builtins"
#endif

/* thread-local storage class */
#if defined(PTH_ATOMIC_C11) && !defined(__STDC_NO_THREADS__)
#define PTH_TLS _Thread_local
#else
#define PTH_TLS __thread
#endif

/* atomic integer */
#if defined(PTH_ATOMIC_C11)
typedef atomic_int pth_atomic_t;
#define PTH_ATOMIC_INIT(v)          (v)
#define pth_atomic_load(p)          atomic_load(p)
#define pth_atomic_store(p,v)       atomic_store((p),(v))
#define pth_atomic_inc(p)           (atomic_fetch_add((p),1)+1)
#define pth_atomic_dec(p)           (atomic_fetch_sub((p),1)-1)
static inline int pth_atomic_cas(pth_atomic_t *p, int expected, int desired)
{
    return atomic_compare_exchange_strong(p, &expected, desired);
}
#else /* PTH_ATOMIC_GNU */
typedef int pth_atomic_t;
#define PTH_ATOMIC_INIT(v)          (v)
#define pth_atomic_load(p)          __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define pth_atomic_store(p,v)       __atomic_store_n((p),(v), __ATOMIC_SEQ_CST)
#define pth_atomic_inc(p)           __atomic_add_fetch((p),1, __ATOMIC_SEQ_CST)
#define pth_atomic_dec(p)           __atomic_sub_fetch((p),1, __ATOMIC_SEQ_CST)
static inline int pth_atomic_cas(pth_atomic_t *p, int expected, int desired)
{
    return __atomic_compare_exchange_n(p, &expected, desired, 0,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
#endif

/* spinlock (test-and-test-and-set; hold times are O(1) list operations,
   and a lightweight thread never context-switches while holding one) */
typedef struct { volatile int l; } pth_spin_t;
#define PTH_SPIN_INIT { 0 }

static inline void pth_spin_init(pth_spin_t *s)
{
    s->l = 0;
}
static inline int pth_spin_trylock(pth_spin_t *s)
{
#if defined(PTH_ATOMIC_C11)
    int expected = 0;
    return atomic_compare_exchange_strong((atomic_int *)&s->l, &expected, 1);
#else
    return !__atomic_test_and_set((void *)&s->l, __ATOMIC_ACQUIRE);
#endif
}
static inline void pth_spin_lock(pth_spin_t *s)
{
    for (;;) {
        if (pth_spin_trylock(s))
            return;
        while (s->l != 0)
            /* spin on the (cached) read before retrying the RMW */ ;
    }
}
static inline void pth_spin_unlock(pth_spin_t *s)
{
#if defined(PTH_ATOMIC_C11)
    atomic_store_explicit((atomic_int *)&s->l, 0, memory_order_release);
#else
    __atomic_clear((void *)&s->l, __ATOMIC_RELEASE);
#endif
}

#else /* !PTH_MP: classic single-scheduler build; everything is trivial */

#define PTH_TLS /* nothing */

typedef int pth_atomic_t;
#define PTH_ATOMIC_INIT(v)          (v)
#define pth_atomic_load(p)          (*(p))
#define pth_atomic_store(p,v)       (*(p) = (v))
#define pth_atomic_inc(p)           (++(*(p)))
#define pth_atomic_dec(p)           (--(*(p)))
static inline int pth_atomic_cas(pth_atomic_t *p, int expected, int desired)
{
    if (*p == expected) { *p = desired; return 1; }
    return 0;
}

typedef struct { int l; } pth_spin_t;
#define PTH_SPIN_INIT { 0 }
static inline void pth_spin_init(pth_spin_t *s)    { s->l = 0; }
static inline int  pth_spin_trylock(pth_spin_t *s) { s->l = 1; return 1; }
static inline void pth_spin_lock(pth_spin_t *s)    { s->l = 1; }
static inline void pth_spin_unlock(pth_spin_t *s)  { s->l = 0; }

#endif /* PTH_MP */

#endif /* _PTH_ATOMIC_H_ */
