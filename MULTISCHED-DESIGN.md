# GNU Pth Multi-Scheduler Design

**Goal:** Run N independent Pth schedulers, one per OS thread (pthread), each cooperatively
scheduling its own set of lightweight threads, with `pth_mutex_t`, `pth_cond_t`,
`pth_rwlock_t`, and `pth_barrier_t` working across schedulers.

**Constraints chosen:** native pthreads back each scheduler; no thread migration (a
lightweight thread lives and dies on the scheduler that spawned it); existing
single-scheduler API remains source-compatible.

---

## 1. How Pth works today (relevant facts)

All scheduler state is process-global, defined in `pth_sched.c`:

```c
intern pth_t        pth_main, pth_sched, pth_current;
intern pth_pqueue_t pth_NQ, pth_RQ, pth_WQ, pth_SQ, pth_DQ;
intern int          pth_favournew;
intern float        pth_loadval;
static int          pth_sigpipe[2];
static sigset_t     pth_sigpending, pth_sigblock, pth_sigcatch, pth_sigraised;
```

Blocking is uniform: a thread builds an event ring, calls `pth_wait()`
(`pth_event.c:386`), which sets `state = PTH_STATE_WAITING` and context-switches to the
scheduler. The scheduler's `pth_sched_eventmanager()` **polls every event predicate of
every waiting thread on every loop iteration**, then sleeps in `select()` (woken early by
a byte written to `pth_sigpipe`).

The sync primitives contain **no waiter queues**. They are plain state polled by the
event manager:

* `PTH_EVENT_MUTEX` occurs when `!(mutex->mx_state & PTH_MUTEX_LOCKED)` (`pth_sched.c:526`).
  `pth_mutex_acquire()` loops: wait on that event, re-check, CAS-less retake.
* `PTH_EVENT_COND` occurs based on `cn_state` flags `SIGNALED/BROADCAST/HANDLED`; the
  event manager itself clears the flags in a post-processing pass (`pth_sched.c:789`).
* `pth_rwlock_t` and `pth_barrier_t` are built purely on top of mutex + cond.

This polling model is the crux of the problem: **a scheduler only notices a predicate
change when its own loop runs.** If a thread on scheduler B releases a mutex, scheduler A
may be asleep in `select()` with no fd activity and never re-evaluate the predicate.
Additionally, `mx_state`, `cn_state`, `cn_waiters`, etc. would be read/written by
multiple OS threads with no synchronization.

Other process-global state that becomes shared under MP (§7): the TSD key table
`pth_keytab` (`pth_data.c:35`), `pth_errno_storage/pth_errno_flag` (`pth_errno.c`),
the global msgport ring (`pth_msg.c:42`), the atfork handler table (`pth_fork.c`),
and — critically — the sjlj machine-context bootstrap in `pth_mctx.c`, which uses
static globals (`mctx_creating`, `mctx_trampoline`, `mctx_caller`) plus a process-wide
`SIGUSR1` handler and `sigaltstack` dance, making concurrent `pth_spawn()` unsafe.

---

## 2. Architecture overview

### 2.1 The core invariant

> **A thread control block, its event ring, and the five run queues are touched only by
> the thread's home scheduler (the OS thread running it).**

Cross-scheduler interaction is limited to exactly two shared-memory channels:

1. **Primitive-internal state** (mutex/cond/rwlock/barrier fields), protected by a small
   per-primitive spinlock.
2. **Per-scheduler wakeup inbox**: an MPSC list onto which remote schedulers push
   "wake thread T for event E" notifications, paired with a byte written to the target
   scheduler's existing sigpipe to break it out of `select()`.

This keeps the entire existing scheduler core (queue handling, event-ring walking,
context switching, signal bookkeeping) single-threaded and unchanged in behavior. No
lock is ever held across a context switch; spinlocks protect only short
enqueue/dequeue/flag sections.

### 2.2 The scheduler object

All globals listed in §1 move into a scheduler struct:

