# Review: M:N Threading Support

## Findings

### High: Async cancellation can leave stale wait-queue entries or crash

`pth_cancel_local()` runs `pth_thread_cleanup(thread)` from the scheduler side
(`pth_cancel.c`). `pth_cleanup_popall()` then executes the target thread's cleanup
handlers without making that target thread current (`pth_clean.c`).

The new internal cleanup handlers for mutexes, rwlocks, and condition variables
use `pth_current` to remove the waiter or reacquire locks (`pth_sync.c`). For
async cancellation of a blocked thread, `pth_current` is the scheduler's current
context, not necessarily the cancelled thread. That can remove the wrong TCB,
derefence `NULL`, or leave a freed TCB in a primitive wait queue. A later wakeup
can then hit use-after-free.

The cleanup path should pass the target TCB explicitly to internal wait cleanup
handlers, or async cleanup should execute with the cancelled thread as the
effective current thread.

### High: Cross-scheduler `pth_abort()` mutates foreign TCB state directly

`pth_abort()` reads and writes `thread->state`, `thread->joinable`, and
`thread->cancelstate` directly before delegating to `pth_cancel()` (`pth_cancel.c`).
For a foreign target this bypasses the design invariant that only a thread's home
scheduler mutates its TCB and run queues.

This races with the target scheduler and the target thread itself. The force-abort
operation should be routed to the target scheduler as a dedicated inbox message,
or otherwise handled entirely by the target's home scheduler.

### Medium: TSD key table is only partially synchronized

`pth_key_create()` and `pth_key_delete()` guard `pth_keytab` with
`pth_keytab_lock`, but `pth_key_setdata()`, `pth_key_getdata()`, and
`pth_key_destroydata()` read `pth_keytab[key].used` and destructor entries without
holding that lock (`pth_data.c`).

In an MP build, another scheduler can delete or recreate a key concurrently, so
these unlocked reads are data races and can produce inconsistent key validity or
destructor behavior. The key metadata needs consistent locking or atomic access
with a clear lifetime protocol for destructors.

## Verification

I attempted to configure an MP CMake build on the local Windows environment:

```sh
cmake -S . -B build-review -DPTH_MP=ON -DPTH_SCHED_POLL=ON -DPTH_BUILD_TESTS=ON
```

Configuration failed because no C compiler is configured in this environment. I
removed the generated `build-review` directory afterward. A Linux build would be
useful to compile and run both scheduler backends:

```sh
cmake -S . -B build-mp-poll -DPTH_MP=ON -DPTH_SCHED_POLL=ON -DPTH_BUILD_TESTS=ON -DPTH_BUILD_PTHREAD=ON
cmake --build build-mp-poll
ctest --test-dir build-mp-poll --output-on-failure

cmake -S . -B build-mp-select -DPTH_MP=ON -DPTH_SCHED_POLL=OFF -DPTH_BUILD_TESTS=ON -DPTH_BUILD_PTHREAD=ON
cmake --build build-mp-select
ctest --test-dir build-mp-select --output-on-failure
```

---

## Resolution (all three findings addressed)

### High #1 — async cancellation cleanup context: FIXED
`pth_thread_cleanup()` (`pth_lib.c`) now runs the target thread's cleanup
handlers, TSD destructors and `pth_mutex_releaseall()` with `pth_current`
temporarily set to the target thread (saved/restored around the body; no
context switch occurs inside). This makes the internal mutex/rwlock/cond wait
cleanups remove the correct waiter and lets `pth_mutex_releaseall()` (which
checks `mx_owner == pth_current`) actually release a cancelled thread's held
mutexes. It is also the correct POSIX context for handlers/destructors, and is
a no-op on the normal `pth_exit()`/`pth_kill()` paths where `pth_current`
already equals the target.

Confirmed with `test_cancelblock_mp.c`: a *detached* thread blocked on a
cross-scheduler mutex is async-cancelled and the mutex then released — this
**dumps core without the fix** (use-after-free on the stale wait-queue entry)
and passes with it; a cancelled mutex *holder* now correctly releases its mutex.

