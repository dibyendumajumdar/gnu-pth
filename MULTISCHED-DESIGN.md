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

> Note: this section records the **phase-1** restrictions. Several have since
> been lifted in phase 4 — see §11 (`pth_join`), §14 (message ports), §16
> (`pth_cancel`/`pth_raise`), and the current status matrix in §15.

* **`pth_join` / `PTH_EVENT_TID`** on a thread of another scheduler: the TID event polls
  a foreign TCB's `state`. Restricted (returns `EINVAL`) initially; the later fix is a
  waiter list on the TCB serviced by the dying thread's home scheduler through the same
  inbox mechanism. **(RESOLVED — implemented in §11.)**
* **`pth_raise`, `pth_suspend`, `pth_resume`, `pth_yield(to)`, `pth_cancel`** on foreign
  threads: same reason (they mutate foreign TCBs/queues). Return `EPERM`/`EINVAL`
  cross-scheduler in phase 1; each is a candidate for a dedicated inbox message kind later.
  **(`pth_cancel`/`pth_raise` RESOLVED in §16; `pth_suspend`/`pth_resume`/`pth_yield(to)`
  still restricted.)**
* **Signal events (`PTH_EVENT_SIGS`, `pth_sigwait`)**: Pth's signal machinery
  (per-loop `sigpending()` scans, handler swapping, the `pth_sigraised` protocol) is
  process-global. Scheduler 0 keeps it; auxiliary scheduler OS threads block all signals
  permanently (they already run with signals blocked, `pth_sched.c:175`, but must also
  block them while running lwts). Signal-waiting threads must live on scheduler 0.
* **Message ports** (`pth_msg.c`): global `pth_msgport` ring is unprotected; out of
  scope per requirements. Documented as scheduler-0-only until given the spinlock+inbox
  treatment (which is straightforward — `PTH_EVENT_MSG` is exactly a cond-style
  notification). **(RESOLVED in §14.)**
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

## 11. Phase 4 progress

**Mutex fairness (ownership handoff).** `PTH_MUTEX_FAIR` (a new `mx_state`
bit, also available as `PTH_MUTEX_INIT_FAIR` and via `pth_mutex_setfair()`)
switches a mutex from the default barging release to direct ownership
handoff: on release the lock is passed straight to the head waiter (lock
stays held, the waiter appends the mutex to its *own* mutexring on resume so
no foreign TCB is touched), giving FIFO/no-starvation. The cancellation
cleanup handler re-hands-off (or fully releases) if a thread is cancelled
after being handed the lock, so the lock can never be stranded on a dead
thread. Default behaviour is unchanged.

**Cross-scheduler `pth_join` implemented (§5 restriction lifted).** The TCB
gains a spinlock-guarded notify wait queue (`join_lock`, `join_waitq`,
`join_done`). A joiner whose target lives on another scheduler parks on that
queue on a never-polled `PTH_EVENT_NOTIFY` event instead of returning
`EINVAL`. When a joinable thread dies, its home scheduler places it in its DQ
(as before), then under `join_lock` sets `join_done` and delivers a directed
wakeup (`pth_gsched_wake`) to every parked joiner. The woken joiner reads
`join_arg` under `join_lock`, then posts a new `PTH_GMSG_REAP` inbox message
so the *home* scheduler — the sole owner of the dead TCB — frees it; the
joiner never frees a foreign TCB. Ordering (home does DQ-insert then wake
before any REAP can drain on the same OS thread) guarantees the reap always
finds the TCB in the DQ. Same-scheduler `pth_join` and `pth_join(NULL)`
(join-any) are unchanged. Constraint (unchanged from classic Pth): a thread
must have a single joiner, and a cross-scheduler-joined thread must not also
be subject to join-any on its home scheduler.

## 12. Prototype: poll(2) scheduler core (opt-in, `PTH_SCHED_POLL`)

`select(2)`/`fd_set` imposes a hard `FD_SETSIZE` (typically 1024) ceiling on
descriptor *values*: `FD_SET(fd, …)` with `fd >= FD_SETSIZE` corrupts memory,
so Pth defensively rejects such fds in `pth_util_fd_valid()`. The per-scheduler
split (§9.5) relieves the fd *count* each scheduler scans but not this ceiling,
since fd values are process-global.

An opt-in `poll(2)` backend removes the ceiling. Built with
`-DPTH_SCHED_POLL` (CMake: `-DPTH_SCHED_POLL=ON`); the default build is the
unchanged `select(2)` path. Three layers change, all behind the flag:

