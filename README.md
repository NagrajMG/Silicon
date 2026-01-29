# SILICON

Silicon is a minimal, production-oriented macOS background system agent written in C++23.

It is built in **progressive phases**, starting from a correct agent lifecycle and evolving toward **policy-driven system monitoring and enforcement**.

---

## Current Phases

### Phase 1 — Agent Foundation

Phase 1 establishes the core system-agent groundwork:

- Background agent lifecycle.
- Graceful shutdown using POSIX signals (SIGINT, SIGTERM, SIGQUIT).
- Thread-safe file logging.
- launchd integration on macOS.
- Timed execution for development safety.

Detailed documentation: [Phase 1](READMES/Phase1.md)

---

### Phase 2 — Policy-Aware Monitoring

Phase 2 builds on the foundation and introduces controlled observation:

- Process monitoring with allowlist and baseline learning.
- Lite trigger model (warm-up + repeat offender gating).
- Filesystem integrity monitoring (create / modify / delete).
- Optional, reversible network restriction demo.
- Noise suppression for macOS system processes.
- Internal debug tracing for development.

Detailed documentation: [Phase 2](READMES/Phase2.md)

---

Silicon is currently in **Phase 2**, focusing on *safe observation and signal extraction* before any destructive enforcement is introduced.
