# Jaf-R Mission Forecast

The immutable accepted baseline is `checkpoint-2026-11-07`. Experimental work must remain on isolated branches until its milestone is physically accepted. The pinned DualEye revision must not move on `main` until replacement firmware is accepted.

## Audio routing

- The S31 speaker is primary and always plays the boot sound.
- When Bluetooth audio is enabled, DualEye is the automatic default unless another paired output is explicitly selected.
- Preserve any manually selected Bluetooth output until the user changes it.
- DualEye is BLE-only; it does not support A2DP or LE Audio.
- DualEye audio uses a custom BLE GATT receiver and its existing ES8311 codec.
- Initial transport is compressed 16 kHz mono voice with buffered playback.
- Conventional earbuds will use the official ESP-IDF S31 A2DP Source implementation in a later milestone.
- Eye animation/expression updates remain independent of speaker selection.
- The existing graphics UART protocol is frozen.

## PC-independent voice

- Prioritize direct cloud API calls from S31 over extra gateway/recovery layers.
- Reuse the verified LLM, STT, TTS, persona and dashboard behavior.
- The PC becomes optional only after standalone S31 operation is demonstrated.

## Hardware allocation

- Do not add either ESP32-S3 SuperMini without a measured resource or performance need.
- Reserve one SuperMini for deterministic future servo/stepper motion control.

## Milestones

1. Preserve the verified checkpoint. **Accepted.**
2. Test DualEye local speaker. **Accepted at DualEye `e11f247`, parent `612cb2e`.**
3. Prove S31-to-DualEye BLE audio while eyes animate. **Experimental baseline frozen on `feature/m2-ble-gatt-audio`; automated gate passed and DualEye playback/animation were physically confirmed.**
4. Prove ordinary Bluetooth earbuds via A2DP.
5. Add WebUI speaker selection and defaults.
6. Implement direct S31-to-cloud conversation. **Implemented on isolated
   `feature/s31-direct-cloud-voice`; build and source contract pass, physical
   flash/API/audio validation still required.**
7. Demonstrate normal operation with the PC switched off.

## Direct-cloud validation gate

- DualEye source and pinned revision remain unchanged.
- PC gateway remains selectable and functional as the rollback path.
- Realtime STT accepts the S31's 16 kHz mono PCM and produces complete turns.
- Streamed chat deltas reach realtime TTS; 24 kHz PCM is converted to 16 kHz.
- Existing Auto/S31/DualEye output selection controls response playback.
- Listening, thinking, speaking and error states use the existing ESP-NOW eye
  expression interface without changing its protocol.
- Barge-in stops audible response promptly without stale audio leaking into the
  next turn; reconnect and API failure return to a usable state.
- WebUI never returns/logs the API key. Plain-NVS and HTTP provisioning remain
  known security limitations until a secure provisioning milestone is accepted.
- Heap, dropped chunks, reconnects, response latency and eye animation are
  measured on hardware before merge or PC-independent acceptance.

## M2 acceptance gate

Baseline: 15.6 FPS DualEye graphics before BLE audio.

M2 is accepted only when physical evidence shows all of the following:

- S31 discovers the intended DualEye service, negotiates an MTU large enough for one audio frame, reconnects after a link interruption, and exposes counters.
- Compressed 16 kHz mono voice sent by S31 is heard through the verified DualEye speaker.
- S31 boot audio still plays locally and S31 audio remains the fallback if BLE is unavailable.
- ESP-NOW expressions still arrive; both eyes animate; touch remains responsive.
- Eye FPS, render time, heap, packet drops/gaps and playback drops are measured before, during and after audio.
- No reset, allocation failure, frozen display, material audio breakup or unacceptable graphics regression occurs.

Any hardware regression stops M2. Do not merge the M2 branch into `main` before this gate is measured and accepted.

### Current M2 evidence — 2026-11-08

- User heard response audio through DualEye for two conversation turns while the eyes remained animated.
- Fifty status requests completed without an allocation failure; internal minimum remained 19,460 bytes and graphics held 15.58 FPS.
- Five of five controlled startup cycles reconnected at MTU 247 with 15.62-15.74 FPS and zero allocation failures.
- The persisted Auto/S31/DualEye selector was built and flashed. The structured acceptance harness passed 55/55 checks, including route persistence and zero BLE drop/failure deltas.
- User-confirmed physical evidence: clear response audio reached DualEye for two turns, the eyes stayed animated, and expression events were visible.
- Still pending before merge to `main`: left/right touch under active audio, S31 local/fallback speaker playback, and physical confirmation that manual S31/DualEye route selection changes the audible destination.