* **Scheduler core** (`pth_sched.c`). `pth_sched_eventmanager()` becomes a thin
  dispatcher to `pth_sched_eventmanager_poll()` (poll) or the original
  `pth_sched_eventmanager_select()`. The poll variant assembles a grow-on-demand
  `struct pollfd` vector (single-fd `PTH_EVENT_FD` events, plus each set fd of a
  `PTH_EVENT_SELECT` event, plus the self-pipe), blocks in `poll()`, and reads
  readiness from `revents`. `POLLNVAL` pinpoints bad descriptors directly,
  replacing select's per-fd error re-probe.
* **I/O wrappers** (`pth_high.c`). The "fast-poll before parking" step in the
  six `pth_{read,readv,recv,write,writev,send}_ev` paths uses a one-entry
  `poll()` instead of `FD_SET`+`select`.
* **Validation** (`pth_util.c`). `pth_util_fd_valid()` drops the
  `fd >= FD_SETSIZE` rejection.

`PTH_EVENT_SELECT` (from `pth_select`) keeps `fd_set` semantics at the API
boundary (its `nfd` is `<= FD_SETSIZE` by contract); the poll core synthesises
its readiness `fd_set`s from `revents`, which is safe within that bound. Test
`test_pollfd.c` exercises the FD path, the SELECT path, and a descriptor above
`FD_SETSIZE` (passes under `PTH_SCHED_POLL`, cleanly skipped otherwise). The
full suite passes on the poll backend in both single-scheduler and MP builds.

Not yet converted (same pattern, straightforward follow-ups): the `connect`/
`accept` fast-polls, the reusable pollfd vector is currently reallocated per
pass rather than cached on the scheduler struct, and the millisecond timeout
granularity of `poll()` is coarser than select's microseconds (rounded up, so
never a busy-spin). A stateful `epoll`/`kqueue` backend behind the same
dispatcher remains the scalable end goal (§ discussion).

## 13. Phase 4: multi-scheduler pthread emulation