```c
typedef struct pth_gsched_st pth_gsched_t;
struct pth_gsched_st {
    int            id;
    pthread_t      os_thread;
    pth_t          main;          /* pseudo-main of this scheduler        */
    pth_t          sched;         /* scheduler TCB                        */
    pth_t          current;       /* currently running lwt                */
    pth_pqueue_t   NQ, RQ, WQ, SQ, DQ;
    int            favournew;
    float          loadval;
    pth_time_t     loadticknext, loadtickgap;
    int            sigpipe[2];
    sigset_t       sigpending, sigblock, sigcatch, sigraised;
    /* MP additions */
    pth_inbox_t    inbox;         /* MPSC wakeup inbox (see §4)           */
    int            state;         /* RUNNING | SHUTDOWN                   */
};
```

A thread-local pointer identifies the calling OS thread's scheduler:

```c
static __thread pth_gsched_t *pth_gsched_self_;   /* or pthread_getspecific fallback */
#define pth_gsched_self() (pth_gsched_self_)
```

Every reference to `pth_current`, `pth_RQ`, etc. throughout the tree becomes
`pth_gsched_self()->current`, etc. For source compatibility and to keep the diff
reviewable, keep the old names as macros during Phase 1:

```c
#define pth_current (pth_gsched_self()->current)
#define pth_RQ      (pth_gsched_self()->RQ)
/* ... */
```

The TCB (`struct pth_st`, defined in the `#if cpp` section of `pth_tcb.c:34`) gains one
field: `pth_gsched_t *sched` — the
home scheduler, set at spawn and never changed (no migration).

### 2.3 Public API additions

```c
int   pth_mp_init(int nschedulers);   /* pth_init() == pth_mp_init(1)           */
pth_t pth_spawn_on(int sched_id, pth_attr_t attr, void *(*f)(void *), void *arg);
int   pth_sched_count(void);
int   pth_sched_id(void);             /* scheduler of the calling lwt           */
int   pth_mp_shutdown(void);          /* join all schedulers; called from sched 0 */
```

`pth_mp_init(n)` runs `pth_init()` logic for scheduler 0 on the calling OS thread, then
spawns `n-1` pthreads. Each auxiliary pthread creates its scheduler struct, its
scheduler TCB, and a pseudo-main TCB (which just services the scheduler until real
threads arrive), sets the TLS pointer, and enters `pth_scheduler()`. `pth_spawn()`
without a target spawns on the caller's scheduler; `pth_spawn_on()` inserts the new TCB
into the *target* scheduler's NQ **via the inbox** (see §4.3), never by touching the
remote NQ directly.

---

## 3. Cross-scheduler synchronization primitives

### 3.1 From polled predicates to waiter queues

Polling cannot work across schedulers, so mutex and cond change from
"state polled by the event manager" to "explicit waiter queue + directed wakeup".
`PTH_EVENT_MUTEX` and `PTH_EVENT_COND` become **notification events**: the event manager
no longer evaluates their predicates (the two `else if` blocks at `pth_sched.c:526` and
`:531`, and the cond post-processing at `:789`, are removed). Their `ev_status` is set
to `PTH_STATUS_OCCURRED` only by the home scheduler while draining its inbox. Because the
event-manager cleanup loop already moves any waiting thread that has a non-PENDING event
to the ready queue, no other scheduler-core change is needed for wakeup delivery.

This is also an improvement in the single-scheduler case: it removes the thundering-herd
re-check of every waiter and the fragile `SIGNALED/BROADCAST/HANDLED` cond flag protocol.

### 3.2 Spinlock

A one-word test-and-set lock using C11 atomics (GCC `__atomic` builtins as fallback;
`configure` gains a check, and MP mode is simply unavailable on platforms without them):

```c
typedef struct { volatile int lock; } pth_spin_t;
#define PTH_SPIN_INIT {0}
void pth_spin_lock(pth_spin_t *s);    /* acquire semantics */
void pth_spin_unlock(pth_spin_t *s);  /* release semantics */
```

Hold times are O(1) list operations; a lightweight thread never context-switches while
holding one, so spinning (with `sched_yield()` backoff) is appropriate.

### 3.3 Mutex

