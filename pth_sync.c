/*
**  GNU Pth - The GNU Portable Threads
**  Copyright (c) 1999-2006 Ralf S. Engelschall <rse@engelschall.com>
**
**  This file is part of GNU Pth, a non-preemptive thread scheduling
**  library which can be found at http://www.gnu.org/software/pth/.
**
**  This library is free software; you can redistribute it and/or
**  modify it under the terms of the GNU Lesser General Public
**  License as published by the Free Software Foundation; either
**  version 2.1 of the License, or (at your option) any later version.
**
**  This library is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
**  Lesser General Public License for more details.
**
**  You should have received a copy of the GNU Lesser General Public
**  License along with this library; if not, write to the Free Software
**  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
**  USA, or contact Ralf S. Engelschall <rse@engelschall.com>.
**
**  pth_sync.c: Pth synchronization facilities
**
**  The primitives use explicit per-primitive wait queues protected by
**  a small spinlock, instead of the classic scheme where the scheduler
**  polled the primitive state on every loop. This makes them work
**  across multiple schedulers (see MULTISCHED-DESIGN.md): a releasing
**  or notifying thread dequeues a waiter and delivers a directed
**  wakeup through the waiter's home scheduler inbox (pth_gsched_wake).
*/
                             /* ``It is hard to fly with
                                  the eagles when you work
                                  with the turkeys.''
                                          -- Unknown  */
#include "pth_p.h"

/*
**  Wait Queues (FIFO of blocked threads, guarded by the primitive's lock)
*/

intern void pth_waitq_init(pth_waitq_t *q)
{
    q->wq_head = NULL;
    q->wq_tail = NULL;
    return;
}

intern void pth_waitq_append(pth_waitq_t *q, pth_t t)
{
    t->wq_next = NULL;
    if (q->wq_tail != NULL)
        q->wq_tail->wq_next = t;
    else
        q->wq_head = t;
    q->wq_tail = t;
    return;
}

intern pth_t pth_waitq_shift(pth_waitq_t *q)
{
    pth_t t;

    t = q->wq_head;
    if (t != NULL) {
        q->wq_head = t->wq_next;
        if (q->wq_head == NULL)
            q->wq_tail = NULL;
        t->wq_next = NULL;
    }
    return t;
}

intern int pth_waitq_remove(pth_waitq_t *q, pth_t t)
{
    pth_t c, p;

    p = NULL;
    for (c = q->wq_head; c != NULL; p = c, c = c->wq_next) {
        if (c == t) {
            if (p != NULL)
                p->wq_next = c->wq_next;
            else
                q->wq_head = c->wq_next;
            if (q->wq_tail == c)
                q->wq_tail = p;
            c->wq_next = NULL;
            return TRUE;
        }
    }
    return FALSE;
}

/*
**  Mutual Exclusion Locks
*/

int pth_mutex_init(pth_mutex_t *mutex)
{
    if (mutex == NULL)
        return pth_error(FALSE, EINVAL);
    pth_atomic_store(&mutex->mx_state, PTH_MUTEX_INITIALIZED);
    mutex->mx_owner = NULL;
    mutex->mx_count = 0;
    pth_spin_init(&mutex->mx_lock);
    pth_waitq_init(&mutex->mx_waitq);
    return TRUE;
}

/* select the fairness policy of a mutex: with fairness enabled the mutex
   uses direct ownership handoff on release (the first queued waiter becomes
   the owner while the lock stays held), giving FIFO ordering and preventing
   the starvation possible under the default barging policy. Intended to be
   called right after pth_mutex_init(3), before the mutex is contended. */
int pth_mutex_setfair(pth_mutex_t *mutex, int fair)
{
    if (mutex == NULL)
        return pth_error(FALSE, EINVAL);
    if (!(pth_atomic_load(&mutex->mx_state) & PTH_MUTEX_INITIALIZED))
        return pth_error(FALSE, EDEADLK);
    pth_spin_lock(&mutex->mx_lock);
    if (fair)
        pth_atomic_store(&mutex->mx_state,
                         pth_atomic_load(&mutex->mx_state) | PTH_MUTEX_FAIR);
    else
        pth_atomic_store(&mutex->mx_state,
                         pth_atomic_load(&mutex->mx_state) & ~PTH_MUTEX_FAIR);
    pth_spin_unlock(&mutex->mx_lock);
    return TRUE;
}