### High #2 — cross-scheduler `pth_abort()`: FIXED
`pth_abort()` (`pth_cancel.c`) no longer writes a foreign TCB's `joinable` /
`cancelstate`. For a foreign target it posts a new `PTH_GMSG_ABORT` inbox
message; the target's home scheduler runs `pth_abort_local()`, which reaps the
thread if already dead or force-detaches and async-cancels it otherwise — all on
the owning scheduler. Same-scheduler behaviour is unchanged. Covered by
`test_cancelblock_mp.c` case C (a clean `pth_mp_shutdown` proves the aborted
thread's scheduler ran dry).

### Medium #3 — TSD key table synchronization: FIXED
`pth_key_setdata()`, `pth_key_getdata()` and `pth_key_destroydata()`
(`pth_data.c`) now take `pth_keytab_lock` when reading `pth_keytab[].used` /
`.destructor`. `pth_key_destroydata()` snapshots the validity flag and
destructor pointer under the lock and invokes the user destructor *outside* the
lock (destructors must not run under a spinlock). Per-thread `data_value`
storage is unshared and needs no locking.

### Verification performed (Linux)
Both scheduler backends and single-scheduler builds were compiled and run:
plain `PTH_MP`, `PTH_MP + PTH_SCHED_POLL`, and the default single-scheduler
build. The full MP test suite (`test_mpsched`, `test_join_mp`,
`test_msgport_mp`, `test_cancelraise_mp`, `test_suspend_mp`, `test_mutexfair_mp`,
`test_cancelblock_mp`) plus `test_std` / `test_sync` all pass; the new
`test_cancelblock_mp` was also confirmed to fail (core dump) against a build
with fix #1 reverted, demonstrating it actually catches the regression.

---

## Follow-up Review: Open Findings

### High: `pth_once()` / `pthread_once()` are not MP-safe

`pth_once()` (`pth_lib.c`) and `pthread_once()` (`pthread.c`) still use the
classic unsynchronized check-then-set implementation on an integer once control.
Under M:N scheduling, two OS-backed schedulers can concurrently observe the
control value as unset and both run the initializer. This is both a data race
and a violation of the expected once-only semantics.

This likely needs an atomic state machine such as UNINIT / IN_PROGRESS / DONE,
where the winning scheduler runs the initializer and other schedulers wait or
yield until the state reaches DONE. Holding a spinlock while executing the user
initializer would be unsafe because the initializer can call back into Pth or
block cooperatively.

### Medium: named message-port lookup has a cross-scheduler lifetime race

The message-port registry ring is now protected while inserting, removing and
searching (`pth_msg.c`), but `pth_msgport_find()` returns a raw `pth_msgport_t`
after dropping `pth_msgport_lock`. Another scheduler can then call
`pth_msgport_destroy()` and free that port before the finder uses it in
`pth_msgport_put()`, `pth_msgport_pending()` or `pth_msgport_get()`.

The ring lock fixes traversal corruption, but it does not protect the lifetime
of the returned port object. This needs either a reference/lifetime protocol,
deferred destruction on the owning scheduler, or clearly documented external
lifetime rules with tests that cover cross-scheduler find/destroy races.

### Follow-up Verification Performed

On the local Linux build directory, `cmake --build build -j2` completed
successfully. `ctest --test-dir build --output-on-failure` passed for the
registered tests in that build (`test_std`, `test_uctx`, `test_sync`,
`test_mpsched`). The MP regression binaries were also run directly and passed:
`test_join_mp`, `test_cancelblock_mp`, `test_cancelraise_mp`,
`test_suspend_mp`, `test_msgport_mp`, and `test_mutexfair_mp`.


---

## Follow-up Resolution

### High — `pth_once()` / `pthread_once()` not MP-safe: FIXED
Both now drive the control word as an atomic state machine (0 = uninitialized,
which is C<PTH_ONCE_INIT>/`PTHREAD_ONCE_INIT`; 1 = in progress; 2 = done) using
the existing `pth_atomic_cas`/`pth_atomic_load`/`pth_atomic_store` (real atomics
under `PTH_MP`, trivial otherwise). The winner of the `0 -> 1` CAS runs the
initializer once and publishes `2`; other callers spin cooperatively
(`pth_yield`) until they observe `2`, so the initializer has completed on
return (POSIX semantics). No lock is held across the user initializer, which may
call back into Pth or block.

New `test_once_mp.c`: 40 callers spread over 4 schedulers race on one control
with a deliberately widened initializer window. With the fix the constructor
runs exactly once (`constructor_runs=1`); against the old check-then-set it runs
up to 40 times — so the test demonstrably catches the regression.

### Medium — named message-port lookup lifetime race: DOCUMENTED (by design)
The registry ring traversal and each port operation are internally synchronized
(finding from the first review), but the I<lifetime> of a port object cannot be
managed by the library without adding reference counting to an API that has no
ownership/`release` model -- which would be a semantic API change out of scope
here. Per the review's third accepted option, the lifetime contract is now
documented explicitly: a port must not be destroyed while another thread still
holds and may use a reference obtained from `pth_msgport_create`/`_find`;
destruction is the application's responsibility to serialize (typically the
owning/creator thread destroys it only after establishing, via join/barrier,
that no other thread will touch it). See the lifetime notes added to
`pth_msgport_destroy(3)`/`pth_msgport_find(3)` in pth.pod. `test_msgport_mp.c`
already exercises the safe cross-scheduler pattern (owner keeps the port alive
across all senders, destroys only after they finish).

