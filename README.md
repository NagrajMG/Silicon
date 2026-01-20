# SILICON

Silicon is a minimal, production-oriented macOS background system agent written in C++17.

It is built in progressive phases, starting with a clean and correct agent lifecycle and system integration.

## Current Phase (Phase 1)

Phase 1 establishes the foundation of the system agent:

- Background agent lifecycle
- Graceful shutdown using POSIX signals (SIGINT, SIGTERM, SIGQUIT)
- Thread-safe file logging
- launchd integration on macOS
- Timed execution for development safety

Detailed documentation: [README][../READMES/Phase1.md]

---
