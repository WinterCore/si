# Task: Two-Tier Job Supervisor with Graceful Shutdown

Build a program in C that shuts down cleanly under every kill path, using both child processes and threads.

The work the program does is deliberately trivial. The shutdown is the
assignment.

---

## Exact inventory

```
supervisor          1 process,  1 thread
  │
  ├─ pipe 0 ──▶  worker 0       1 process,  5 threads
  ├─ pipe 1 ──▶  worker 1       1 process,  5 threads
  └─ pipe 2 ──▶  worker 2       1 process,  5 threads

                     total:  4 processes, 16 threads
```

Each forked worker creates its own 4 processing threads. They are not shared,
and the supervisor does not create any of them. Three separate boxes, each with its own runners inside.

Each worker's 5 threads:

- **1 reader** — the worker's main thread, the one that exists right after the fork. Reads job ids off its pipe and pushes them onto the queue. This thread is the sole owner of the pipe.
- **4 processors** — created with `pthread_create`. Pull from the queue and do the work. They never touch the pipe.

---

## What each tier does

**The supervisor (parent process)**

- Creates 3 pipes, then forks 3 worker processes.
- Generates jobs and dispatches them down the pipes.
- Runs until signaled. It has no natural end condition.

**Each worker (child process)**

- Reader thread moves ids from the pipe into a bounded queue.
- 4 processor threads take from the queue and do the work.
- Processing a job takes 2–5 seconds. A sleep is a fine stand-in.
- On completion, append one line to that worker's own output file.

---

## Job protocol

**A job is a single `int`.** No struct, no serialization, no text. The id exists only so you can count things at the end.

**Topology: one pipe per worker.** Create each pipe before forking that worker. Parent holds the write end, worker holds the read end. Three workers, three pipes. A single shared pipe is the tempting simplification — it is not the assignment, and it hides the thing you're here to learn.

**Dispatch**

- Job ids start at 1 and increase by 1. Never reused.
- One job every 300ms, assigned round-robin: worker 0, worker 1, worker 2, worker 0, and so on.
- The id is written to that worker's pipe as raw bytes.
- **How many jobs: unbounded.** The parent generates forever and stops only because it was signaled. This is deliberate — a program that ends on its own doesn't test shutdown. The final count is whatever it reached, and that's the number your accounting balances against.
- At 300ms per dispatch versus 2–5 seconds per job of work, the parent
  outruns the workers on purpose. Queues fill and there is always something in flight when you kill it.

**Routing, and why the two tiers differ**

The parent must explicitly choose a worker, because separate processes share no memory — it has no way to hand a job to "whichever worker is free", so it picks a pipe and commits.

Inside a worker, nothing chooses. All 4 processors wait on the same queue and whichever is free takes the next item. A thread busy on a 5-second job simply isn't at the queue, so it stops receiving work. Expect uneven per-thread counts; that is correct behavior. Which thread wakes is the kernel's choice — don't try to control it.

**Guarantees your implementation must provide**

1. A dispatched id is never silently lost. At shutdown every id must be accountable as exactly one of: **completed**, **queued but never started**, or **still in the pipe, never read**. The last two are fine to discard — but you must be able to say how many, not shrug.
2. No id is ever read as a partial or corrupted value.
3. No id is processed twice, including during shutdown.

**Explicitly out of scope** — retry logic, job priorities, dynamic worker counts, work stealing between workers, persisting the queue across restarts. None of these are the lesson.

---

## The queue

One queue per worker **process**, shared by that worker's threads — the reader pushes, the 4 processors pop. Three queues exist in the whole program, one per box. Worker 0's queue is invisible to worker 1 in every sense.

It is ordinary memory, not a kernel object. An array plus a mutex plus
condition variables is the entire thing.

**Capacity: 8, and small is the point.** With 4 threads each holding a job for 2–5 seconds and one arriving every 900ms, a capacity of 8 fills within seconds and the reader thread starts blocking.
Make it 1000 and the reader never blocks once during a test run — you would never encounter the case this exercise is built around, and you'd wrongly conclude your shutdown worked.

Do not build a ring buffer. At 8 elements, shifting the array on pop costs nothing, and wraparound arithmetic is an hour of debugging that teaches you nothing about shutdown.

A mutex alone is insufficient. It gives exclusive access but not waiting — with only a mutex, an idle processor has to lock, look, find nothing, unlock, and retry, burning a core to discover nothing changed. Whether you need one condition variable or two is worth working out yourself: the question is who you wake and whether they can actually proceed.