/* dequeue us from the wait queue if we get cancelled inside pth_wait(3),
   because otherwise a later releaser would touch our freed thread control
   block */
static void pth_mutex_wait_cleanup(void *arg)
{
    pth_mutex_t *mutex = (pth_mutex_t *)arg;
    pth_t w;

    pth_spin_lock(&mutex->mx_lock);
    if (mutex->mx_owner == pth_current) {
        /* fair mode: we had been handed ownership (mutex still LOCKED) but
           are being cancelled before we could take it. Pass the lock on to
           the next waiter, or fully release it if the queue is now empty,
           so the mutex is never left stuck owned by a dead thread. */
        w = pth_waitq_shift(&mutex->mx_waitq);
        if (w != NULL) {
            mutex->mx_owner = w;
            mutex->mx_count = 1;
            pth_spin_unlock(&mutex->mx_lock);
            pth_gsched_wake(w->sched_home, w, w->wq_event);
            return;
        }
        pth_atomic_store(&mutex->mx_state,
                         pth_atomic_load(&mutex->mx_state) & ~PTH_MUTEX_LOCKED);
        mutex->mx_owner = NULL;
        mutex->mx_count = 0;
    }
    else
        pth_waitq_remove(&mutex->mx_waitq, pth_current);
    pth_spin_unlock(&mutex->mx_lock);
    return;
}

int pth_mutex_acquire(pth_mutex_t *mutex, int tryonly, pth_event_t ev_extra)
{
    static pth_key_t ev_key = PTH_KEY_INIT;
    pth_event_t ev;

    pth_debug2("pth_mutex_acquire: called from thread \"%s\"", pth_current->name);

    /* consistency checks */
    if (mutex == NULL)
        return pth_error(FALSE, EINVAL);
    if (!(pth_atomic_load(&mutex->mx_state) & PTH_MUTEX_INITIALIZED))
        return pth_error(FALSE, EDEADLK);

    pth_spin_lock(&mutex->mx_lock);

    /* still not locked, so simply acquire mutex? */
    if (!(pth_atomic_load(&mutex->mx_state) & PTH_MUTEX_LOCKED)) {
        pth_atomic_store(&mutex->mx_state,
                         pth_atomic_load(&mutex->mx_state) | PTH_MUTEX_LOCKED);
        mutex->mx_owner = pth_current;
        mutex->mx_count = 1;
        pth_ring_append(&(pth_current->mutexring), &(mutex->mx_node));
        pth_spin_unlock(&mutex->mx_lock);
        pth_debug1("pth_mutex_acquire: immediately locking mutex");
        return TRUE;
    }

    /* already locked by caller? */
    if (mutex->mx_count >= 1 && mutex->mx_owner == pth_current) {
        /* recursive lock */
        mutex->mx_count++;
        pth_spin_unlock(&mutex->mx_lock);
        pth_debug1("pth_mutex_acquire: recursive locking");
        return TRUE;
    }

    /* should we just tryonly? */
    if (tryonly) {
        pth_spin_unlock(&mutex->mx_lock);
        return pth_error(FALSE, EBUSY);
    }

    /* else wait for mutex to become unlocked.. */
    pth_debug1("pth_mutex_acquire: wait until mutex is unlocked");
    for (;;) {
        /* park on the mutex wait queue (the spinlock is still held, so
           no release can slip through between check and enqueue) */
        ev = pth_event(PTH_EVENT_MUTEX|PTH_MODE_STATIC, &ev_key, mutex);
        if (ev_extra != NULL)
            pth_event_concat(ev, ev_extra, NULL);
        pth_current->wq_event = ev;
        pth_waitq_append(&mutex->mx_waitq, pth_current);
        pth_spin_unlock(&mutex->mx_lock);

        pth_cleanup_push(pth_mutex_wait_cleanup, mutex);
        pth_wait(ev);
        pth_cleanup_pop(FALSE);

        pth_spin_lock(&mutex->mx_lock);
        /* fair mode: the releaser may have handed ownership directly to us
           (mutex kept LOCKED, mx_owner set to us, and we were already
           dequeued). Recognise that and take the lock without re-contending. */
        if (mutex->mx_owner == pth_current) {
            pth_ring_append(&(pth_current->mutexring), &(mutex->mx_node));
            pth_spin_unlock(&mutex->mx_lock);
            if (ev_extra != NULL)
                pth_event_isolate(ev);
            pth_debug1("pth_mutex_acquire: acquired mutex via fair handoff");
            return TRUE;
        }
        /* no-op if the releaser already dequeued us */
        pth_waitq_remove(&mutex->mx_waitq, pth_current);
        if (ev_extra != NULL) {
            pth_event_isolate(ev);
            if (pth_event_status(ev) == PTH_STATUS_PENDING) {
                pth_spin_unlock(&mutex->mx_lock);
                return pth_error(FALSE, EINTR);
            }
        }
        if (!(pth_atomic_load(&mutex->mx_state) & PTH_MUTEX_LOCKED))
            break;
        /* someone else barged in between wakeup and here: park again */
    }

    /* now it's again unlocked, so acquire mutex (spinlock still held) */
    pth_debug1("pth_mutex_acquire: locking mutex");
    pth_atomic_store(&mutex->mx_state,
                     pth_atomic_load(&mutex->mx_state) | PTH_MUTEX_LOCKED);
    mutex->mx_owner = pth_current;
    mutex->mx_count = 1;
    pth_ring_append(&(pth_current->mutexring), &(mutex->mx_node));
    pth_spin_unlock(&mutex->mx_lock);
    return TRUE;
}