---

## Second Follow-up Review: Open Findings

### High: `pthread_once()` can hang forever if `init_routine()` is cancelled

`pthread_once()` (`pthread.c`) now changes the once control from `0` to `1`
before calling `init_routine()`, and only publishes `2` after the routine
returns. If the routine reaches a cancellation point and is cancelled, the
control word remains stuck at `1`; later callers spin/yield forever waiting for
state `2`.

This regresses the documented POSIX behavior in `pthread.pod`: cancellation
inside `init_routine()` should leave the control as if `pthread_once()` was
never called. The same stuck-in-progress risk exists for `pth_once()` if its
constructor exits non-locally via Pth cancellation/exit. A cleanup handler
around the initializer should reset `1 -> 0` on cancellation/non-completion,
while normal completion publishes `2`.

### Low: checked-in `pth.3` has stale message-port lifetime documentation

The new message-port lifetime contract is present in `pth.pod`, but the
checked-in `pth.3` still has the old short `pth_msgport_destroy()` /
`pth_msgport_find()` text. Since `pth.3` is present in the tree and install
paths reference generated manpages, this can ship stale documentation unless
the manpage is regenerated.

### Second Follow-up Verification Performed

A fresh default CMake configure in `/tmp/gnu-pth-default-review` confirmed the
new defaults: `PTH_MP=ON` and `PTH_SCHED_POLL=ON`. The fresh build completed
successfully and `ctest --test-dir /tmp/gnu-pth-default-review
--output-on-failure` passed all 13 registered tests, including the MP and
poll-related tests: `test_mpsched`, `test_mutexfair_mp`, `test_join_mp`,
`test_msgport_mp`, `test_cancelraise_mp`, `test_suspend_mp`,
`test_cancelblock_mp`, and `test_once_mp`.


---

## Second Follow-up Resolution

### High — once hangs if the initializer is cancelled: FIXED
`pth_once()` and `pthread_once()` now push a cleanup handler
(`pth_once_cleanup`) around the initializer that, on cancellation or non-local
exit, resets the control word from IN_PROGRESS (1) back to UNINITIALIZED (0);
on normal completion the handler is popped without executing and the control is
published as DONE (2). A caller cancelled inside the initializer therefore
leaves the control as if `pth_once()` was never called (POSIX behaviour), and a
later caller re-runs and completes it instead of spinning forever on state 1.

