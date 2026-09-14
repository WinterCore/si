# Graceful Shutdown Review

The ordinary single-signal drain path is mostly present: the parent closes the worker pipes, active jobs finish, processor threads join, and the parent waits for the worker processes.

The remaining correctness and graceful-shutdown problems are:

- **Queue corruption:** The queue stores `int64_t` values, but the queue shift copies `job_count * sizeof(int)` bytes. When several jobs are queued, job IDs can be duplicated or lost.
- **Post-signal dispatch race:** A signal can arrive after the parent checks the shutdown descriptor but before it writes the next job. One job can therefore be dispatched after the signal.
- **Unbounded shutdown:** There is no 10-second shutdown enforcement. Workers drain the entire pipe and queue before exiting, and the parent can wait indefinitely.
- **In-flight jobs abandoned by a second signal:** A second signal immediately sends `SIGKILL` to every worker, including workers processing jobs that have already started.
- **Incomplete job accounting:** Jobs abandoned by the second-signal path are not completed, queued but unstarted, or unread in the pipe, so they fall outside the required accounting categories.
- **Worker failures are reported as success:** The parent discards every child's wait status and returns status 0 even when workers were killed or exited unsuccessfully. The printed worker number is based on loop order rather than the process that was actually reaped.
- **Parent shutdown thread remains active:** After the first signal, the shutdown thread stays blocked in `sigwait`. The parent exits without joining or otherwise completing that thread.
- **Unreliable dispatch-log formatting:** `dispatch.log` formats an `int64_t` job ID using an incompatible format specifier. This is undefined behavior and makes the accounting record formally unreliable.
- **Queue-full shutdown path is not exercised:** With the current dispatch rate, processing duration, and processor count, workers generally have enough processing capacity to prevent their queues from filling. The shutdown behavior while the reader is blocked on a full queue is therefore not genuinely exercised.
