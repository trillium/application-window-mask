# Fail-safe — Safety Mechanisms

Mechanisms ensuring PII never leaks, even when components crash. Distributed
across the plugin and daemon — each side has its own fail-safe behaviors.

## Principles

1. **Default-deny:** Screen is fully masked until explicitly revealed
2. **Fail closed:** Any error → full mask, never reveal on failure
3. **Zero trust between components:** Plugin doesn't trust daemon; daemon doesn't trust Talon

## Plugin-Side (failsafe_c/)

- On startup: full blur until valid shm data received
- On stale data (sequence counter not updated in 5s): full blur
- On missing shm: full blur
- On torn read (seqlock mismatch): use last good data

## Daemon-Side (failsafe_python/)

- On startup: write full-mask to shm before anything else
- On window change: write full-mask immediately, then compute proper mask
- Watchdog: if no update in 5s with windows on screen, force recomposite
- On crash: shm goes stale, plugin detects and fails safe

## Implementations

| Directory | Language | Side | Status |
|-----------|----------|------|--------|
| `failsafe_c/` | C | Plugin | — |
| `failsafe_python/` | Python | Daemon | — |
