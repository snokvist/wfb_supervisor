# Repository Guidelines

## Project Structure & Module Organization
The repository is intentionally small: `wfb_supervisor.c` in the root contains the entire supervisor, including config parsing, argument synthesis, and child management. The expected build artifact is `wfb_supervisor`, placed alongside the source for simplicity. `wfb.conf` demonstrates the canonical layout of `[general]` radio defaults followed by multiple `[instance name]` blocks that describe aggregators, forwarders, locals, and tunnel helpers. Keep sample configs in the root so contributors can diff them easily, and document any required external binaries (e.g., `wfb_ng`, `sse_tail`) inline in the config comments.

## Build, Test, and Development Commands
```
gcc -O2 -std=c11 -Wall -Wextra -o wfb_supervisor wfb_supervisor.c   # build locally
./wfb_supervisor wfb.conf                                   # run using the provided sample config
./wfb_supervisor /path/to/custom.conf                       # use any alternate config
```
Prefer `clang` only if you also add `-fsanitize=address` for leak checks. When iterating, rebuild before each run so compiler warnings stay visible.

## Coding Style & Naming Conventions
Follow the existing C11 style: four-space indentation, no tabs, braces on the same line as statements, and minimal inline comments unless the logic is non-obvious. Functions, variables, and struct fields use snake_case; globals are prefixed with `g_`, while enum constants use `INST_KIND_*`. Keep helpers `static` inside `wfb_supervisor.c` unless you introduce a new translation unit. Guard string buffers with the provided `MAX_*` constants and reuse the existing error helpers instead of printing raw `perror`.

## Testing Guidelines
There is no automated test harness, so rely on targeted runtime checks. Before opening a PR: (1) run `./wfb_supervisor wfb.conf` and ensure all configured child commands are echoed correctly; (2) interrupt with `Ctrl+C` to confirm graceful teardown messages; (3) if you change parsing, craft a minimal config that exercises the new option and observe the stderr diagnostics. Consider wrapping runs with `timeout` so zombies do not linger.

## Commit & Pull Request Guidelines
Recent history favors short, imperative summaries (e.g., “Update wfb.conf”, “Implement supervisor functionality…”). Keep subject lines under ~72 characters and mention the touched subsystem when possible. Pull requests should describe the scenario, highlight config samples that reviewers can try, and include logs or command snippets that prove successful child startup and shutdown. Link any tracking issues and note risk areas such as signal handling or buffer sizing.

## Security & Configuration Tips
Never commit real keys; `wfb.conf`’s `key_file` should reference a local path (`/etc/gs.key`) that is not tracked. Validate any contributor config so it binds only to loopback unless the use case explicitly requires public listeners. When enabling SSE streaming, double-check `sse_host` and port ranges to avoid exposing telemetry unintentionally.
