# Task: One Loop to Wait — signalfd and eventfd

Build a program where a single `poll()` call is the only place the main
thread ever blocks. Signals arrive through it. Worker completions arrive
through it. Input arrives through it. Nothing sleeps, nothing polls on a
timer, and no signal handler ever runs.

This is the pattern lighttpd calls **fdevents** — an event loop over a set
of file descriptors — with the industrial parts stripped off. Linux lets
you turn the two famously non-fd things into fds:

- **signalfd** — a signal becomes readable bytes. You read a
  `struct signalfd_siginfo` instead of running a handler.
- **eventfd** — "hey, look over here" becomes readable bytes. It is how a
  thread wakes a poll loop. It is the fd-world replacement for
  `pthread_cond_broadcast`, *when the waiter is a loop, not a condvar*.

Both are Linux-only. You are on Linux. Build it as one file, `evloop.c`:

```
cc -std=c11 -Wall -Wextra -Werror evloop.c -pthread -o evloop
```

---

## Exact inventory

```
1 process

main thread — the loop
  poll() over:
    stdin      → job characters come in
    signalfd   → SIGINT, SIGTERM
    eventfd    → "a worker finished something"

2 worker threads — created once at startup, never created again
  pop a job, sleep 2–4 seconds (that's the work), write a line to
  stderr, then write to the eventfd
```

Three threads, one process. The workers may block on the queue's mutex and
condvar. The main thread may not — see the constraint below.

---

## What each part does

**The loop** reads characters off stdin, assigns ids 1, 2, 3… in acceptance
order, pushes jobs onto the queue, and keeps count of everything. When the
eventfd fires it collects finished jobs for accounting. When the signalfd
fires it starts shutdown. When stdin ends it starts a gentler shutdown.
That is the entire main thread.

**A worker** is a body you have already written once in your supervisor:
wait for a job, sleep, write the line to stderr, signal completion. The only
new sentence is *how* it signals completion — an `eventfd` write, because
the thing waiting on the other end is blocked in `poll()`, and a condvar
broadcast cannot reach a thread that is not waiting on a condvar.

---

## Protocol and queue

Every stdin character is one job — `'a'`, `'7'`, `'\n'`, all of them. There
is no quit command; input ends by EOF or by signal, never by a keyword.

The queue is shared memory between the loop and the 2 workers: an array of
capacity **4**, a mutex, a condvar. Bounded on purpose, small on purpose.

Two rules, and they are the exercise:

- **The queue's condvar is the workers' condvar, and it stays a condvar.**
  Workers waiting on empty are threads with shared memory and no fd in
  sight — right tool, right boundary. Do not eventfd-ify this wait. If you
  do, you have taken on lost-wakeup reasoning, counter semantics, and
  multi-reader consumption to solve a problem pthreads already solved for
  you.
- **The loop never blocks on push.** If the queue is full, the loop stops
  asking stdin for more — drop POLLIN interest on stdin until a slot
  frees. The kernel's pipe buffer becomes your staging area; the producer
  blocks in its `write()`. This is backpressure, and it is the fd-world
  answer to a bounded queue. If the loop blocks anywhere but `poll()`,
  the design is wrong, not merely slow.

---

## Output and accounting

- stderr — one line per finished job, written after the sleep, e.g.
  `done 3 'x'`. stderr is unbuffered: every line lands the moment it is
  written, so there is nothing to flush and nothing to lose.
- At exit, print one line to stderr: `accepted A, completed C, discarded D`,
  and `A = C + D` must hold. Completed jobs finished and were written
  out.
  Discarded jobs were accepted but queued-and-never-started at shutdown.

---

## Shutdown requirements

1. **EOF on stdin** — no more input ever. Every accepted job, including
   everything still queued, runs to completion. Then exit 0.
2. **SIGINT or SIGTERM** (read off the signalfd, in the same poll set) —
   stop reading input immediately. Jobs already started run to completion
   and are written out. Queued-but-unstarted jobs are discarded and
   counted. Exit 0.
3. **A second signal during the drain** — exit immediately with status 1.
   In-flight lines may be absent on this path; that is accepted here.
4. During any drain the loop stays blocked in `poll()` — still watching
   signalfd and eventfd — never spinning, never sleeping.

EOF and signal produce *different* drain modes. Notice which parts of your
code care about the difference and which don't.

---

## The constraint that makes this real

- The main thread may block **only** inside `poll()`. Not in `read`, not in
  a push, not in a sleep.
- No signal handlers anywhere, no `sigwait`, no second signal thread. The
  signalfd is the only path a signal takes into this program.
- No timeouts as a crutch: when idle, the process sits in one indefinite
  `poll()` and makes no syscalls at all. An acceptance test verifies this.
- Block SIGINT and SIGTERM **before creating the signalfd**, and think
  hard about what order things happen in at startup. The test for getting
  this wrong is loud.

---

## Acceptance tests

1. Idle, press Ctrl-C → exit 0, `accepted 0, completed 0, discarded 0`.
2. `printf 'a\nb\nc\n' | ./evloop` → EOF path: 6 `done` lines on
   stderr (every character, `\n` included), exit 0.
3. SIGTERM from another terminal while jobs are mid-flight → started jobs
   still complete and appear on stderr, accounting balances, exit 0.
4. Blast 1000 characters into a pipe instantly, signal mid-backlog →
   queue was full, stdin was ignored, pipe backed up; accounting still
   balances.
5. Second signal during drain → immediate exit 1.
6. While idle: `strace -p <pid>` → the process is sitting in
   `poll`/`ppoll` and doing nothing else. If you see a syscall loop, you
   are polling, not waiting.
7. Clean under valgrind.

Test 6 catches test cheating of every kind at once. Run it on the idle
program, during load, and during drain.

---

## Stretch goals

- Add a 10-second drain deadline with a **timerfd** — time becomes an fd
  too, and the deadline joins the same poll set. Still zero sleeps.
- Swap `poll` for `epoll` with edge-triggered eventfd. Work out what you
  now owe the kernel that level-triggered was forgiving about.
- Try `EFD_SEMAPHORE` and decide which semantics this program actually
  wants, and why the default is correct here.
- Fork a producer that feeds stdin. Send it a signal directly and explain
  what it did with the mask it inherited.

---

## Self-check

Answer without looking anything up. If you can't, some part was cargo-culted.

- Why must the signals be blocked before the signalfd can see them? What
  does Ctrl-C do if you forgot to block?
- Why block before `pthread_create` and not after?
- Two workers finish within a millisecond of each other. How many eventfd
  writes, how many reads, and what number did the read return? What does
  that number actually count?
- The queue kept its condvar; the completion wake became an eventfd. What
  property of the waiter decides which tool is right?
- SIGINT lands while a worker is mid-sleep and the loop is blocked in
  `poll()`. Trace the exact sequence from keyboard to the loop's next
  iteration.
- Two SIGINTs, 10 ms apart, at an idle loop. How many signalfd reads come
  out, and why? What does that imply about requirement 3's reliability?
- After stdin hits EOF you had to stop watching it. What does `poll` do on
  a pipe at EOF if you keep asking it for POLLIN, and what did your CPU
  meter do?
- Why should the signalfd be read until EAGAIN rather than once?
- In your supervisor, which thread handled the signal? Here, which? What
  could each safely touch from inside that position?
- The queue is full and you dropped stdin interest. Where did the next
  1000 lines physically live, and which process was blocked because of it?
