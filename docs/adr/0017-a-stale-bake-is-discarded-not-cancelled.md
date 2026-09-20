# ADR 0017: A stale bake is discarded, not cancelled — Steam Audio's cancel is unusable

**Status:** Accepted, 20 Sep 2026. Corrects ADR 0015.

## Context

ADR 0015 decided that a bake whose box the listener has left should be abandoned through
`iplPathBakerCancelBake`, rather than finishing and being thrown away. Chris's game then began
crashing while walking — three times, twice in a row — with no managed exception in
`client-crash.log` and no clean shutdown in the log: the process simply stopped, seconds after a
bake had completed. A stress test that walks a listener back and forth across a region
reproduces it within seconds.

Reading Steam Audio 4.8.1's source, cancelling is unsound twice over.

**The thread pool is never un-cancelled.** `ThreadPool::cancel()` sets `mCancel = true`, and
nothing ever clears it. Its workers wait on `mReady > 0 || mCancel || mQuit`, so once cancelled
they never wait again: they spin round the loop calling `mJobGraph->processNextJob(...)`. The
`JobGraph` belongs to the bake stage that has since returned and destroyed it, so the workers are
reading freed memory for the rest of the bake — and every later stage creates a new graph while
they are still on the old pointer.

**The pool itself is a dangling pointer.** `PathBaker::bake` keeps its `ThreadPool` on its own
stack and publishes it in a global raw pointer, `sThreadPool`, cleared at the end without
synchronisation. A cancel arriving from another thread as the bake returns writes `mCancel = true`
into a dead stack frame.

Either way the damage is silent memory corruption: the game dies later, somewhere unrelated, with
nothing in the logs that points at audio.

## Decision

- **Never call `iplPathBakerCancelBake`** — not from another thread, not from the progress
  callback, not at shutdown. The progress callback stays a no-op (it must still exist: ADR 0013).
- **A bake whose box the listener has left runs to the end, and its result is thrown away.** The
  `abandoned_` flag is only read after `iplPathBakerBake` returns. Discarded bakes go on being
  counted as `cancelled_bakes`.
- **Shutdown waits for the bake in flight**, which ADR 0015's probe budget keeps to about a
  second.

## Consequences

- A wasted bake costs its full ~500 ms of one thread rather than being cut short. This is a small
  price: in the walking measurement only 1 bake in 13 was discarded, because the box is left only
  every ~21 blocks while a bake takes half a second. **The probe budget, not cancellation, was
  what made walking affordable** — ADR 0015's other two decisions stand.
- `test_pathing_bake.cpp` keeps the case: a listener walked back and forth across a region
  abandons bake after bake, which crashed within seconds under the old code and is stable now.
  It is the test to run under the Linux sanitizer presets, where the corruption would be named.
- Quitting the world can wait up to about a second for a bake. If the budget is ever raised far,
  that wait grows with it.
- Worth reporting upstream. Until it is fixed, this is a landmine for any Steam Audio user who
  reads `iplPathBakerCancelBake` in the header and takes it at its word.