```c
struct pth_mutex_st {
    pth_ringnode_t mx_node;
    int            mx_state;
    pth_t          mx_owner;
    unsigned long  mx_count;
    /* MP additions */
    pth_spin_t     mx_lock;
    pth_waitq_t    mx_waiters;    /* FIFO of pth_t blocked on this mutex */
};
```

This changes the public struct layout and `PTH_MUTEX_INIT` (both in `pth.h.in`), so MP
Pth is not binary-compatible with old clients — acceptable; it is source-compatible.
The waiter linkage lives in the TCB (`pth_t wq_next` + a pointer to the event being
waited on), so enqueue/dequeue allocate nothing.

Acquire (replaces the loop at `pth_sync.c:82`):

```
pth_spin_lock(&m->mx_lock);
if (!(m->mx_state & LOCKED)) { take it; unlock; return TRUE; }
if (m->mx_owner == self)     { mx_count++; unlock; return TRUE; }
if (tryonly)                 { unlock; return EBUSY; }
loop:
    enqueue self on m->mx_waiters;          /* still holding mx_lock */
    pth_spin_unlock(&m->mx_lock);
    ev = pth_event(PTH_EVENT_MUTEX|PTH_MODE_STATIC, &ev_key, m);
    concat ev_extra if given;
    pth_wait(ev);                            /* blocks; woken via inbox */
    pth_spin_lock(&m->mx_lock);
    remove self from m->mx_waiters if still enqueued;   /* timeout/ev_extra path */
    if (ev_extra fired and mutex event still pending) { unlock; return EINTR; }
    if (!(m->mx_state & LOCKED)) { take it; unlock; return TRUE; }
    goto loop;                               /* barging retry, as today */
```

Release (replaces `pth_sync.c:105`):

```
pth_spin_lock(&m->mx_lock);
if (--m->mx_count > 0) { unlock; return; }
m->mx_state &= ~LOCKED; m->mx_owner = NULL;
w = dequeue one waiter (may be NULL);
pth_spin_unlock(&m->mx_lock);
if (w) pth_sched_wake(w->sched, w, w->waitev);   /* inbox push + pipe byte */
```

Retry-on-wake (barging) is chosen over direct ownership handoff because it preserves
current Pth semantics exactly and keeps the release path trivially correct; handoff can
be added later as a fairness option.

**Lost-wakeup safety:** the waiter is on `mx_waiters` *before* dropping `mx_lock`, and
the releaser holds `mx_lock` while dequeuing — so a release either sees the waiter and
wakes it, or happened before enqueue, in which case the `!(LOCKED)` re-check inside the
lock catches it. The window between "enqueued" and "actually switched into the WQ" is
covered by the inbox protocol (§4.2): a wakeup that arrives early just marks the event
occurred before the thread finishes entering the wait state, and the event manager's
existing non-PENDING scan picks it up on the next loop iteration.

`pth_mutex_releaseall()` (thread-death cleanup, `pth_sync.c:128`) iterates the TCB's
`mutexring`; the ring is only ever touched by the owning thread, so it needs no lock,
but each release inside it goes through the locked release path above.

### 3.4 Condition variables

```c
struct pth_cond_st {
    unsigned long cn_state;
    unsigned int  cn_waiters;     /* count, kept for compat/debug */
    /* MP additions */
    pth_spin_t    cn_lock;
    pth_waitq_t   cn_waitq;
};
```

`pth_cond_await` (replaces `pth_sync.c:252`):

```
pth_spin_lock(&c->cn_lock);
enqueue self on c->cn_waitq; cn_waiters++;
pth_spin_unlock(&c->cn_lock);
pth_mutex_release(mutex);                 /* AFTER enqueue: no signal can be missed */
pth_wait(cond event [+ ev_extra]);        /* cleanup handler: dequeue self, waiters-- */
pth_spin_lock(&c->cn_lock);
dequeue self if still enqueued (spurious/extra-event wake); cn_waiters--;
pth_spin_unlock(&c->cn_lock);
pth_mutex_acquire(mutex, FALSE, NULL);
```