int pth_mutex_release(pth_mutex_t *mutex)
{
    pth_t w;

    /* consistency checks */
    if (mutex == NULL)
        return pth_error(FALSE, EINVAL);
    if (!(pth_atomic_load(&mutex->mx_state) & PTH_MUTEX_INITIALIZED))
        return pth_error(FALSE, EDEADLK);
    if (!(pth_atomic_load(&mutex->mx_state) & PTH_MUTEX_LOCKED))
        return pth_error(FALSE, EDEADLK);
    if (mutex->mx_owner != pth_current)
        return pth_error(FALSE, EACCES);

    /* decrement recursion counter and release mutex */
    pth_spin_lock(&mutex->mx_lock);
    mutex->mx_count--;
    if (mutex->mx_count <= 0) {
        w = pth_waitq_shift(&mutex->mx_waitq);
        if (w != NULL && (pth_atomic_load(&mutex->mx_state) & PTH_MUTEX_FAIR)) {
            /* fair mode: hand ownership directly to the first waiter so no
               newly arriving thread can barge ahead of the queue. The mutex
               stays LOCKED throughout; the woken waiter finds itself already
               the owner and appends the mutex to its own mutexring (we must
               not touch a foreign thread's ring). */
            mutex->mx_owner = w;
            mutex->mx_count = 1;
            pth_ring_delete(&(pth_current->mutexring), &(mutex->mx_node));
            pth_spin_unlock(&mutex->mx_lock);
            pth_gsched_wake(w->sched_home, w, w->wq_event);
        }
        else {
            /* barging mode (default): drop the lock and wake one waiter,
               which re-contends for the mutex on resume */
            pth_atomic_store(&mutex->mx_state,
                             pth_atomic_load(&mutex->mx_state) & ~PTH_MUTEX_LOCKED);
            mutex->mx_owner = NULL;
            mutex->mx_count = 0;
            pth_ring_delete(&(pth_current->mutexring), &(mutex->mx_node));
            pth_spin_unlock(&mutex->mx_lock);
            if (w != NULL)
                pth_gsched_wake(w->sched_home, w, w->wq_event);
        }
    }
    else
        pth_spin_unlock(&mutex->mx_lock);
    return TRUE;
}

intern void pth_mutex_releaseall(pth_t thread)
{
    pth_ringnode_t *rn, *rnf;

    if (thread == NULL)
        return;
    /* iterate over all mutexes of thread */
    rn = rnf = pth_ring_first(&(thread->mutexring));
    while (rn != NULL) {
        pth_mutex_release((pth_mutex_t *)rn);
        rn = pth_ring_next(&(thread->mutexring), rn);
        if (rn == rnf)
            break;
    }
    return;
}

/*
**  Read-Write Locks
*/

/*
 * NOTE: the classic Pth read-write lock was composed from two mutexes,
 * with the first arriving reader acquiring the writer-excluding mutex
 * and the last leaving reader releasing it. That only works while
 * readers never truly overlap on release (the mutex owner check fails
 * otherwise), which multiple schedulers immediately violate. It is now
 * implemented natively: a spinlock-guarded state word plus separate
 * reader/writer wait queues with directed wakeups (writer preference).
 */

