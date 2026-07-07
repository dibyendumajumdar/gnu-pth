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