New `test_oncecancel_mp.c`: thread A wins the once race and blocks inside the
initializer; thread B waits; A is cancelled; B then re-runs the initializer to
completion and returns. With the fix the initializer starts twice (aborted +
retried), completes once, and B returns; **without the fix the test hangs**
(B never observes DONE) -- confirmed by reverting the cleanup and observing a
timeout.

### Low — stale `pth.3`: FIXED
`pth.3` was regenerated from `pth.pod` via the project's `make pth.3` rule
(pod2man + the `PTH_VERSION_STR` substitution), so the message-port lifetime
notes are now present in the shipped manpage.

### Second Follow-up Verification
Rebuilt and ran under plain `PTH_MP`, `PTH_MP + PTH_SCHED_POLL` (the new
defaults), the pthread emulation, and the default single-scheduler build. All MP
tests pass, including the two new `test_once_mp` and `test_oncecancel_mp`; the
latter was confirmed to hang against a build with the cleanup fix reverted.


---

## Third Follow-up Review: Open Findings

Scope: updates after the second follow-up, especially `MULTISCHED-DESIGN.md`
sections covering Boost.Context `bctx`, the persistent epoll/kqueue scheduler
backends, and the new CMake backend defaults.

### High: persistent fd backend can strand waiters after close-while-waiting

The epoll backend keeps a userspace registration cache (`registered`,
`regmask`) in `pth_sched.c`. If an application closes a descriptor while a
thread is still waiting on it, Linux removes that descriptor from the epoll
interest set, but the cache can still say it is registered. On the next event
manager pass, if the same fd/mask is still wanted, `pth_evp_ctl()` does not call
`epoll_ctl()` at all because `r->registered` is true and `r->regmask ==
r->want`. The scheduler can then block in `epoll_wait()` with no kernel
registration for the waiter, so the thread may never be woken.

This is related to the fd-reuse limitation documented in `MULTISCHED-DESIGN.md`,
but it is broader than same-tick fd reuse: plain close-while-waiting is enough
to invalidate the kernel registration while leaving the userspace cache stale.
The backend needs either a reliable eviction path (for example a `pth_close()`
wrapper used by Pth-managed fds), a conservative validation/re-add strategy, or
a clearly documented hard restriction that descriptors must not be closed while
any Pth thread may still be waiting on them, with regression coverage.

### Medium: evport bad-fd handling regresses `PTH_EVENT_SELECT` failure semantics

The poll backend maps `POLLNVAL` to `PTH_STATUS_FAILED`, so a `pth_select()` wait
on an invalid descriptor fails instead of becoming ready. The epoll/kqueue
evport path does not carry a failure bit into the cleanup loop: `pth_evp_ready()`
returns only readiness bits, and the cleanup for `PTH_EVENT_FD` /
`PTH_EVENT_SELECT` only marks events occurred when those readiness bits match.

In the epoll `ADD` failure path, bad descriptors are deliberately reported as
ready so higher-level I/O wrappers can make the real syscall and return the real
error. That is fine for `pth_read()` / `pth_write()` style waits, but it is wrong
for `PTH_EVENT_SELECT`: there is no follow-up syscall that can report `EBADF`,
so invalid descriptors can be reported as ready instead of failed. The evport
backend should preserve an explicit failed state for bad fd registrations and
map that to `PTH_STATUS_FAILED` for `PTH_EVENT_SELECT`, matching the select/poll
backends.

### Low: backend default documentation is internally stale

`MULTISCHED-DESIGN.md` still titles the epoll section as "opt-in,
`PTH_SCHED_EPOLL`", but the later defaults text and current CMake logic make
epoll the default backend on Linux and kqueue the default backend on
BSD/macOS. This is not a runtime issue, but it makes the design document
internally inconsistent for reviewers and users trying to understand the
default behavior.

### Third Follow-up Verification Performed

