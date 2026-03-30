# Classifier — Window Safety Rules

Determines whether a window should be masked (unsafe) or revealed (safe).
Used by the daemon to classify each window before writing to shared memory.

## Interface

- **Input:** Window metadata (app name, bundle ID, window title, layer)
- **Output:** `safe` or `unsafe`
- **Config:** TOML file at `config/config.toml`

## Rules (evaluated in order)

1. Layer > 0 → always unsafe (notifications, overlays, popups)
2. Bundle ID in safe list → safe
3. Bundle ID + title pattern match → override (e.g., Chrome + "Gmail" → unsafe)
4. Everything else → unsafe (default-deny)

## Implementations

| Directory | Language | Status |
|-----------|----------|--------|
| `classifier_python/` | Python | — |
| `classifier_swift/` | Swift | — |
