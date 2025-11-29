# Project Review

## Overview
The codebase provides a small supervisor (`forker.c`) that parses INI-like configuration files, expands placeholders, runs init hooks, launches multiple child processes (optionally wrapped by `sse_tail`), and tears everything down when any child exits or on termination signals.

## Suggested Improvements
1. **Add shutdown escalation for stubborn children.** `shutdown_all` sends `SIGTERM` to running children and then blocks on `waitpid` without ever escalating to `SIGKILL`. If a child refuses to exit, the supervisor will hang indefinitely and skip cleanup hooks. Adding a timed second pass that escalates to `SIGKILL` after a grace period would prevent shutdown deadlocks and guarantee cleanup runs.
2. **Handle `fork` failures after partial startup.** `start_children` calls `die` immediately when a `fork` fails, which exits the supervisor without signalling any instances that were already started. That can leave orphaned children and skip cleanup hooks. Tracking how many processes have been spawned and reusing the existing `shutdown_all` path on errors would allow a clean rollback when one spawn fails.
3. **Detect duplicate instance names in config parsing.** The config loader accepts multiple `[instance <name>]` sections with the same name and treats them as distinct entries. This makes logs and SSE naming ambiguous, and auto-assigned SSE ports are consumed even if the logical instance name repeats. Recording names as they are added and rejecting duplicates would make misconfigured files fail fast.
