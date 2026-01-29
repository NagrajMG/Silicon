# Silicon (Phase 2)

Policy-driven macOS background security agent.  
Phase 2 introduces **behavior-aware enforcement** with a *lite trigger model* that suppresses system noise, learns baseline behavior, and raises violations only when meaningful thresholds are crossed.

Phase 2 is strictly **observe + detect**, not destructive.

---

## Features

- Process policy enforcement with allowlist
- Lite trigger model (warm-up + repeat offender gating)  
- Noise suppression for macOS system processes  
- Filesystem integrity monitoring (create / modify / delete)  
- Network restriction demo (non-destructive)  
- Thread-safe logging + internal debug tracing  
- launchd-compatible agent lifecycle  
---

## Build (CMake)

```bash

mkdir -p build

cmake -S . -B build
cmake --build build

```

The output binary is: `build/SILICON`

## Run manually

```bash

./build/SILICON

```

---

## Run with launchd

1) Update the path in the plist to your absolute binary location:

- [installer/com.nagrajmg.silicon.plist](../installer/com.nagrajmg.silicon.plist)

2) Copy the plist in the LaunchAgents:

```bash

cp installer/com.nagrajmg.silicon.plist ~/Library/LaunchAgents/

```

3) Load and start the agent:

```bash

launchctl load -w ~/Library/LaunchAgents/com.nagrajmg.silicon.plist

```

4) Unload and stop the agent:

```bash

launchctl unload -w ~/Library/LaunchAgents/com.nagrajmg.silicon.plist

```

### Checks

```bash

launchctl list | grep silicon

```
This returns the job identifier and confirms the agent is active.

---

## Development Safety - Phase 2 Safety Guarantees
1. No kernel hooks
2. No forced termination by default
3. No system process disruption
4. No irreversible network changes
5. Phase 2 is safe by design.
---
