# ESPressio-ESP32 BLE Radio Tranche Handoff

## Scope of work

Implement an ESP32 Bluetooth Low Energy (BLE) concrete implementation of the `ESPressio-Radio` `IRadio` abstraction on branch `structural_realignment_propagation_ESPressio-Mesh`, without altering any version numbers.

The implementation is intended to provide an ESPressio-Mesh bearer over the ESP32 integrated BLE radio while preserving conventional Wi-Fi use (including SoftAP/STA and ESPressio-Web/WebSocket traffic). Bluetooth Mesh semantics must not be introduced; BLE is only the hardware/platform-specific Radio bearer.

The user has imposed a strict conflict-avoidance boundary because an ESPressio-Mesh tranche is concurrently active on this branch:

- New BLE Radio implementation code may be introduced in a new source file.
- Existing source code may be changed only in `src/ESPressio_ESP32.hpp`, which is explicitly authorised solely to expose/include the new BLE Radio concrete.
- `README.md` updates are explicitly authorised.
- If any other existing code-containing source file must be changed, halt before changing it and report the filename, required change, and purpose.
- No version numbers may be altered.
- Reciprocal upstream/downstream changes, if genuinely necessary, may only use an already-existing branch in this priority order: `structural_realignment_propagation_ESPressio-Mesh`, then `structural_realignment_propagation`, then `structural_realignment`.

## Mandatory policies

The following policy documents are authoritative throughout this tranche:

1. `ABSTRACTION_POLICY.MD` — platform-specific code belongs in ESPressio-ESP32 while the contextual Radio abstraction remains in ESPressio-Radio; do not leak hardware APIs into platform-agnostic libraries.
2. `CIRCULAR_DEPENDENCY_POLICY.MD` — do not introduce a mandatory or optional circular ESPressio dependency; halt and obtain explicit approval if one becomes unavoidable.
3. `DEMO_POLICY.MD` — public components should have appropriate consuming-developer demonstrations; any demo produced must obey the required Arduino IDE / PlatformIO dual structure and be compilable.
4. `DOCUMENTATION_POLICY.MD` — new/modified public symbols require Visual Studio-compatible doc comments; README must remain accurate after functionality changes.
5. `GITHUB_TASK_RESULT_POLICY.MD` — if GitHub jobs fail because of exhausted task/job budget, do not trust those results and switch to recursive self-review, reporting the condition.
6. `LOGGING_INSTRUMENTATION_POLICY.MD` — use compile-switched ESPressio-Logging instrumentation wherever it makes sense, based on the correct working branch API.
7. `OBSERVER_CALLBACK_POLICY.MD` — use ESPressio-Observable multi-observer semantics for optional callback behaviour rather than singular ad-hoc callbacks; optional Event bridges should be provided where appropriate.
8. `TRANCHE_TIMEOUT_OR_INTERRUPTION_POLICY.MD` — this handoff is the source of current absolute truth; every status change must be logged chronologically with UTC timestamps. If the tranche is interrupted before completion, arrange continuation at the earliest permitted opportunity. On completion, provide this handoff to the initiating chat and delete it from the repository.

## Chronological status log

- 2026-09-03T17:51:27Z — Tranche initiated. User requested BLE `Radio` concrete implementation on `ESPressio-ESP32#structural_realignment_propagation_ESPressio-Mesh` with no version changes and strict protection against conflicts with the active ESPressio-Mesh tranche.
- 2026-09-03T17:51:27Z — Inspected the target branch manifest and confirmed it already depends on `ESPressio-Radio#structural_realignment_propagation_ESPressio-Mesh`; no dependency-manifest change is presently required.
- 2026-09-03T17:51:27Z — Inspected current `IRadio`, `RadioTypes`, `ESPressio_ESP32.hpp`, and `Raw80211Radio` structure. Determined that a new BLE Radio header can implement the current abstraction without modifying `ESPressio-Radio`.
- 2026-09-03T17:51:27Z — Halted before source changes because normal public exposure requires modifying existing `src/ESPressio_ESP32.hpp`; reported the required include-only change to the user.
- 2026-09-03T17:51:27Z — User explicitly authorised the required `src/ESPressio_ESP32.hpp` change and subsequently authorised `README.md` updates. No other existing code-containing source-file changes are authorised.
- 2026-09-03T17:55:29Z — Confirmed `RadioTransport` already owns bounded hop-local fragmentation/reassembly. BLE therefore needs only expose its true physical advertising payload bound; no reciprocal `ESPressio-Radio` change is required.
- 2026-09-03T17:55:29Z — Selected legacy BLE non-connectable advertising + passive scanning as the baseline implementation so original ESP32-class BLE hardware remains supported. Frames use manufacturer-specific advertising data with a configurable company identifier, an ESPressio frame marker, a six-byte destination RadioAddress, and up to 20 opaque Radio payload bytes.
- 2026-09-03T17:55:29Z — Explicitly decided not to advertise `RadioCapability::ReceiveTimestamp` in the initial BLE concrete. The ordinary GAP scan-result callback is not demonstrably close enough to the physical RF reception event for the application’s hard reliable sub-1 ms synchronization requirement. Timestamp capability must only be added after empirical controller/driver-level timing validation.
- 2026-09-03T17:55:29Z — BLE GAP callbacks will perform bounded copying only and signal `RadioWorker`; no parsing/routing/Foundation-Type work will run in the Bluetooth callback context. The implementation will reuse the existing Radio observable surface rather than add ad-hoc callbacks.