Note that the reader and the processors block on *opposite* conditions — full versus empty. At shutdown that may mean they need different treatment.

---

## Output files

**Four files, all in the working directory.**

- `dispatch.log` — written by the parent, one line per job sent, recording the id and which worker received it.
- `worker-0.log`, `worker-1.log`, `worker-2.log` — one per worker process, one line per **completed** job, recording the worker's pid and the job id.

Rules:

- A line is written only *after* the work finishes. Never on receipt.
- **The line must be on disk before the process exits.** This is why the exercise uses files rather than stdout: the file is evidence that outlives the process. If you buffer and exit the wrong way, the line simply isn't there, and the test fails loudly instead of quietly passing.
- No shared output file. Three processes appending to one file is a locking exercise, not this one.

Verification is then line counting: total lines in `dispatch.log` versus total lines across the three worker logs. Completed must be less than or equal to dispatched, and every completed id must appear in `dispatch.log` assigned to the same worker whose file logged it.

That last clause is the only reason the parent records the worker number and not just the id. Without it a missing job tells you nothing — you can't say whether it died in worker 1's pipe or worker 2's queue, and a job processed by the wrong worker is completely invisible. If you finish and find the attribution never earned its keep, drop it.

---

## Shutdown requirements

1. On SIGTERM or SIGINT, **no job that has already started may be abandoned.** Every in-flight job runs to completion and its line reaches disk.
2. No new jobs are accepted or dispatched after the signal arrives.
3. Every worker process must be fully gone before the parent exits, and the parent prints each worker's exit status.
4. The parent exits with status 0 on a clean shutdown.
5. Shutdown completes within 10 seconds. If a worker overruns that, the parent escalates rather than hanging.

---

## The constraint that makes this real

**At the moment the signal arrives, at least one thread in each worker must be
blocked in an indefinite blocking call** — no timeout, no polling loop.

If every thread is a loop that does a little work and then sleeps 100ms, none of the interesting problems appear and you will learn nothing. Pick real blocking waits and make shutdown work around them.

Likewise, the parent may not sleep for a few seconds and then assume the
workers are done. It has to actually know.

---

## Acceptance tests

Done when all of these pass.

1. **Ctrl-C** in the foreground → clean shutdown, exit 0.
2. **SIGTERM to the parent pid** from another terminal → *identical* result to test 1. Same output files, same exit code, same timing.
3. **DEFERRED** — ~~SIGTERM to one worker pid directly~~. Re-add later together with the parent detecting children that die on their own.
4. **Two signals in rapid succession** → no crash, no double-free, no hang, no duplicated output lines.
5. **DEFERRED** — ~~SIGKILL to one worker~~. Same later batch as test 3.
6. **After exit:** no orphaned processes, no zombies. Verify with `ps` — nothing of yours should survive.
7. **Job accounting balances**, per the guarantees above.
8. **Clean under a thread sanitizer**, and clean under valgrind for leaks.

Tests 1 and 2 producing identical results is the one most people get wrong.
Understand *why* they differ by default before you make them match.

---

## Stretch goals

- SIGHUP triggers a config reload instead of a shutdown, with the shutdown path still working afterward.
- Run it as PID 1 inside a container. `docker stop` must exit cleanly well within the default grace period.
- Replace the sleep-based work with a real blocking socket read, and make shutdown still interrupt it correctly.
- A configurable drain timeout for the escalation deadline.
- **Hard mode:** shrink the pipe buffer to one page and raise the dispatch rate until the parent itself blocks writing to a full pipe. Now the supervisor is stuck in a blocking call when the signal lands.

---

## Self-check

When you're finished you should be able to answer these without looking anything up. If you can't, some part of it was cargo-culted.

- Which thread ran your signal handler, and why couldn't you predict it?
- Why can't you call `printf` from inside a handler? What breaks, concretely?
- What woke the thread that was blocked indefinitely, and what would have
  happened if you hadn't done that?
- Under test 1, how many times did each worker get signaled? Under test 2?
- What exactly did your parent wait on, and how did it know a worker was done?
- What happens if a signal arrives *during* shutdown?
- Why is `volatile` sufficient here, or why isn't it?
- What happened to the thread blocked on the *pipe*, versus the threads blocked on the *queue*? Same mechanism, or two different ones?
- If the parent had exited without closing its write ends, what would each worker's reader thread have done?
- Why did the queue bound of 8 make this harder than an unbounded queue would have?
