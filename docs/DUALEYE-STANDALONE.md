# DualEye-V2 standalone Mistral / Voxtral voice — accepted checkpoint

**Date:** 2026-11-11 (SAST)  
**Repository:** `LeiEugeb-ballz/JAFR-Two-Board-Companion`  
**Implementation branch:** `experiment/dualeye-standalone`  
**Verified implementation commit:** `8351b0499317dd73d2260a3633d0484e1168639d`  
**Primary test record:** [11 November hardware acceptance](../evidence/20261111T184245Z-dualeye-standalone/summary.md)

This is a **standalone firmware experiment** on the Waveshare DualEye board, **not** the two-board S31 + DualEye V2 release. The older V2 main-branch documentation and acceptance remain valid for that separate configuration. The historical `DualEye-Live-Graphics` submodule is a reference; `DualEye-V2` contains the standalone implementation.

## Validated architecture

```text
Waveshare ESP32-S3 DualEye (SKU 32265)
  ES7210 onboard microphone, 16 kHz capture
    -> Wi-Fi -> Voxtral Realtime Transcribe 3
    -> Mistral streamed conversational reply
    -> Voxtral Mini TTS 3 streamed PCM
  ES8311 onboard codec + amplifier + speaker
  Dual GC9A01 displays and onboard IMU continue animating
```

- This run required **no PC audio gateway** and **no ESP32-S31** for the conversation.
- The ESP-NOW receiver was disabled during standalone acceptance. Do not describe this mode as simultaneously validating S31-to-eye expression transport.
- Credentials were provisioned locally into NVS over USB, not committed to GitHub.
- A LAN test control started voice turns. The web console also provides a `Start voice turn` action and `Speak now` state.
- Existing display DMA, artwork, eye rendering, animation and IMU were preserved relative to the V2 hardware baseline.

## Acceptance results recorded on physical hardware

| Measurement | Recorded result |
| --- | --- |
| Device | Waveshare SKU 32265, ESP32-S3 revision 0.2, COM13 |
| Built app | 2,012,752 bytes; hash-verified flash at offset `0x10000` |
| Spoken turns | 3 completed; owner heard each clearly, including the start |
| Audio errors on those turns | 0 RX overruns; 0 capture/playback drops; 0 playback gaps; 0 I2S TX underruns |
| Rendering before voice | 19.1–19.8 frame pairs/s, median 19.6 (93 samples) |
| Rendering during voice | 17.8–19.3 frame pairs/s, median 18.3 (34 samples) |
| Rendering after voice | 18.7–19.5 frame pairs/s, median 19.3 (193 samples) |
| Gyro / gaze | Physical tilt approximately −28.8° pitch, gaze blend 1.00 |
| Continuous post-turn serial coverage | 320.8 seconds, one boot, no reset/crash markers |
| RAM floor | 24,384 bytes internal free; 8,121,408 bytes free PSRAM at Ready |

The previous approximately two-second **opening audio breakup** was isolated to the DMA refill window. Earlier 4 × 192-frame DMA descriptors (48 ms) were raised to 12 descriptors (144 ms), with amplifier drain-time adjustment. All three final animated-build turns then recorded **zero TX underruns** and the operator confirmed clear openings. This is a verified outcome for the recorded three-turn test, **not** a guarantee that all future network conditions are glitch-free.

End-to-end turn timing, from turn start: first TTS PCM at **15.19 s**, **15.17 s**, and **13.94 s** respectively; return to Ready at **20.95 s**, **20.29 s**, and **16.35 s**. These delays are distinct from the resolved audio-breakup symptom; the individual service-latency contributions were not measured.

## Build and local operation

- Firmware: `DualEye-V2`, using Pioarduino `55.03.39`, Arduino `3.3.9`, and Espressif WebSocket client `1.8.0`.
- Board pin allocation: [board profile](../DualEye-V2/board-profile.json). The microphone I2S DIN is GPIO15; I2S OUT is GPIO16; BCLK/WS/MCLK are GPIO13/14/12.
- Start from the standalone branch (not `main`), install matching toolchain/dependencies and provision cloud credentials locally. Read the source/build script and `platformio.ini` before attempting a new flash.
- The accepted app and rollback artifacts are **local** under `DualEye-V2/.pio/bench`; they are **not GitHub release binaries**. Named evidence artifacts: `standalone-audio-dma.bin`, `v2-hardware-rollback.bin`, and `audio-dma-serial.log`.
- Validated app SHA-256: `CED1A23A899E29C7727F502F7CF44BEC9F92D63AB3A90541A482FA76B4390912`.
- Historical rollback image SHA-256: `0D4CBDAFE9E00040C55FC35B816920F9A8C3DD09615A998CECD559FDBB4A1EBB`.
- The rollback baseline tag is `v2-hardware-2026-11-11` at `c3864636285fb42b172a09a24b3da6fbf864ceb0`; keep it immutable.

**Important:** The project directory can contain local uncommitted changes after this commit. A GitHub acceptance record proves the **committed** implementation at `8351b04`, not later edits that have not been separately reviewed or pushed. Before calling the workspace fully synchronized, verify both the parent repository and `DualEye-Live-Graphics` submodule status, including any submodule-local edits. Do not silently stage a dirty submodule or advance its pinned commit.

## Scope and recovery

- Standalone voice acceptance: **passed for the recorded three turns**.
- Eye animation and gyro under voice: **passed for the recorded test**.
- PC-independent operation on the single DualEye board: **demonstrated for that test**.
- Two-board S31 ESP-NOW, BLE audio routing, A2DP, and generalized acoustic echo/barge-in regression: **not part of this acceptance**.
- Credential hardening, long-duration network resilience, and latency optimization: **not claimed as completed**.
- No additional firmware change, flash, or acceptance test is implied by documentation edits alone.

For troubleshooting or continuation, start with [the recorded evidence](../evidence/20261111T184245Z-dualeye-standalone/summary.md), the implementation at `8351b04`, and any subsequently saved local-only changes. Do not rerun the old stutter investigations as though their final DMA correction had never been validated.