The POSIX pthread emulation (`pthread.c`) now works together with `PTH_MP`,
so existing pthreads programs get multi-core parallelism by relinking against
`libpthread` (Pth's) with no source changes. `pthread_create(3)` round-robins
new threads across the schedulers via `pth_spawn_on()`; `pthread_join(3)` rides
on the cross-scheduler join of §11; the mutex/cond/rwlock emulation already
maps onto the cross-scheduler-safe primitives.

**The symbol-collision problem.** `PTH_MP` needs the *real* libc
`pthread_create`/`pthread_join`/`pthread_sigmask` to run its scheduler OS
threads, but the emulation *is* those symbols. Resolution (in `pth_sched.c`,
selected by the `PTH_EMULATION` compile flag set only on the emulation
library):

* **Emulation build** (`PTH_MP && PTH_EMULATION`): the real primitives are
  fetched once via `dlsym(RTLD_NEXT, …)`, which skips the emulation's own
  symbols and finds libc's. `_GNU_SOURCE` is defined at the top of the file
  (before any header) so `RTLD_NEXT` is visible; the library links `libdl`.
* **Plain MP build** (`PTH_MP`, no emulation): `pth_sched.c` deliberately does
  *not* `#include <pthread.h>` — in a combined build Pth's own `pthread.h` can
  shadow the system one — and instead declares the three primitives directly
  (`pthread_t` is `unsigned long` on the supported targets, matching the
  `osthread` field).

Both go through one set of `PTH_OS_{CREATE,JOIN,SIGMASK}` macros, so the
scheduler bootstrap/shutdown code is backend-agnostic.

**Portability note (FreeBSD).** `pthread.c` includes both the emulation
`pthread.h` (which declares `pthread_kill(pthread_t, int)` with Pth's own
`pthread_t`) and `pth_p.h` (which pulls in `<signal.h>`). On glibc, `<signal.h>`
spells `pthread_kill`'s argument with the `pthread_t` token that Pth has
remapped, so the two declarations agree; on FreeBSD `<signal.h>` uses the raw
system type `__pthread_t`, so they conflict. `pthread.c` therefore hides the
system `pthread_kill` declaration (`#define pthread_kill __pth_sys_pthread_kill`
around the `pth_p.h` include) -- nothing there uses it. This surfaced only via
the FreeBSD CI job.

**Startup / shutdown.** The first pthread call runs `pth_mp_init(N)` where
`N = $PTH_SCHEDULERS` if set, else the online CPU count (clamped to
`PTH_GSCHED_MAX`); the `atexit` handler runs `pth_mp_shutdown()` before
`pth_kill()`. CMake gains no new option — building `-DPTH_BUILD_PTHREAD=ON
-DPTH_MP=ON` is now allowed (the former `FATAL_ERROR` is gone) and compiles
the emulation with `PTH_EMULATION` + `libdl`.

`test_pthread_mp.c` confirms distribution: N pthreads report a spread of
distinct OS-thread ids (`gettid`, one per scheduler, no migration) and a
`pthread_mutex`-protected counter stays exact under the real parallelism.

Caveat: the `pthread_t == unsigned long` assumption and `dlsym(RTLD_NEXT)`
symbol interposition target glibc/musl-style dynamic linking; fully static
executables or platforms with a struct `pthread_t` would need a small porting
shim.

## 14. Phase 4: cross-scheduler message ports (§5 restriction lifted)

Message ports (`pth_msg.c`) were documented in §5 as scheduler-0-only: the
global registry ring and each port's `mp_queue` were unprotected, and
`PTH_EVENT_MSG` is *polled* by the event manager rather than sitting on a
directed-wakeup waiter queue, so a scheduler asleep in `poll(2)` would never
notice a message arriving from another scheduler.

The fix keeps the polled predicate (no waiter-queue rework needed) and adds
two things:

* **Locking.** A per-port spinlock (`mp_lock`) guards each `mp_queue`
  (`put`/`get`/`pending` and the event-manager predicate read, in both the
  select and poll backends); a global spinlock guards the registry ring
  (`create`/`destroy`/`find`).
* **Wakeup.** `pth_msgport_put()` appends under the lock, then calls
  `pth_gsched_kick_others()`, which writes a byte to every *other* scheduler's
  signal pipe. That breaks any scheduler sleeping in `poll(2)` out of its wait;
  its next event-manager assembly pass re-reads the (now non-empty) port under
  `mp_lock` and promotes the waiter. The kick carries no inbox message and is
  idempotent — a spurious kick just costs one extra event-manager pass. The
  ordering (append-before-kick, both after the lock is released) closes the
  lost-wakeup window exactly as the mutex/cond path does.

**Scheduler-0 robustness.** This exposed a latent assumption: scheduler 0
treated an empty ready queue as a fatal "no more threads to schedule" error,
whereas a kick can wake it with its only thread still parked on a port. The
loop now waits (re-runs the event manager) whenever threads remain in the
waiting/new/suspend queues, and only aborts when *all* queues are truly empty —
which also hardens scheduler 0 against spurious signal-pipe wakeups in general.

`test_msgport_mp.c`: a receiver on scheduler 0 drains a port fed by senders on
schedulers 1..3; it checks the exact message count and a checksum, and passes
under both the select and poll backends with no lost-wakeup hang.

## 15. API compatibility status under M:N (as of phase 4 in progress)

A map of the original single-scheduler API against the current multi-scheduler
(M:N) build. "Cross-scheduler safe" means the call works regardless of which
scheduler owns the target thread or primitive.

### Fully cross-scheduler safe
* Synchronization: `pth_mutex_*` (including the `PTH_MUTEX_FAIR` handoff mode),
  `pth_cond_*`, `pth_rwlock_*`, `pth_barrier_*` — spinlock-guarded state with
  directed wakeups through the target's home-scheduler inbox.
* `pth_join(tid)` on a specific thread (§11); `pth_join(NULL)` join-any remains
  local (reaps only the calling scheduler's dead queue).
* Message ports `pth_msgport_*` / `PTH_EVENT_MSG` (§14).
* `pth_spawn` (caller's scheduler) and `pth_spawn_on` (chosen scheduler).
* Thread-specific data `pth_key_*` — process-global key table under a spinlock
  (`pth_data.c`); per-thread values live in the TCB.
* Per-thread `errno`; fd / `select` / timer events (`PTH_EVENT_FD`,
  `PTH_EVENT_SELECT`, `PTH_EVENT_TIME`) — each scheduler runs its own event
  loop, no cross-scheduler coordination needed.
* `pth_self`, `pth_sched_id`, `pth_sched_count`, `pth_yield(NULL)`.

### Works, but scoped to the calling scheduler (semantics changed)
* `pth_ctrl(PTH_CTRL_GETTHREADS…)` counts only the calling scheduler's queues
  (the `pth_NQ`/`pth_RQ`/… macros resolve to `pth_gsched_active->…`). There is
  no process-wide total; a `PTH_CTRL_GETTHREADS_ALL` summing per-scheduler
  counters is a candidate addition.
* `pth_ctrl(PTH_CTRL_GETAVLOAD)` and `pth_ctrl(PTH_CTRL_FAVOURNEW)` are
  per-scheduler.

### Cross-scheduler safe (added in phase 4, §16 and §18)
* `pth_cancel` (deferred and asynchronous) and `pth_raise` — routed to the
  target's home scheduler through the inbox (§16).
* `pth_suspend`, `pth_resume`, and `pth_yield(target)` — likewise routed to the
  target's home scheduler (§18). These are asynchronous *requests*; they take
  effect at the target scheduler's next drain point (its next idle/block), so a
  target that never yields to its own scheduler will not observe them promptly.

### Restricted across schedulers (return EPERM/EINVAL for a foreign target)
* (none remaining — all original thread-control verbs now work across
  schedulers, subject to the asynchronous-delivery note above.)

### Scheduler-0-only / process-global (deliberately not M:N)
* Signals — `PTH_EVENT_SIGS`, `pth_sigwait`, `pth_sigmask`: the signal
  machinery is scheduler-0 business; auxiliary schedulers run with all signals
  blocked, so signal-waiting threads must live on scheduler 0.
* `pth_fork` — fork + MP is out of scope.


## 16. Phase 4: cross-scheduler pth_cancel and pth_raise

`pth_cancel` and `pth_raise` mutate the target thread's TCB and its home
scheduler's run queues, so — like `pth_join` and message ports — the work is
handed to the target's home scheduler through the wakeup inbox. Two new message
kinds carry the request: `PTH_GMSG_CANCEL` and `PTH_GMSG_RAISE` (the latter
also carries a signal number in a new `sig` field of the inbox message).

* **Front ends.** `pth_cancel`/`pth_raise` keep the classic behaviour when the
  target is local. For a foreign target (`sched_home != pth_gsched_active`, a
  read that is always safe since `sched_home` is immutable) they post the
  request and return success — a *request*, carried out asynchronously, exactly
  as `pthread_cancel(3)` specifies. Globally-ignored signals and `sig == 0`
  existence tests are short-circuited on the caller side.
* **Local workers.** The bodies were extracted into `pth_cancel_local()` and
  `pth_raise_local()`, run either directly (local call) or from the home
  scheduler's inbox drain. Each first re-validates the target with
  `pth_thread_exists()` (it may have terminated — and, if detached, been freed
  — between posting and processing; for a detached target this remains
  inherently racy, as in POSIX).
* **Deferred vs async cancel.** Deferred cancellation just sets `cancelreq`;
  the target's own scheduler promotes it (the event manager already treats
  `cancelreq` as an occurred event) and it exits at its next cancellation point
  through the normal death path. Asynchronous cancellation reaps the target
  in place; that path was extended to also publish death on the TCB and wake
  any cross-scheduler joiners (`join_done` + `join_waitq`), mirroring the
  scheduler's normal death block — without which a remote `pth_join()` of an
  async-cancelled thread would hang.
* **Raise.** `pth_raise_local()` sets the per-thread pending signal; a target
  parked on `PTH_EVENT_SIGS` is promoted by the event manager's next pass. This
  uses the per-thread `sigpending` bitmap (not OS signals), so it works on
  auxiliary schedulers even though process-signal machinery stays on
  scheduler 0. Unlike the local path it does not force the target to run
  immediately; delivery is "soon", at the home scheduler's next loop.

`test_cancelraise_mp.c` covers all three: deferred cancel of a looping thread
on one scheduler, async cancel of a napping thread on another, and a SIGUSR1
raise to a `pth_sigwait` thread on a third — each verified through a
cross-scheduler `pth_join`, under both the select and poll backends.

(`pth_suspend`/`pth_resume`/directed `pth_yield(target)` were the last verbs
still pinned to one scheduler; they are lifted in §18.)


## 17. Design assessment: the wakeup substrate vs. other M:N runtimes

### What the substrate actually is

Cross-scheduler communication is a **doorbell + mailbox** pattern, plus direct
shared memory for primitive state:

* **Doorbell.** A single byte written to the target scheduler's self-pipe
  (`sigpipe`). It carries no information; its only job is to break the target
  out of the `poll(2)`/`select(2)` call it blocks in when idle. On Linux an
  `eventfd` would be a cheaper doorbell (one fd + an 8-byte counter instead of
  a pipe pair).
* **Mailbox.** A lock-free MPSC stack (Treiber) — the per-scheduler inbox —
  carrying the payload: `WAKE` (mark an event occurred), `SPAWN` (adopt a
  remotely created thread), `REAP` (free a joined TCB), `CANCEL`, `RAISE`,
  `STOP`. Producers CAS-push; the owning scheduler takes the whole list with
  one atomic exchange and processes it FIFO. The invariant that closes the
  lost-wakeup window is **push-to-mailbox before ring-the-doorbell**, paired
  with the scheduler clearing its pipe *before* draining — the standard
  self-pipe discipline.
* **Shared memory (not messages).** The synchronization primitives' internal
  state — the mutex locked bit and waiter queue, cond waitqueue, rwlock counts,
  msgport queues — lives in plain shared memory under small per-primitive
  spinlocks. Threads on different schedulers read/write it directly; the inbox
  is used only for the *directed wakeup* that follows a state change, and for
  thread-control ops that must touch a foreign scheduler's private queues. This
  is a hybrid, unlike pure message-passing runtimes (Erlang) that copy
  everything through mailboxes.

It is worth separating the **wakeup mechanism** from the **scheduling policy**,
because they sit very differently against the state of the art.

### The wakeup mechanism: standard and appropriate

Self-pipe + `poll()` + an MPSC inbox is the same pattern libuv (`uv_async`),
Python asyncio, and Tokio's I/O driver use to wake an event loop from another
thread: register a wakeup fd in the same readiness set you already block on for
I/O, and let other threads poke it. For an fd-centric green-thread library this
is arguably the *right* idiom — each scheduler is already parked in `poll()`
waiting for socket readiness, so unifying "an fd became ready" and "another
scheduler wants my attention" into one blocking call avoids a second wait
primitive. The cost is that a wakeup is two syscalls (a `write` and the `poll`
return), ~1-3 µs round trip, versus ~0.5-1 µs for futex park/unpark (what Go and
Tokio worker parking use). But a futex cannot also wait on fds, so a futex
design ends up registering an eventfd in epoll anyway — i.e. it reinvents the
self-pipe. The cheap incremental wins here are `eventfd` for the doorbell and
`epoll`/`kqueue` for the wait (the poll-backend prototype, §12, is a step toward
the latter); neither is a fundamental change.

### The scheduling policy: deliberately conservative

This is where the design trails top-tier M:N runtimes, by choice:

* **No migration / no work-stealing.** The core invariant — a thread lives and
  dies on its home scheduler — lets the entire single-threaded scheduler core
  (run queues, event-ring walking, context switching) be reused verbatim and
  never made concurrent. That is a large correctness and reviewability win for
  retrofitting M:N onto a mature cooperative library. The cost is load
  balancing: Go, Tokio, Java Loom and Erlang/BEAM all work-steal across
  concurrent (Chase-Lev-style) deques so an idle core pulls work from a busy
  one. Here, balancing is *static* — the pthread layer round-robins at thread
  creation (§13) and never rebalances — so uniform workloads spread well but
  skewed ones can leave cores idle with no recourse. Work-stealing is where most
  M:N runtime complexity and bugs live, so omitting it is a defensible trade,
  but it is the clearest scalability gap.
* **Cooperative, no preemption.** A CPU-bound thread that never yields
  monopolizes its scheduler. Go (async preemption via signals), Loom, and BEAM
  (reduction counting) preempt. This is inherent to Pth's model, not the MP
  layer.
* **The msgport "kick everyone" path (§14)** is the least refined piece — a
  `put` wakes *all* other schedulers to re-poll rather than doing a directed
  wakeup to the specific waiter (as mutex/cond do), because ports do not track
  their waiters. Fine for a low-frequency IPC feature; O(schedulers) doorbells
  per message, so not suitable as a hot path.

### Verdict

The communication substrate is sound, standard, and correct — not novel, but
the right pattern for an fd-driven library, with careful lost-wakeup reasoning.
Where it trails Go/Tokio/Loom is scheduling *policy*, not *plumbing*: static
load balancing (no work-stealing) and cooperative-only execution. For I/O-bound
or evenly distributed workloads it should hold up well; for fine-grained compute
parallelism with skewed load or heavy cross-thread signalling, a work-stealing,
futex-parked, preemptive runtime would pull ahead. Given the goal — retrofitting
M:N onto Pth while keeping the existing core intact and source-compatible —
trading peak scalability for simplicity and correctness is a reasonable place to
land. Natural next steps to close the gap, in increasing order of effort:
`eventfd` doorbell, an `epoll`/`kqueue` scheduler backend, directed msgport
wakeups, and ultimately some form of work-stealing / thread migration (by far
the largest, as it breaks the single-owner invariant the rest of the design
leans on).


## 18. Phase 4: cross-scheduler pth_suspend / pth_resume / pth_yield(target)

The last three thread-control verbs move a thread between its home scheduler's
run queues (`pth_suspend`: live queue -> suspend queue; `pth_resume`: the
reverse; `pth_yield(target)`: bump the target to the front of its ready/new
queue). Like `pth_cancel`, the work is handed to the target's home scheduler
through the inbox, with three new message kinds `PTH_GMSG_SUSPEND`,
`PTH_GMSG_RESUME`, `PTH_GMSG_FAVOR`.

* **Front ends / local workers.** `pth_suspend`/`pth_resume`/`pth_yield` keep
  the classic path for a local target; for a foreign target they post the
  request and return success. The bodies were extracted into
  `pth_suspend_local()`, `pth_resume_local()` and `pth_favor_local()`, run
  either directly or from the home scheduler's inbox drain. Each re-validates
  the target with `pth_thread_exists()` before dereferencing it — that helper
  only compares queue-node addresses, so it is safe even if the TCB was freed
  between the request being posted and processed.
* **`pth_yield(target)` cross-scheduler.** A thread on scheduler A cannot favour
  a thread that runs on scheduler B, so the foreign case posts a `FAVOR` hint to
  B (which bumps the target in its ready/new queue if still schedulable) and
  then yields A's own scheduler, i.e. it degrades to `pth_yield(NULL)` plus a
  remote hint.
* **Suspending a waiting thread.** A thread parked on a sync primitive (in the
  WQ) can be suspended: it moves WQ -> SQ while its pending event stays on the
  TCB. A wakeup that arrives meanwhile just marks the event occurred; on
  `pth_resume` the thread returns to the WQ and the event manager promotes it on
  its next pass — identical to the single-scheduler behaviour.
* **Asynchronous delivery.** These are requests carried out at the target
  scheduler's next drain point (reached when its ready queue empties — i.e. when
  it next goes idle or all its threads block). A momentarily-running target is
  back in a queue by then, so the operation still applies; but a target that
  spins without ever yielding to its scheduler will not observe the request
  until it does. This is the same cooperative-latency property all the
  cross-scheduler inbox operations share.

`test_suspend_mp.c` suspends a worker looping on scheduler 1 from scheduler 0,
asserts its progress counter freezes while suspended and advances again after
`pth_resume`, and exercises a cross-scheduler `pth_yield(target)` — under both
the select and poll backends.

With this every original Pth thread-control verb operates across schedulers; the
only remaining single-scheduler-bound facilities are process signals
(`PTH_EVENT_SIGS`/`pth_sigwait`, scheduler-0 only) and `pth_fork` (§15).

## 19. Correctness fixes from code review

A review of the phase-4 work surfaced three defects, since fixed (see
C<REVIEW.md> for the full write-up):

* **Async cancellation ran cleanup handlers in the wrong thread context.**
  `pth_cleanup_popall()` invokes a thread's cleanup handlers without making that
  thread current, so for *asynchronous* cancellation driven from the scheduler
  side the internal sync-primitive wait cleanups (which use `pth_current` to
  pick the waiter) removed the wrong entry -- leaving the cancelled thread
  stranded in a mutex/cond/rwlock wait queue (a later release then hit
  use-after-free), and `pth_mutex_releaseall()` failed to release a cancelled
  holder's mutexes (its `mx_owner == pth_current` check). Fix: `pth_thread_cleanup()`
  now sets `pth_current` to the target thread around the whole body. This was a
  latent bug introduced by the phase-2 conversion of the primitives to explicit
  wait queues; it affects single-scheduler async cancellation too.
* **`pth_abort()` mutated a foreign TCB directly.** It set `joinable` /
  `cancelstate` on the target before delegating to `pth_cancel()`; for a foreign
  target that raced the owning scheduler. Fix: a `PTH_GMSG_ABORT` inbox message
  routes the whole operation to the home scheduler (`pth_abort_local`).
* **TSD key-table reads were unlocked.** `pth_key_{set,get,destroy}data` read
  `pth_keytab[].used`/`.destructor` without the key-table spinlock, racing with
  `pth_key_create`/`pth_key_delete` on another scheduler. Fix: guard those reads;
  `destroydata` snapshots under the lock and runs the destructor outside it.

`test_cancelblock_mp.c` regression-tests the first two (and is confirmed to core
dump against a build with the cleanup-context fix reverted).

## 20. Follow-up review fixes

A second review round raised two more items:

* **`pth_once`/`pthread_once` were not MP-safe** (racy check-then-set: two
  schedulers could both run the initializer). Now an atomic state machine
  (0 uninit / 1 in-progress / 2 done) via `pth_atomic_cas`; the CAS winner runs
  the initializer once and publishes done, losers `pth_yield` until they observe
  done (no lock held across the user initializer). Test `test_once_mp.c` (40
  callers over 4 schedulers) confirms exactly-once and, against the old code,
  reproduces the multiple-init bug.
* **Message-port object lifetime is not library-managed.** The registry and per
  port operations are locked, but `pth_msgport_find` returns a raw pointer whose
  target another scheduler could `pth_msgport_destroy`. Rather than add
  reference counting to an API with no ownership model, the lifetime contract is
  documented (pth.pod): a port must outlive all threads that may use a reference
  to it; destruction is the application's responsibility to serialize.
  `test_msgport_mp.c` exercises the safe pattern.

## 21. Second follow-up review fixes

* **`pth_once`/`pthread_once` could hang if the initializer was cancelled.** The
  atomic once state machine (§20) left the control stuck at IN_PROGRESS if the
  initializing thread was cancelled or exited non-locally between states 1 and
  2, so waiters spun forever. Fixed by pushing a cleanup handler
  (`pth_once_cleanup`) around the initializer that resets 1 -> 0 on
  cancellation/non-completion (popped without executing on normal completion);
  a later caller then retries, per POSIX. Test `test_oncecancel_mp.c` (hangs
  against the un-fixed build).
* **Regenerated `pth.3`** from `pth.pod` so the shipped manpage carries the
  current API/contract text (including the message-port lifetime note).

## 22. Portable context switching: the "bctx" (Boost.Context) method

`ucontext(3)` (`makecontext`/`swapcontext`, the `mcsc` method that `PTH_MP`
required) is deprecated on macOS and unusable on Apple Silicon, so a portable
alternative was added: `bctx`, built on Boost.Context's low-level `fcontext`
API. The relevant assembly (`make_fcontext`/`jump_fcontext`) is vendored under
`fcontext/` (Boost Software License 1.0) for x86_64 and arm64, in both ELF
(Linux/FreeBSD) and Mach-O (macOS) formats; only two routines are used and they
carry hidden visibility so they neither export from a shared libpth nor clash
with an application's own Boost.Context.

`fcontext` differs from `ucontext` in two ways that the `bctx` method
(`pth_mctx.c`, `PTH_MCTX_MTH(bctx)`) handles:

* **Handle bookkeeping.** `fcontext` has no separate save/restore; a single
  `jump_fcontext(to, data)` both suspends the caller and resumes `to`. The
  invariant kept is *X->fc always holds the continuation to resume X*, rewritten
  by whoever X switches to at the moment they resume (the switcher passes its
  own mctx as the jump datum). A freshly `make_fcontext`'d context finds its own
  entry function through a per-OS-thread `pth_bctx_self` set immediately before
  the jump; `pth_mctx_restore` (used by `pth_uctx`) discards the abandoned
  continuation into a per-thread trash mctx.
* **Signal mask.** `fcontext` carries no signal mask, whereas `swapcontext`
  restores one. `bctx` therefore `sigprocmask`s the incoming context's mask on
  every switch (stored in a new `sysmask` TCB-context field), matching the
  `mcsc` semantics; auxiliary-scheduler adoption fills that mask (as the mcsc
  path did to `uc_sigmask`).

**Selection (CMake).** A new arch/format probe picks the right vendored asm;
`bctx` is the *default on macOS* and a fallback elsewhere, selectable anywhere
with `-DPTH_MCTX_MTH=bctx`, and is now accepted by the `PTH_MP` requirement
check alongside `mcsc`. The full MP test suite passes with `bctx` as the context
method on Linux/x86_64; macOS is exercised via CI. (The autoconf build still
selects `mcsc`/`sjlj` only; `bctx` is wired through the CMake build, which is the
supported build for this fork.)

## 23. Scalable fd readiness: the epoll(7) scheduler backend (opt-in, `PTH_SCHED_EPOLL`)

The `select(2)` and `poll(2)` scheduler cores (§12) both *rebuild and rescan the
entire descriptor set on every scheduler pass*: a scheduler parked on N idle
sockets pays an O(N) kernel scan each time it wakes for **any** reason. That is
the classic C10K ceiling. `PTH_SCHED_EPOLL` replaces the per-pass scan with a
persistent kernel interest set.

**What it is (persistent registration).** Each scheduler owns its own `epoll`
instance (created in `pth_scheduler_init`, closed in `pth_scheduler_kill`) plus
an fd-indexed registration cache. The event manager gains a third variant,
`pth_sched_eventmanager_evport`, generated from the `poll` variant so all of the
non-fd logic (signals, timers, the wakeup-inbox drain, the `loop_repeat`
machinery) is byte-identical; only the fd handling differs. Each pass:

* `pth_evp_begin` bumps a generation counter;
* assembly records the wanted `(fd, R/W/E)` mask via `pth_evp_want` (same O(n)
  walk of the waiting queue as before, but now purely in user space);
* `pth_evp_commit` diffs the wanted set against the cache and issues
  `epoll_ctl` **only for genuine changes** — `ADD` for newly-waited fds, `MOD`
  when the mask changed, `DEL` (a generation sweep over the registered list) for
  fds no longer waited on;
* `pth_evp_wait` calls `epoll_wait`, which returns **only the ready fds**
  (O(ready), not O(watched)); readiness is stamped with the current generation
  so the cleanup pass reads it with `pth_evp_ready`.

So the expensive kernel scan of all N descriptors every pass is gone. The O(n)
*user-space* walk of the waiting queue remains — that is the deliberate boundary
of this change (see "persistent registration" vs a full event-driven rewrite):
it keeps the proven event-manager structure and every event type working
unchanged, while removing the syscall cost that actually dominates at scale. It
is also the natural first step toward a fully event-driven loop later.

**Self-healing `epoll_ctl`.** Because Pth has no `close(2)` wrapper, a descriptor
a thread was waiting on can be closed (and its number reused) without the backend
being told. The commit path is therefore defensive: `MOD` returning `ENOENT`
(the kernel auto-dropped a closed fd) falls back to `ADD`; `ADD` returning
`EEXIST` (a stale registration we lost track of) falls back to `MOD`; an `ADD`
that fails outright (e.g. `EBADF`) marks the fd *ready* so the waiter's own I/O
call returns the real error instead of blocking forever.

**fd-reuse contract (limitation).** The one residual hazard is the same as every
persistent-poller (libev's `ev_io_stop`, libevent): if an application closes a
descriptor that a thread is *still waiting on* and immediately reuses the same
number, on the same scheduler, with the *same* wait direction, all within one
scheduler tick, a wakeup can be missed. Applications should not close descriptors
out from under waiting threads (already true in general; only epoll is sensitive
to reuse). The `select`/`poll` backends remain fully reuse-immune, so they stay
the default; `PTH_SCHED_EPOLL` is strictly opt-in. A future `pth_close()` that
evicts the fd from its scheduler's interest set would close this gap entirely.

**No API/ABI impact.** This is a build-time backend selection only:
`-DPTH_SCHED_EPOLL=ON` (Linux-only; it implies `PTH_SCHED_POLL` so the
descriptor-value fast paths in `pth_high.c`/`pth_util.c` are active and there is
no `FD_SETSIZE` ceiling). No public header, function, struct, or semantic
changes; `pth_read`/`pth_select`/`pth_poll`/`pth_connect`/`pth_accept` behave
identically. The dispatcher picks `evport` when `PTH_SCHED_EPOLL` is defined,
else `poll` when `PTH_SCHED_POLL`, else classic `select`.

**Testing.** The complete multi-scheduler suite passes against the epoll backend
(sync/join/cancel/raise/suspend/msgports/once/network I/O/philosophers), plus a
new `test_epoll_scale` that parks 120 readers across 3 schedulers simultaneously,
wakes each selectively with its own byte, and then repeats after closing and
reopening every socketpair (reusing the descriptor numbers) to drive the
self-healing `epoll_ctl` paths. `test_epoll_scale` is backend-agnostic and also
passes under `poll`/`select`. A `linux / MP + epoll + pthread` CI configuration
builds and runs it on every push.

**kqueue(2) backend (`PTH_SCHED_KQUEUE`, \*BSD/macOS).** The design paid off:
the whole event manager and the generic cache bookkeeping are backend-agnostic,
so kqueue reuses `evport` unchanged and supplies only its own `pth_evp_commit`
/ `pth_evp_wait` / `pth_evp_osfd` primitives. The one structural difference is
that kqueue registers each `(fd, filter)` separately (`EVFILT_READ`,
`EVFILT_WRITE`, and `EVFILT_EXCEPT` for the exception/`PTH_UNTIL_FD_EXCEPTION`
goal where available), rather than one combined mask per fd as epoll does, so
the commit diffs and adds/deletes individual filters (batched through the
`kevent` changelist); `EV_EOF` is mapped to both readable and writable so a
peer hangup wakes readers and writers alike. Bad-descriptor and stale-filter
errors are absorbed from the changelist's `EV_ERROR` results the same way epoll
self-heals. `PTH_SCHED_KQUEUE` is opt-in, \*BSD/macOS-only, implies
`PTH_SCHED_POLL`, and is mutually exclusive with `PTH_SCHED_EPOLL`. It is built
and the full suite (including `test_epoll_scale`) is run on every push via the
`macos / MP + kqueue + pthread` and `freebsd / MP + kqueue + pthread` CI
configurations. (Exception/out-of-band waits require `EVFILT_EXCEPT`, present on
macOS and FreeBSD 11+; read/write readiness -- the overwhelmingly common case --
needs only the universal `EVFILT_READ`/`EVFILT_WRITE`.)

**Defaults (as of the kqueue landing).** Once both backends were green in CI, the
scalable backend became the *platform default*: `PTH_SCHED_EPOLL` defaults ON on
Linux and `PTH_SCHED_KQUEUE` defaults ON on \*BSD/macOS (CMake selects by
`CMAKE_SYSTEM_NAME`). A build with no backend flags therefore gets the native
persistent-registration poller; `-DPTH_SCHED_EPOLL=OFF` / `-DPTH_SCHED_KQUEUE=OFF`
falls back to poll(2) (or select(2) with `PTH_SCHED_POLL=OFF` as well). The
poll/select/single-scheduler CI configurations pin the fallback explicitly so
they keep exercising those paths. This is still a build-time choice with no
public API/ABI change; the fd-reuse contract above now applies by default on
those platforms, which is why the reuse-immune poll/select paths are retained as
first-class, explicitly selectable fallbacks.
