# forker

A tiny supervisor that parses `wfb.conf` and launches WFB helpers (`wfb_rx`, `wfb_tx`, `wfb_tun`) optionally wrapped by `sse_tail`. It starts every instance, logs the exact command line, and tears everything down if any child exits or on SIGINT/SIGTERM.

## Building and running
- `make` (or `make rebuild`) builds `forker` with `gcc -O2 -std=c11 -Wall -Wextra`
- `./forker wfb.conf` runs against the sample config in the repo
- `./forker /path/to/custom.conf` uses an alternate config

## Command synthesis at a glance
- `aggregator` (rx): `wfb_rx -a <listen_port> [-K key] { -c <host> -u <port> | -U unix:/path } [-R rcv_buf] [-s snd_buf] [-l log] [-i linkid] [-p radio_port] <ifaces...>`
- `forwarder` (rx): `wfb_rx -f -c <host> -u <port> [-i linkid] [-p radio_port] <ifaces...>`
- `local/aggregator` (tx): `wfb_tx -K <key> -k <fec_k> -n <fec_n> -u <udp_port> [-C control] [-R rcv_buf] [-s snd_buf] [-l log] -F <fec_delay> -T <fec_timeout> -B <bandwidth> -G <guard_interval> -f <data|rts> -M <mcs> -S <stbc> -L <ldpc> [-i linkid] [-p radio_port] <iface>`
- `distributor` (tx): `wfb_tx -d` plus TX opts, followed by `host:port1,port2,...` (output)
- `injector` (tx): `wfb_tx -I <port> [-R rcv_buf] [-l log] <iface>`
- `tunnel`: `wfb_tun -t <ifname> -a <cidr> -l <input_port> -u <output_port> -c <master_node>`
- SSE wrapper (per-instance `sse=yes`): `sse_tail -p <sse_port> -h <sse_host> -n <sse_name> -- <wfb_* ...>`

## Config notes
- `type=distributor` always runs TX with `-d`; supply a comma list in `output=` (e.g. `host:port1,port2,...`). Interfaces and `radio_port` are ignored in this mode.
- `type=injector` is the `wfb_tx -I` mode; set `inject_port=` and an interface via `wfb_tx` (or first `wfb_nics`). Other TX-specific flags (FEC, modem, control_port, radio_port) are omitted to match the injector CLI.
- `guard_interval` in `[general]` accepts `short|long` (default `long`) and maps to `-G` on TX.
- `distributor` and `injector` are mutually exclusive; `inject_port` is required for injectors.

## Parameters not yet implemented
Tracking the flags we still need to plumb from config → command lines:

- `wfb_rx`:
  - `-e epoch`
  - Any future radio/modem toggles beyond the current set

- `wfb_tx`:
  - Timing: `-F fec_delay`, `-T fec_timeout`
  - RF/modem: `-N VHT_NSS`, `-e epoch`, `-m` (mirror), `-V`, `-Q`, `-P fwmark`
  - Transport variants: `-U unix_socket`

- `wfb_tun`:
  - Only core flags are wired (`-t/-a/-l/-u/-c`); add any additional tun options as needed from the deployed binary version.

- `sse_tail`:
  - Only `-p/-h/-n` are passed; extend if newer versions add knobs.

Feel free to expand the config schema and builders when new flags are required; the defaults live in `init_general_defaults()` and instance parsing in `parse_instance_kv()`.