A fresh Linux CMake configure in `/tmp/gnu-pth-review-current` with
`PTH_BUILD_TESTS=ON` and `PTH_BUILD_PTHREAD=ON` selected the new default epoll
backend and built successfully. In that environment, the registered suite
passed through the existing MP tests, `test_epoll_scale` passed, and
`test_philo_mp` passed. My sandboxed run timed out in `test_netio_mp`; this was
not treated as a confirmed finding and appears to be an artifact of the
restricted execution sandbox, because a standalone run outside that sandbox
completed successfully in 0.017s:

```
part1 socketpair streams: 4 pairs x 262144 bytes (write/send + read/select), OK
part2 connect/accept (cross-scheduler): OK
schedulers: 4
ALL NETIO-MP TESTS PASSED
```


---

## Fourth Follow-up Review: Boost.Context `bctx` Open Findings

Scope: the new Boost.Context/fcontext machine-context method (`PTH_MCTX_MTH=bctx`),
including CMake selection, vendored assembly integration, context-switch
semantics, and MP compatibility.

### Medium: `bctx` does not preserve `errno` across context switches

The existing machine-context contract treats `errno` as part of the saved
context: the `mcsc` and `sjlj` paths save `mctx->error = errno` before switching
and restore `errno = mctx->error` when entering a context. The `bctx` path does
not currently do the equivalent. `pth_mctx_switch_bctx()` swaps the signal mask
and fcontext handle, but does not save the outgoing context's `errno` or restore
the incoming context's saved value. `pth_mctx_restore_bctx()` likewise jumps
into the target context without restoring `mctx->error`.

This can leak `errno` between Pth threads or standalone `pth_uctx` contexts,
especially after scheduler activity or a non-returning `pth_uctx` restore. The
fix should mirror the other methods: save `old->error = errno` before
`jump_fcontext()`, restore the resumed context's saved `errno` when control
returns, and set `errno = mctx->error` in the non-returning restore path before
jumping.

### Low: CMake cache help does not list `bctx` as a supported mctx method

`PTH_MCTX_MTH` is now selectable as `bctx` and is the default on macOS when
vendored fcontext assembly is available, but the CMake cache help still
describes the option as only `mcsc, sjlj`. This is a documentation/discovery
issue rather than a runtime defect, but it makes the new supported path harder
to find and can mislead users trying to force the context method.

### Fourth Follow-up Verification Performed

A forced Linux `bctx` build was configured with:

```
cmake -S . -B /tmp/gnu-pth-review-bctx \
  -DPTH_MCTX_MTH=bctx \
  -DPTH_BUILD_TESTS=ON \
  -DPTH_BUILD_PTHREAD=ON \
  -DPTH_SCHED_EPOLL=OFF \
  -DPTH_SCHED_POLL=ON
```

The build succeeded, including both `libpth` and the pthread emulation, and
selected the vendored x86_64 ELF fcontext assembly. The registered test set
excluding the sandbox-sensitive `test_netio_mp` passed: 17/17 tests, including
the MP scheduler, join, message-port, cancel/raise, suspend, once, epoll-scale,
pthread-emulation, and bounded philosopher tests.

---

## Third Follow-up Resolution (epoll/kqueue backends)

### High — persistent fd backend can strand waiters after close-while-waiting: FIXED (bounded safety net)
The persistent-registration backends cannot see a *silent* kernel deregistration
(the kernel drops a closed fd from the epoll/kqueue interest set without an
event), so a descriptor closed while a thread is still waiting on it could
strand that waiter. This is the same hazard every persistent poller has
(libev/libevent require unregister-before-close). Rather than leave the now-
default backend able to hang, a **bounded revalidation heartbeat** was added
(`pth_sched.c`):

* a scheduler never blocks in `epoll_wait()`/`kevent()` longer than
  `PTH_SCHED_REVAL_MS` (env, default 1000 ms; `0` disables for pure persistent
  behaviour), so an otherwise-idle scheduler still wakes periodically;
