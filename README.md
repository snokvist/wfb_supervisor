# forker

A tiny supervisor that parses `wfb.conf` and launches WFB helpers (`wfb_rx`, `wfb_tx`, `wfb_tun`) optionally wrapped by `sse_tail`. It starts every instance, logs the exact command line, and tears everything down if any child exits or on SIGINT/SIGTERM. Reload with `SIGHUP` to force a full teardown/config reload/restart cycle, or send `SIGUSR1` to dump per-instance status.

## Building and running
- `make` (or `make rebuild`) builds `forker` with `gcc -O2 -std=c11 -Wall -Wextra`
- `./forker wfb.conf` runs against the sample config in the repo
- `./forker /path/to/custom.conf` uses an alternate config

## Signals
- `SIGINT`/`SIGTERM`: begin shutdown, send `SIGTERM` to children, and escalate to `SIGKILL` after the grace window if anything lingers.
- `SIGHUP`: trigger a full teardown, reload the config file from disk, and restart all instances with fresh defaults.
- `SIGUSR1`: dump each instance’s running/exited state and exit codes to stderr for quick health checks.

## Command synthesis at a glance
- `aggregator` (rx): `wfb_rx -a <listen_port> [-K key] { -c <host> -u <port> | -U unix:/path } [-R rcv_buf] [-s snd_buf] [-l log] [-i linkid] [-p radio_port] <ifaces...>`
- `forwarder` (rx): `wfb_rx -f -c <host> -u <port> [-i linkid] [-p radio_port] <ifaces...>`
- `local/aggregator` (tx): `wfb_tx -K <key> -k <fec_k> -n <fec_n> -u <udp_port> [-C control] [-R rcv_buf] [-s snd_buf] [-l log] -F <fec_delay> -T <fec_timeout> -B <bandwidth> -G <guard_interval> -f <data|rts> -M <mcs> -S <stbc> -L <ldpc> [-P fwmark] [-Q] [-i linkid] [-p radio_port] <iface>`
- `distributor` (tx): `wfb_tx -d` plus TX opts, followed by `host:port1,port2,...` (output)
- `injector` (tx): `wfb_tx -I <port> [-R rcv_buf] [-l log] <iface>`
- `tunnel`: `wfb_tun -t <ifname> -a <cidr> -l <input_port> -u <output_port> -c <peer_address>`
- SSE wrapper (per-instance `sse=yes`): `sse_tail -p <sse_port> -h <sse_host> -n <sse_name> -- <wfb_* ...>`

## Config notes
- `type=distributor` always runs TX with `-d`; supply a comma list in `output=` (e.g. `host:port1,port2,...`). Interfaces are not used; `radio_port` is respected.
- `type=injector` is the `wfb_tx -I` mode; set `inject_port=` and an interface via `wfb_tx` (or first `wfb_nics`). Other TX-specific flags (FEC, modem, control_port, radio_port) are omitted to match the injector CLI.
- `guard_interval` in `[general]` accepts `short|long` (default `long`) and maps to `-G` on TX.
- `distributor` and `injector` are mutually exclusive; `inject_port` is required for injectors.
- `tunnel` instances now require `peer_address` (used for `wfb_tun -c`); `master_node` is no longer implied.
- `frame_type` can be set per instance (or globally) to choose `-f data|rts` for TX modes (skipped for injectors).
- `agg_timeout_ms` is supported for tunnels (including 0) and maps to `wfb_tun -T`.
- `fwmark` can be set per TX instance to emit `-P <mark>` (useful with shaper). `qdisc=yes` adds `-Q` on TX.

## Parameters not yet implemented
Tracking the flags we still need to plumb from config → command lines:

- `wfb_rx`:
  - `-e epoch`
  - Any future radio/modem toggles beyond the current set

- `wfb_tx`:
  - Timing: `-F fec_delay`, `-T fec_timeout`
  - RF/modem: `-N VHT_NSS`, `-e epoch`, `-m` (mirror), `-V`
  - Transport variants: `-U unix_socket`

- `wfb_tun`:
  - Only core flags are wired (`-t/-a/-l/-u/-c`); add any additional tun options as needed from the deployed binary version.

- `sse_tail`:
  - Only `-p/-h/-n` are passed; extend if newer versions add knobs.

Feel free to expand the config schema and builders when new flags are required; the defaults live in `init_general_defaults()` and instance parsing in `parse_instance_kv()`.
