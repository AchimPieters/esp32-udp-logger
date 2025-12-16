# esp32-udp-logger — Deep Audit

## Scope
- C component in `src/` and `include/`
- Python CLI in `tools/`

## Observations
- Component keeps its footprint minimal and defers initialization until IP is available.
- UDP RX/TX sockets are created lazily and guarded by a mutex when toggling destinations.

## Findings

### 1) CLI network error handling (fixed)
- **Issue:** `tools/esp32_udp_logger_cli.py` assumed local routing and remote host reachability; DNS/routing failures raised uncaught `OSError`, terminating the tool with a traceback.
- **Risk:** Poor UX during discovery/bind flows on hosts with multiple NICs, VPNs, or blocked mDNS.
- **Fix:** Wrap socket operations with explicit `SystemExit` messages to surface actionable context when send/bind/route steps fail.

### 2) Concurrency of drop counter (risk to monitor)
- **Issue:** `s_drop_count` is incremented without a lock; reads in `status` take the mutex. On SMP targets this could produce torn reads.
- **Impact:** Drop counts may be slightly inaccurate but no crash risk.
- **Suggestion:** Guard increments with the mutex or convert the counter to an atomic type if precise telemetry is required.

### 3) Resource cleanup (risk to monitor)
- **Issue:** `esp32_udp_logger_stop()` closes sockets and tasks but leaves the queue and mutex allocated.
- **Impact:** Minor heap leak when repeatedly starting/stopping the logger within the same process lifetime.
- **Suggestion:** Add `vQueueDelete`/`vSemaphoreDelete` (or document the one-shot lifecycle) if repeated lifecycle transitions are expected.

## Recommendations
- Add a minimal unit/integration test for the CLI bind/status flows to prevent regressions in error messaging.
- Consider an optional log of socket errors inside the component (e.g., failed `sendto`), gated by a verbose Kconfig flag.
