# Jafr: two-board working set

Open `Jafr.code-workspace` to work on the current two-board system. The copied
`JAFR-Workspace` still contains unrelated historical projects and is outside this
two-board working set.

## Clean checkout

This repository is the source-of-truth two-board working set. Clone it with the
DualEye source pinned to the accepted firmware commit:

```powershell
git clone --recurse-submodules https://github.com/LeiEugeb-ballz/JAFR-Two-Board-Companion.git
```

If it was cloned without submodules, run `git submodule update --init --recursive`.
Generated builds, serial logs, private flash dumps, old workspace copies, caches,
and credentials are intentionally excluded. Rebuild both images from source using
the documented toolchains; local `sdkconfig` and API/Wi-Fi secrets remain local.

## Current accepted checkpoint, 2026-11-05

- S31 COM18 and DualEye COM13 were rebuilt, flashed, and hash-verified by
  `esptool`. Saved NVS/network configuration was preserved. The S31 image
  physically accepted in that session had SHA256
  `1A4B81225CED10C04D6093D73A230574FC0BF97318A51CFAC9B642730E142302`.
- Final S31 host artifact: `S31-Live-Brain/build_s31_release_j/JAFR_S31_Live_Brain.bin`,
  SHA256 `7C6BE67FA95981DAED340DAFD42CF1C2449EACD0BEE1430265B7696283690C6F`.
  It adds the 12-second stale processing/thinking watchdog and builds with 63%
  of the app partition free. It was flashed to COM18 and boot-accepted on
  2026-11-07; esptool verified the written hash.
- DualEye image: `DualEye-Live-Graphics/xiaozhi-esp32/build_safe/xiaozhi.bin`,
  SHA256 `A857500D07FDDA39E8419E93AE9B0DEDCC1572F9DE23CC13EDFC1B9F250CEBE2`.
- The frozen gaze command is now `LOOK x,y` at both ends. ESP-NOW expressions
  now drive the renderer's live state instead of an unused transition variable.
- Hardware acceptance captured `listening -> thinking -> speaking -> neutral`
  on COM13 with successful parsing and continued animation. The control deck
  reported nine unicast radio deliveries, zero failures, gateway locked, and
  microphone live. No allocation failures occurred during the capture.
- S31 WebUI: `http://192.168.88.242/` on the current Wi-Fi network, or
  `http://192.168.77.2/` when the dedicated Ethernet link is connected.
- Gateway state synchronization now explicitly returns S31 and Eyes to neutral
  after empty STT, rejected noise, TTS completion, or LLM failure. This prevents
  the Eyes remaining stuck in thinking after a silent/failed turn.

Next voice/persona tweak: stop JAPA from repeatedly saying the LED colour.
Preserve the existing persona and voice pipeline; make only evidence-based voice
model refinements, not a rewrite or restructuring. Bluetooth audio routing is
deferred to a later isolated feature.

The eye-awareness refinement is now implemented in source: the model knows the
DualEye is its physical eye hardware, normal conversation no longer exposes the
LED tool, and a dedicated WebUI/API eye test surface is included. Candidate S31
image SHA256: `9269E605AB9C7015E8356360416C3E7D3186DC17624CCCFBD515A9D91EE65781`.
It was flashed and physically accepted on COM18 on 2026-11-07. See
`EYE_CONTROL.md` for the exact protocol, HTTP hooks and acceptance evidence.

| Board / role | Working source | Current local artifact | Running image |
| --- | --- | --- | --- |
| Waveshare DualEye | `DualEye-Live-Graphics/xiaozhi-esp32` | `build_safe/xiaozhi.bin`, SHA256 `A857500D…50CEBE2` | Physically accepted on COM13 with this image |
| ESP32-S31 audio brain | `S31-Live-Brain` | `build_s31_release_j/JAFR_S31_Live_Brain.bin`, SHA256 `7C6BE67F…3690C6F` | Physically accepted on COM18 on 2026-11-07 |

## Inventory, 2026-11-05

- The older downloaded `ESP32-S3-DualEye-Touch-LCD-1.28` was a clean ancestor
  at `a0fd427`. It has been moved to the Windows Recycle Bin after the live
  `3.0.0.1` tree was verified.
- The DualEye source contains independent left/right touch preset selection,
  eye rendering, voice state, and a PC WebSocket gateway. The 2026-09-17 log
  records a verified COM13 flash and stable idle session for its then-current
  `build_safe` image. That log is historical evidence, not a current device match.
- The matching S31 source was found under `Project TakeOver/JAPA-Workspace/esp32s31`
  and copied to `S31-Live-Brain` (5,226 files verified). It contains
  microphone/speaker I2S, Wi-Fi, UDP PCM transport, and the PC Mistral/Voxtral
  voice pipeline. Its firmware project is still named `hello_world`. The
  `build_s31_uart` app image verified against COM18 flash. Reconfigure in a
  fresh build directory before building at this new location.
- The older S31 binary copied from GeminiPro failed verification against COM18.
  The `build_s31_uart/hello_world.bin` image verified byte-for-byte against
  flash at `0x10000`; the matching ELF hash is also in the app descriptor.
