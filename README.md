# forker

A tiny supervisor that reads explicit command lines from `wfb.conf` and launches them (optionally wrapped by `sse_tail`). It runs init hooks, starts every instance, logs the exact command line, and tears everything down if any child exits or on SIGINT/SIGTERM, then runs cleanup hooks.

## Building and running
- `make` (or `make rebuild`) builds `forker` with `gcc -O2 -std=c11 -Wall -Wextra`
- `./forker wfb.conf` runs against the sample config in the repo
- `./forker /path/to/custom.conf` uses an alternate config

## Signals
- `SIGINT`/`SIGTERM`: begin shutdown, send `SIGTERM` to children, and escalate to `SIGKILL` if anything lingers.
- `SIGUSR1`: dump each instance’s running/exited state and exit codes to stderr.

## Config shape
- `[general]`: `sse_tail`, `sse_host`, `sse_base_port`, and zero or more `init_cmd=` / `cleanup_cmd=` entries (run before starting instances and after shutdown). Commands run via `/bin/sh -c`.
- `[instance <name>]`: `cmd=...` (full command line). Optional `sse=yes|no`, `sse_port=`, `sse_name=`. If `sse=yes`, the command is wrapped as `sse_tail -p <port> -h <host> -n <name> -- /bin/sh -c "<cmd>"`.

There is no derived flag handling—encode everything you need directly in `cmd=`.

## Samples
- `wfb.conf` shows a multi-instance setup with init/cleanup hooks and mixed SSE/non-SSE instances.
- `local-wfb.conf` is a minimal local RX/TX test.