`pth_cond_notify`: lock, dequeue one (or all, for broadcast), unlock, wake each via
`pth_sched_wake()`. The `SIGNALED/BROADCAST/HANDLED` flag machinery and the short-circuit
"signal with no waiter pending" path disappear; POSIX semantics (notify with no waiters
is a no-op) are preserved by the empty-queue case. The `pth_yield(NULL)` in notify is
kept only when the woken thread is on the caller's own scheduler.

Note the ordering difference from today: enqueue happens *before* releasing the mutex,
closing the classic missed-wakeup window without relying on scheduler-loop atomicity
(which no longer exists across schedulers).

### 3.5 Read-write locks and barriers

Both are already composed from mutex + cond (`pth_rwlock_st` holds two mutexes;
`pth_barrier_st` holds a mutex and a cond) and contain no scheduler-touching logic of
their own. Once mutex and cond are cross-scheduler safe, they work unchanged:

* `rw_readers`, `rw_mode` are only mutated while holding `rw_mutex_rd`/`rw_mutex_rw`
  (verified in `pth_rwlock_acquire/release`) — one caveat: `rw_mode` is written for the
  RW case after acquiring `rw_mutex_rw` and read in release by the *owner*, which is safe
  under the no-migration model.
* `pth_barrier_reach` does all state mutation under `br_mutex`; `PTH_BARRIER_INIT` and
  `PTH_MUTEX_INIT`/`PTH_COND_INIT` static initializers must be extended for the new
  fields.

---

## 4. The wakeup path

### 4.1 Inbox structure

Lock-free MPSC stack (Treiber): producers CAS-push, the owning scheduler takes the whole
list with one atomic exchange and processes it in reversed (FIFO) order.

```c
typedef struct pth_inbox_msg_st {
    struct pth_inbox_msg_st *next;
    enum { PTH_MSG_WAKE, PTH_MSG_SPAWN, PTH_MSG_SHUTDOWN } kind;
    pth_t        thread;
    pth_event_t  event;       /* WAKE: event to mark OCCURRED */
} pth_inbox_msg_t;
```

Messages for WAKE are embedded in the TCB (one slot suffices: a thread blocks on at most
one primitive at a time), so the hot path allocates nothing.

```c
void pth_sched_wake(pth_gsched_t *g, pth_t t, pth_event_t ev)
{
    /* order matters: inbox push BEFORE pipe write (see §4.2) */
    inbox_push(&g->inbox, t, ev);
    write(g->sigpipe[1], "w", 1);      /* async-signal-safe, thread-safe */
}
```

### 4.2 Event-manager integration and ordering

`pth_sched_eventmanager()` changes in exactly two places:

1. **Drain point.** Immediately after the existing "clear pipe" step (`pth_sched.c:601`)
   and *before* `select()`, drain the inbox: for each WAKE message set
   `ev->ev_status = PTH_STATUS_OCCURRED`. The existing cleanup loop then moves the
   affected threads WQ→RQ. A second drain runs after `select()` returns.
2. **Sleep correctness.** The producer pushes to the inbox *before* writing the pipe
   byte. Therefore: if the byte lands before the scheduler's pipe-drain, the inbox entry
   is visible at the drain that follows; if it lands after, `select()` wakes immediately.
   Either way no wakeup is lost, the standard self-pipe pattern.

If the woken thread has not yet completed its switch into the scheduler (it is still
`pth_current` on its home scheduler, mid-`pth_wait`), marking the event OCCURRED is
still correct: `pth_wait` counts non-pending events after it resumes, and the thread
enters the WQ with an already-occurred event, which the very next event-manager pass
promotes to the RQ. An idle scheduler (empty RQ/NQ, all threads waiting on remote
primitives) simply sleeps in `select()` on its sigpipe — no busy-wait, no periodic tick.

### 4.3 Remote spawn

`pth_spawn_on()` builds the TCB fully (stack, mctx, attributes) on the calling scheduler
— including `t->sched = target` — then sends a `PTH_MSG_SPAWN` inbox message; the target
scheduler inserts it into its own NQ at the drain point. This preserves the §2.1
invariant. The mctx bootstrap constraint (§7.4) applies.

