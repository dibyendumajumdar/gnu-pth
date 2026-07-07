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