int pth_rwlock_init(pth_rwlock_t *rwlock)
{
    if (rwlock == NULL)
        return pth_error(FALSE, EINVAL);
    pth_atomic_store(&rwlock->rw_state, PTH_RWLOCK_INITIALIZED);
    rwlock->rw_mode    = PTH_RWLOCK_RD;
    rwlock->rw_readers = 0;
    rwlock->rw_owner   = NULL;
    pth_spin_init(&rwlock->rw_lock);
    pth_waitq_init(&rwlock->rw_waitq_rd);
    pth_waitq_init(&rwlock->rw_waitq_rw);
    rwlock->rw_wrwait  = 0;
    return TRUE;
}

/* dequeue us from the wait queues if we get cancelled inside pth_wait(3) */
static void pth_rwlock_wait_cleanup(void *arg)
{
    pth_rwlock_t *rwlock = (pth_rwlock_t *)arg;

    pth_spin_lock(&rwlock->rw_lock);
    if (pth_waitq_remove(&rwlock->rw_waitq_rw, pth_current))
        rwlock->rw_wrwait--;
    pth_waitq_remove(&rwlock->rw_waitq_rd, pth_current);
    pth_spin_unlock(&rwlock->rw_lock);
    return;
}

int pth_rwlock_acquire(pth_rwlock_t *rwlock, int op, int tryonly, pth_event_t ev_extra)
{
    static pth_key_t ev_key_rd = PTH_KEY_INIT;
    static pth_key_t ev_key_rw = PTH_KEY_INIT;
    pth_event_t ev;

    /* consistency checks */
    if (rwlock == NULL)
        return pth_error(FALSE, EINVAL);
    if (!(pth_atomic_load(&rwlock->rw_state) & PTH_RWLOCK_INITIALIZED))
        return pth_error(FALSE, EDEADLK);

    pth_spin_lock(&rwlock->rw_lock);
    for (;;) {
        if (op == PTH_RWLOCK_RW) {
            /* writers need the lock exclusively */
            if (rwlock->rw_owner == NULL && rwlock->rw_readers == 0) {
                rwlock->rw_owner = pth_current;
                rwlock->rw_mode  = PTH_RWLOCK_RW;
                pth_spin_unlock(&rwlock->rw_lock);
                return TRUE;
            }
        }
        else {
            /* readers pass unless a writer is active or waiting
               (writer preference avoids writer starvation) */
            if (rwlock->rw_owner == NULL && rwlock->rw_wrwait == 0) {
                rwlock->rw_readers++;
                rwlock->rw_mode = PTH_RWLOCK_RD;
                pth_spin_unlock(&rwlock->rw_lock);
                return TRUE;
            }
        }

        /* should we just tryonly? */
        if (tryonly) {
            pth_spin_unlock(&rwlock->rw_lock);
            return pth_error(FALSE, EBUSY);
        }

        /* park on the respective wait queue (spinlock still held) */
        if (op == PTH_RWLOCK_RW) {
            ev = pth_event(PTH_EVENT_NOTIFY|PTH_MODE_STATIC, &ev_key_rw, (void *)rwlock);
            if (ev_extra != NULL)
                pth_event_concat(ev, ev_extra, NULL);
            pth_current->wq_event = ev;
            pth_waitq_append(&rwlock->rw_waitq_rw, pth_current);
            rwlock->rw_wrwait++;
        }
        else {
            ev = pth_event(PTH_EVENT_NOTIFY|PTH_MODE_STATIC, &ev_key_rd, (void *)rwlock);
            if (ev_extra != NULL)
                pth_event_concat(ev, ev_extra, NULL);
            pth_current->wq_event = ev;
            pth_waitq_append(&rwlock->rw_waitq_rd, pth_current);
        }
        pth_spin_unlock(&rwlock->rw_lock);

        pth_cleanup_push(pth_rwlock_wait_cleanup, rwlock);
        pth_wait(ev);
        pth_cleanup_pop(FALSE);

        pth_spin_lock(&rwlock->rw_lock);
        /* no-ops if a releaser already dequeued us */
        if (op == PTH_RWLOCK_RW) {
            if (pth_waitq_remove(&rwlock->rw_waitq_rw, pth_current))
                rwlock->rw_wrwait--;
        }
        else
            pth_waitq_remove(&rwlock->rw_waitq_rd, pth_current);
        if (ev_extra != NULL) {
            pth_event_isolate(ev);
            if (pth_event_status(ev) == PTH_STATUS_PENDING) {
                pth_spin_unlock(&rwlock->rw_lock);
                return pth_error(FALSE, EINTR);
            }
        }
        /* loop and re-check (someone may have barged in) */
    }
}