---

## 5. What deliberately does NOT work across schedulers (phase 1)

* **`pth_join` / `PTH_EVENT_TID`** on a thread of another scheduler: the TID event polls
  a foreign TCB's `state`. Restricted (returns `EINVAL`) initially; the later fix is a
  waiter list on the TCB serviced by the dying thread's home scheduler through the same
  inbox mechanism.
* **`pth_raise`, `pth_suspend`, `pth_resume`, `pth_yield(to)`, `pth_cancel`** on foreign
  threads: same reason (they mutate foreign TCBs/queues). Return `EPERM`/`EINVAL`
  cross-scheduler in phase 1; each is a candidate for a dedicated inbox message kind later.
* **Signal events (`PTH_EVENT_SIGS`, `pth_sigwait`)**: Pth's signal machinery
  (per-loop `sigpending()` scans, handler swapping, the `pth_sigraised` protocol) is
  process-global. Scheduler 0 keeps it; auxiliary scheduler OS threads block all signals
  permanently (they already run with signals blocked, `pth_sched.c:175`, but must also
  block them while running lwts). Signal-waiting threads must live on scheduler 0.
* **Message ports** (`pth_msg.c`): global `pth_msgport` ring is unprotected; out of
  scope per requirements. Documented as scheduler-0-only until given the spinlock+inbox
  treatment (which is straightforward — `PTH_EVENT_MSG` is exactly a cond-style
  notification).
* **fd events** stay fully functional — each scheduler runs its own `select()` over its
  own threads' fds, which needs no cross-scheduler coordination.

---

## 6. Other global state fixes

| State | File | Fix |
|---|---|---|
| `pth_keytab` (TSD keys) | `pth_data.c:35` | Global spinlock around create/delete; reads of `used` made atomic. Key table stays process-global (keys are shared, values are per-lwt — already in the TCB). |
| `pth_errno_storage/flag` | `pth_errno.c` | Make `__thread`. Per-lwt errno already lives in `mctx.error` and is swapped on context switch, which stays scheduler-local. |
| `pth_initialized` | `pth_lib.c:39` | Becomes `pth_mp_state` guarded by a `pthread_once`-style latch. |
| atfork table | `pth_fork.c` | Spinlock; `pth_fork()` itself is declared scheduler-0-only (fork+MP is out of scope). |
| `pth_loadval` / `pth_ctrl` | `pth_lib.c:161` | Per-scheduler; `pth_ctrl(PTH_CTRL_GETTHREADS…)` reports the calling scheduler only; new `PTH_CTRL_GETTHREADS_ALL` sums via per-scheduler atomically-maintained counters. |
| `pth_exit_cb` main-exit logic | `pth_lib.c:396` | "Last thread" test must span all schedulers: a process-wide atomic count of live lwts; `pth_exit` of scheduler 0's main waits until the count is 1 and all auxiliary schedulers have entered SHUTDOWN (each aux scheduler sends a message to scheduler 0's inbox when it runs dry). |
| Time/`pth_snprintf`/etc. | various | Stateless or read-only after init; no change. |

---

## 7. Machine-context (mctx) constraint

`pth_mctx_set()` in the sjlj variants (`pth_mctx.c:280-447`) bootstraps a new stack via a
process-wide `SIGUSR1` handler, `sigaltstack`, and the static globals `mctx_trampoline`,
`mctx_caller`, `mctx_creating*`. Two OS threads doing this concurrently corrupt each
other.

* **Preferred:** MP mode requires the `mcsc` variant (`makecontext`/`swapcontext`), which
  is reentrant. `configure --enable-mp` forces/verifies `PTH_MCTX_MTH(mcsc)`.
* **Fallback:** a process-global "spawn lock" (pthread mutex) serializing `pth_mctx_set`
  for sjlj variants. Still fragile (signal-handler swap is process-visible), so only for
  platforms without ucontext.

`pth_mctx_switch` itself is pure register save/restore per variant and safe per-OS-thread.

---

## 8. Implementation phases

Each phase leaves the tree green under `make test`.