* once per interval an internal *force* flag makes the next commit re-assert
  **every** current registration instead of skipping unchanged ones;
* a descriptor that vanished fails the re-assert (`epoll_ctl` `EBADF` / kqueue
  `EV_ERROR`); the stale cache entry is dropped and the waiter is woken with
  `PTH_STATUS_FAILED` (`EBADF`) — the select/poll outcome, bounded to at most
  one interval of latency instead of a permanent strand.

Steady-state cost is one idle wakeup per interval per scheduler plus an
`O(registered)` re-assert once per interval; ordinary passes still skip
unchanged registrations, so scalability is preserved. New `test_closewait.c`: a
thread blocks in `pth_read()` on a socketpair end which is then closed out from
under it. **With `PTH_SCHED_REVAL_MS=0` this strands (harness timeout); with the
default it wakes with `EBADF`** — confirmed both ways, and it passes immediately
on the close-safe `select`/`poll` backends. The design doc (§23) documents the
contract, the safety net, and the knob; select/poll remain first-class,
explicitly selectable, close-safe fallbacks. (A fully *immediate* fix would
still need a `close(2)` interception / `pth_close()`; noted as possible future
work on top of the heartbeat.)

### Medium — evport bad-fd regresses PTH_EVENT_SELECT failure semantics: FIXED
The evport cleanup now carries an explicit failure bit (`PTH_EVP_FAIL`).
Registration failure for a bad descriptor (epoll `EPOLL_CTL_ADD` error, kqueue
`EV_ERROR`) flags the fd failed rather than ready, and the cleanup maps it to
`PTH_STATUS_FAILED` for both `PTH_EVENT_FD` and `PTH_EVENT_SELECT` — matching the
`select`/`poll` `POLLNVAL` behaviour (a `pth_select()` on an invalid descriptor
now fails instead of being reported ready, with no follow-up syscall needed).
New `test_badfd.c` (`pth_select()` on a closed descriptor) returns `EBADF` on
all four backends (select, poll, epoll, kqueue).

### Low — backend default documentation internally stale: FIXED
§23's title and body now describe epoll/kqueue as the *platform defaults*
(epoll on Linux, kqueue on \*BSD/macOS) rather than "opt-in", consistent with the
CMake logic and the defaults note.

### Third Follow-up Verification (Linux)
Rebuilt the default epoll backend plus poll and select. The full MP suite
(`test_mpsched`, `test_join_mp`, `test_msgport_mp`, `test_cancelraise_mp`,
`test_suspend_mp`, `test_cancelblock_mp`, `test_once_mp`, `test_netio_mp`,
`test_epoll_scale`, `test_philo mp`) plus the two new `test_badfd` and
`test_closewait` pass on epoll; `test_closewait` was confirmed to strand
(timeout) with the safety net disabled. No regression on poll/select/single.
kqueue paths are syntax-checked locally (fake `<sys/event.h>`, with and without
`EVFILT_EXCEPT`) and built/run via the FreeBSD/macOS CI.

---

## Fourth Follow-up Resolution (Boost.Context bctx)

### Medium — bctx does not preserve errno across context switches: FIXED
`pth_mctx_switch_bctx()` now saves `old->error = errno` before `jump_fcontext()`
and restores `errno = old->error` when the context is resumed;
`pth_mctx_restore_bctx()` sets `errno = mctx->error` before its non-returning
jump; and the fresh-context trampoline (`pth_bctx_entry`) installs
`errno = self->error` before entering the thread body. This mirrors the mcsc/sjlj
`mctx->error` save/restore contract, so `errno` no longer leaks between Pth
threads or `pth_uctx` contexts under the bctx method. Verified with a forced
bctx build (vendored x86_64 ELF fcontext asm) running the MP suite +
`test_netio_mp` + `test_epoll_scale` + `test_badfd`.

### Low — CMake cache help omits bctx: FIXED
The `PTH_MCTX_MTH` cache help now reads `"Force mctx method (mcsc, sjlj, bctx)"`.