int pth_rwlock_release(pth_rwlock_t *rwlock)
{
    pth_t w;
    pth_t chain;

    /* consistency checks */
    if (rwlock == NULL)
        return pth_error(FALSE, EINVAL);
    if (!(pth_atomic_load(&rwlock->rw_state) & PTH_RWLOCK_INITIALIZED))
        return pth_error(FALSE, EDEADLK);

    pth_spin_lock(&rwlock->rw_lock);
    if (rwlock->rw_owner != NULL) {
        /* write lock held: only the owner may release it */
        if (rwlock->rw_owner != pth_current) {
            pth_spin_unlock(&rwlock->rw_lock);
            return pth_error(FALSE, EACCES);
        }
        rwlock->rw_owner = NULL;
    }
    else {
        /* read lock held */
        if (rwlock->rw_readers == 0) {
            pth_spin_unlock(&rwlock->rw_lock);
            return pth_error(FALSE, EPERM);
        }
        rwlock->rw_readers--;
        if (rwlock->rw_readers > 0) {
            pth_spin_unlock(&rwlock->rw_lock);
            return TRUE;
        }
    }

    /* the lock became free: wake a parked writer if any,
       else release the whole herd of parked readers */
    w = pth_waitq_shift(&rwlock->rw_waitq_rw);
    if (w != NULL) {
        rwlock->rw_wrwait--;
        pth_spin_unlock(&rwlock->rw_lock);
        pth_gsched_wake(w->sched_home, w, w->wq_event);
    }
    else {
        chain = NULL;
        while ((w = pth_waitq_shift(&rwlock->rw_waitq_rd)) != NULL) {
            w->wq_next = chain;
            chain = w;
        }
        pth_spin_unlock(&rwlock->rw_lock);
        while (chain != NULL) {
            w = chain;
            chain = w->wq_next;
            w->wq_next = NULL;
            pth_gsched_wake(w->sched_home, w, w->wq_event);
        }
    }
    return TRUE;
}

/*
**  Condition Variables
*/

int pth_cond_init(pth_cond_t *cond)
{
    if (cond == NULL)
        return pth_error(FALSE, EINVAL);
    pth_atomic_store(&cond->cn_state, PTH_COND_INITIALIZED);
    cond->cn_waiters = 0;
    pth_spin_init(&cond->cn_lock);
    pth_waitq_init(&cond->cn_waitq);
    return TRUE;
}

static void pth_cond_cleanup_handler(void *_cleanvec)
{
    pth_mutex_t *mutex = (pth_mutex_t *)(((void **)_cleanvec)[0]);
    pth_cond_t  *cond  = (pth_cond_t  *)(((void **)_cleanvec)[1]);

    /* re-acquire mutex when pth_cond_await() is cancelled
       in order to restore the condition variable semantics */
    pth_mutex_acquire(mutex, FALSE, NULL);

    /* unpark us if we are still enqueued (otherwise a later notifier
       would touch our freed thread control block) */
    pth_spin_lock(&cond->cn_lock);
    if (pth_waitq_remove(&cond->cn_waitq, pth_current))
        cond->cn_waiters--;
    pth_spin_unlock(&cond->cn_lock);
    return;
}

int pth_cond_await(pth_cond_t *cond, pth_mutex_t *mutex, pth_event_t ev_extra)
{
    static pth_key_t ev_key = PTH_KEY_INIT;
    void *cleanvec[2];
    pth_event_t ev;

    /* consistency checks */
    if (cond == NULL || mutex == NULL)
        return pth_error(FALSE, EINVAL);
    if (!(pth_atomic_load(&cond->cn_state) & PTH_COND_INITIALIZED))
        return pth_error(FALSE, EDEADLK);

    /* build the event(s) to wait for */
    ev = pth_event(PTH_EVENT_COND|PTH_MODE_STATIC, &ev_key, cond);
    if (ev_extra != NULL)
        pth_event_concat(ev, ev_extra, NULL);

    /* park on the wait queue BEFORE releasing the mutex: this way no
       notification posted after our release can ever be missed */
    pth_spin_lock(&cond->cn_lock);
    pth_current->wq_event = ev;
    pth_waitq_append(&cond->cn_waitq, pth_current);
    cond->cn_waiters++;
    pth_spin_unlock(&cond->cn_lock);

    /* release mutex (caller had to acquire it first) */
    pth_mutex_release(mutex);

    /* wait until the condition is signaled */
    cleanvec[0] = mutex;
    cleanvec[1] = cond;
    pth_cleanup_push(pth_cond_cleanup_handler, cleanvec);
    pth_wait(ev);
    pth_cleanup_pop(FALSE);
    if (ev_extra != NULL)
        pth_event_isolate(ev);

    /* if we were woken through the extra event(s) only, unpark ourselves
       (no-op if a notifier already dequeued us) */
    pth_spin_lock(&cond->cn_lock);
    if (pth_waitq_remove(&cond->cn_waitq, pth_current))
        cond->cn_waiters--;
    pth_spin_unlock(&cond->cn_lock);

    /* reacquire mutex */
    pth_mutex_acquire(mutex, FALSE, NULL);

    return TRUE;
}

