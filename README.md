# wfb_supervisor

A tiny supervisor that reads explicit command lines from `config/wfb.conf` and launches them. It runs init hooks, starts every instance, logs the exact command line, and tears everything down if any child exits or on SIGINT/SIGTERM, then runs cleanup hooks. A `--restart` flag can keep the supervisor looping after shutdown once cleanup hooks finish.

## Building and running
- `make` (or `make rebuild`) builds `wfb_supervisor` with `gcc -O2 -std=c11 -Wall -Wextra`
- `./wfb_supervisor` (or `./wfb_supervisor config/wfb.conf`) runs against the sample config in the repo
- `./wfb_supervisor /path/to/custom.conf` uses an alternate config
- `./wfb_supervisor --restart --restart-delay 3` restarts after shutdown, sleeping the given number of seconds (default 3) before relaunching

## Signals
- `SIGINT`/`SIGTERM`: begin shutdown, send `SIGTERM` to children, and escalate to `SIGKILL` if anything lingers.

## Config shape
- `[general]`: zero or more `init_cmd=` / `cleanup_cmd=` entries (run before starting instances and after shutdown). Commands run via `/bin/sh -c`, and these hooks are only valid in `[general]`.
- `[parameters]`: runtime knobs that get substituted into command lines and helper scripts, such as `rx_nics`, `tx_nics`, `master_node`, `link_id`, `mcs`, `ldpc`, `stbc`, `key_file`, `log_interval`, `REGION`, `CHANNEL`, `TXPOWER`, and `BANDWIDTH`.
- `[instance <name>]`: `cmd=...` (full command line). Optional `quiet=yes|no` suppresses stdout/stderr.

There is no derived flag handling—encode everything you need directly in `cmd=`.

## Samples
- `config/wfb.conf` shows a multi-instance setup with init/cleanup hooks and quiet logging for background helpers.
- `config/tx-wfb.conf` is a minimal TX-focused sample for local testing (not installed by `make install`).