- The recovered graphics project is now the `DualEye-Live-Graphics` submodule,
  pinned to the accepted, credential-clean `dualeye-accepted-2026-11-07` source.
  Its ESP-NOW receiver is committed and is the implementation accepted on COM13.
- S31 `sdkconfig` selects ESP32-S31, 2 MB flash, and a single-app partition.
  This is the starting point for the flash-capacity investigation, not evidence
  that the physical board has only 2 MB.
- COM18 is a Silicon Labs CP210x USB-UART bridge (VID 10C4 / PID EA60). Its
  running log repeatedly reports `UDP sendto error: errno 12`, zero received
  packets, and LED state `ERROR`; microphone RMS and peak-to-peak values change.
  The matching source sends UDP to PC `192.168.88.252:5000`, while this PC currently has
  Wi-Fi address `192.168.88.180`. This mismatch blocks a voice round trip.
- The PC pipeline listens on `0.0.0.0:5000`; it does not cause the S31 to
  discover a changed PC address. The running S31 image targets `.252`, so a
  durable fix needs configurable/discovered gateway addressing or an intentionally
  reserved PC address. No network or firmware change has been made yet.
- COM13 appeared after the initial inventory as USB VID 303A / PID 1001. Its
  current Wi-Fi address is `192.168.88.145`. Both displays and both CST816S
  touch controllers initialized; gaze and pupil values changed during the
  monitor. The onboard audio service and wake engine are intentionally bypassed
  in dedicated graphics mode. Boot control reports OTA slot 0 running and slot
  1 without a valid app. Preserve this running image until matching source is
  recovered. See `dualeye-com13-boot-2026-11-05.log` for serial evidence.

## Next acceptance pass

1. With both boards powered, confirm one spoken
   turn returns listening -> thinking -> speaking -> neutral without a stale
   processing state.
2. The DualEye does not need another flash unless its source changes; its current
   accepted image and graphics-state path already match the saved artifact.

The 2.8-inch display project is outside this working set. No graphics redesign or
board merge is implied by this inventory.

## Portability and factory reset

Copy **both** `DualEye-Live-Graphics` and `S31-Live-Brain`, plus this guide and
`Jafr.code-workspace`, to a new PC. The DualEye folder alone cannot restore
the S31. The matching build directories contain app images, bootloaders, and
partition tables; the DualEye build also includes its generated assets. Their
ESP-IDF build caches contain old absolute paths, so a fresh build directory is
required after moving. Existing matched binaries can be retained as recovery
artifacts, but no fresh-PC build or complete reflash has been tested.

Full 16 MB flash snapshots of both physical boards were captured on 2026-11-05
and verified byte-for-byte against the boards with `esptool verify-flash`.
See `Recovery-Private/manifest.json` for image hashes, chip identities, and
limits. They include today's flash partitions and NVS settings, but not chip
eFuses or PC environment variables. They are sensitive: keep them off Git and
public storage. A destructive restore has not been tested, so do not call this
a fully accepted recovery procedure yet.

The two boards' ESP-NOW link was accepted on 2026-11-05. The gaze grammar is
`LOOK x,y` at both ends, COM13 logged parsed conversation-state packets, and the
WebUI exposes both local enqueue results and MAC-level unicast delivery results.

This is an exact **running firmware and source inventory** with verified full
flash snapshots. The matching source trees remain necessary for changes and
rebuilds. The PC gateway still needs its API credential and a reachable network
address after moving to another PC.

## Cleanup state

The older downloaded DualEye directory and five stale build directories were
moved to the Windows Recycle Bin on 2026-11-05. This does not reclaim disk space
until the Recycle Bin is emptied. A later permanent cleanup attempt was blocked
by the execution safety review before deleting any files. The matched DualEye `build` and
S31 `build_s31_uart` directories remain. Both original source locations also
remain intact outside `November/Jafr`; the older multi-project workspace was
left untouched because it contains projects beyond these two boards.

## S31 network/control upgrade, 2026-11-05

The S31 source now builds with automatic PC gateway discovery, periodic
rediscovery, a persistent manual gateway override, an onboard control page, and
the board's YT8531 RGMII Ethernet interface. Wired Ethernet is assigned a higher
route priority than Wi-Fi while both remain enabled. The UI reports both
interface addresses, gateway state, packet counters, microphone level, and free
heap.

The intended fixed installation uses a dedicated USB-Ethernet adapter on the PC.
That adapter can provide a stable gateway address while automatic discovery
remains available as a recovery path.

The build profile now matches the physically verified 16 MB flash and provides
a 3 MB recovery app, two 4 MB OTA slots, and a 4.8 MB storage partition. The
result builds successfully with substantial app headroom. These changes were
flashed and accepted on COM18 on 2026-11-05. Wi-Fi fallback, automatic gateway
discovery, voice transport, mic mute, WebUI, ESP-NOW delivery telemetry, and
conversation-state synchronization are live. Bluetooth earbud audio routing
remains a separate future feature.