int pth_cond_notify(pth_cond_t *cond, int broadcast)
{
    pth_t w;
    int wokelocal;

    /* consistency checks */
    if (cond == NULL)
        return pth_error(FALSE, EINVAL);
    if (!(pth_atomic_load(&cond->cn_state) & PTH_COND_INITIALIZED))
        return pth_error(FALSE, EDEADLK);

    /* dequeue one (or all) waiters and hand each a directed wakeup;
       notifying without waiters is a no-op (POSIX semantics) */
    wokelocal = FALSE;
    for (;;) {
        pth_spin_lock(&cond->cn_lock);
        w = pth_waitq_shift(&cond->cn_waitq);
        if (w != NULL)
            cond->cn_waiters--;
        pth_spin_unlock(&cond->cn_lock);
        if (w == NULL)
            break;
        pth_gsched_wake(w->sched_home, w, w->wq_event);
        if (w->sched_home == pth_gsched_active)
            wokelocal = TRUE;
        if (!broadcast)
            break;
    }

    /* Compatibility with original GNU Pth scheduling semantics.  This eager
       yield can make a waiter run while the notifying thread still holds the
       associated mutex, forcing it to block a second time.  The default is to
       make the waiter runnable and let the notifier reach its next scheduling
       point (normally after releasing that mutex). */
#if defined(PTH_COND_LEGACY_YIELD)
    if (wokelocal)
        pth_yield(NULL);
#else
    (void)wokelocal;
#endif

    /* return to caller */
    return TRUE;
}

/*
**  Barriers
*/

int pth_barrier_init(pth_barrier_t *barrier, int threshold)
{
    if (barrier == NULL || threshold <= 0)
        return pth_error(FALSE, EINVAL);
    if (!pth_mutex_init(&(barrier->br_mutex)))
        return FALSE;
    if (!pth_cond_init(&(barrier->br_cond)))
        return FALSE;
    pth_atomic_store(&barrier->br_state, PTH_BARRIER_INITIALIZED);
    barrier->br_threshold = threshold;
    barrier->br_count     = threshold;
    barrier->br_cycle     = FALSE;
    return TRUE;
}

int pth_barrier_reach(pth_barrier_t *barrier)
{
    int cancel, cycle;
    int rv;

    if (barrier == NULL)
        return pth_error(FALSE, EINVAL);
    if (!(pth_atomic_load(&barrier->br_state) & PTH_BARRIER_INITIALIZED))
        return pth_error(FALSE, EINVAL);

    if (!pth_mutex_acquire(&(barrier->br_mutex), FALSE, NULL))
        return FALSE;
    cycle = barrier->br_cycle;
    if (--(barrier->br_count) == 0) {
        /* last thread reached the barrier */
        barrier->br_cycle   = !(barrier->br_cycle);
        barrier->br_count   = barrier->br_threshold;
        if ((rv = pth_cond_notify(&(barrier->br_cond), TRUE)))
            rv = PTH_BARRIER_TAILLIGHT;
    }
    else {
        /* wait until remaining threads have reached the barrier, too */
        pth_cancel_state(PTH_CANCEL_DISABLE, &cancel);
        if (barrier->br_threshold == barrier->br_count)
            rv = PTH_BARRIER_HEADLIGHT;
        else
            rv = TRUE;
        while (cycle == barrier->br_cycle) {
            if (!(rv = pth_cond_await(&(barrier->br_cond), &(barrier->br_mutex), NULL)))
                break;
        }
        pth_cancel_state(cancel, NULL);
    }
    pth_mutex_release(&(barrier->br_mutex));
    return rv;
}
