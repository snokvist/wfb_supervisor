# Project Review

## Overview
The codebase provides a small supervisor (`wfb_supervisor.c`) that parses INI-like configuration files, expands placeholders, runs init hooks, launches multiple child processes, and tears everything down when any child exits or on termination signals. It currently builds the `wfb_supervisor` binary and can optionally restart after cleanup with a configurable delay.

## Suggested Improvements
1. **Handle `fork` failures after partial startup.** Addressed: startup now records the failing instance, triggers the standard shutdown path, and allows cleanup hooks to run so partially launched children are torn down cleanly.
2. **Detect duplicate instance names in config parsing.** Addressed: config parsing rejects repeated `[instance <name>]` sections (case-insensitive), preventing ambiguous logs.
3. **Expose restart limits.** The restart loop currently runs indefinitely when enabled. Adding a max-attempts counter or a backoff strategy would help avoid tight restart loops in the face of persistent misconfiguration.