**Phase 0 — infrastructure.** `pth_atomic.h` (C11/`__atomic` spinlock, atomic ops,
fences), TLS detection, `--enable-mp` configure option, mcsc enforcement (§7).

**Phase 1 — scheduler encapsulation (pure refactor).** Introduce `pth_gsched_t`, move
all globals into it, single static instance, compat macros (§2.2). Zero behavior change;
the full existing test suite is the acceptance gate.

**Phase 2 — waiter-queue sync, still single scheduler.** Rework mutex/cond per §3;
delete the event-manager mutex/cond polling; add the inbox + drain point (used here only
by same-scheduler wakes, replacing predicate polling). Gate: `test_std`, `test_mp`,
`test_philo`, plus new targeted tests for cond wake ordering, `ev_extra` timeouts on
`pth_mutex_acquire`, and cancellation inside `pth_cond_await`.

**Phase 3 — multiple schedulers.** `pth_mp_init`, aux scheduler pthreads, TLS routing,
`pth_spawn_on`, cross-scheduler wake via inbox+pipe, per-scheduler `pth_ctrl`, shutdown
protocol, phase-1 restrictions of §5 enforced with errors. Gate: new stress tests —
mutex ping-pong between two schedulers, N-scheduler barrier cycles, producer/consumer
over a cross-scheduler cond, rwlock reader storms, all run long enough to catch
lost-wakeup hangs; plus `helgrind`/TSan runs on the primitive layer only (sanitizers
don't understand green-thread stack switching, so annotate or restrict scope).

**Phase 4 — extensions (optional, ordered by value).** Cross-scheduler `pth_join`;
msgports + `PTH_EVENT_MSG`; cross-scheduler `pth_cancel`/`pth_raise`; ownership-handoff
mutex fairness mode; `pthread.c` emulation layer mapping to `pth_spawn_on` round-robin.

---

## 9. Key risks

1. **Lost wakeups** — the single most likely bug class. Mitigation: the two invariants
   (enqueue-before-unlock, inbox-push-before-pipe-write) plus hang-detection stress tests.
2. **Hidden global state** — anything missed in §6 becomes a data race. Mitigation: audit
   pass over every `static`/`intern` object (`grep` inventory is in §1/§6); phase-1
   refactor makes remaining globals conspicuous.
3. **Struct/ABI change** of public sync types — clients must recompile; `PTH_MUTEX_INIT`
   et al. change. Bump `PTH_VERSION` major.
4. **Syscall-wrapping (`pth_syscall.c`)** — soft wrappers route through TLS to the right
   scheduler automatically, but calls from non-Pth OS threads must fall through to the
   real syscall (check `pth_gsched_self() == NULL`).
5. **`select()` fd limits per scheduler** — unchanged from stock Pth, but now
   per-scheduler, which actually relieves pressure.

---

## 10. Implementation notes (phases 0-3 are complete)

Two findings from phase 3 that amend the design above:

**rwlock rewritten natively (§3.5 superseded).** The classic composition
(first reader acquires `rw_mutex_rw`, last reader releases it) violates the
mutex ownership check as soon as readers truly overlap — a latent bug that
single-scheduler cooperative scheduling masked and real parallelism exposes
immediately. `pth_rwlock_t` now has its own spinlock, reader count, writer
owner and separate reader/writer wait queues (writer preference), parking
threads on a new never-polled `PTH_EVENT_NOTIFY` event type.

**Shutdown must not freeze scheduler 0.** `pth_mp_shutdown()` cannot simply
`pthread_join(3)` the auxiliary schedulers: threads on scheduler 0 may be
part of the wakeup chains (barrier mutex handoffs) that the auxiliaries'
threads still need to finish, and `pthread_join` would freeze scheduler 0's
event loop — a circular wait. It instead naps cooperatively until each
auxiliary announces its exit through an atomic counter, then reaps the OS
threads. Relatedly, the auxiliary dry-exit check must run at *both* points
where the scheduler can block waiting for work (the empty-run-queue idle
branch and the loop-bottom wait), because the STOP wakeup byte is consumed
once, and blocking afterwards would sleep forever.
