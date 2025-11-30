# wfb_supervisor

A tiny supervisor that reads explicit command lines from `config/wfb.conf` and launches them (optionally wrapped by `sse_tail`). It runs init hooks, starts every instance, logs the exact command line, and tears everything down if any child exits or on SIGINT/SIGTERM, then runs cleanup hooks.

## Building and running
- `make` (or `make rebuild`) builds `wfb_supervisor` with `gcc -O2 -std=c11 -Wall -Wextra`
- `./wfb_supervisor` (or `./wfb_supervisor config/wfb.conf`) runs against the sample config in the repo
- `./wfb_supervisor /path/to/custom.conf` uses an alternate config

## Signals
- `SIGINT`/`SIGTERM`: begin shutdown, send `SIGTERM` to children, and escalate to `SIGKILL` if anything lingers.

## Config shape
- `[general]`: `sse_tail`, `sse_host`, `sse_base_port`, and zero or more `init_cmd=` / `cleanup_cmd=` entries (run before starting instances and after shutdown). Commands run via `/bin/sh -c`.
- `[parameters]`: runtime knobs that get substituted into command lines and helper scripts, such as `rx_nics`, `tx_nics`, `master_node`, `link_id`, `mcs`, `ldpc`, `stbc`, `key_file`, `log_interval`, `REGION`, `CHANNEL`, `TXPOWER`, and `WFB_BW_MHZ`.
- `[instance <name>]`: `cmd=...` (full command line). Optional `sse=yes|no`, `sse_port=`, `sse_name=`. If `sse=yes`, the command is wrapped as `sse_tail -p <port> -h <host> -n <name> -- /bin/sh -c "<cmd>"`.

There is no derived flag handling—encode everything you need directly in `cmd=`.

## Samples
- `config/wfb.conf` shows a multi-instance setup with init/cleanup hooks and mixed SSE/non-SSE instances.
- `configs/tx-wfb.conf` is a minimal TX-focused sample.
