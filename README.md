# Jaf-R V2 - two-board voice companion

Flashed carry-on point, physically accepted by the owner on 2026-11-11. See [installed firmware and tests](docs/V2-FLASHED.md). This is an ongoing hardware project, not a claim that every experimental feature is production certified.

## What works

- S31 runs Mistral/Voxtral speech recognition, conversational responses and streamed speech directly over the network. A PC gateway is optional.
- S31 is the primary speaker. Its WebUI provides network, voice route, persona, microphone mute, eye controls and live telemetry.
- The owner accepts the prior DualEye ESP-NOW reactions and smooth eye rendering with and without gyro. Preserve those accepted implementations.
- BLE speaker routing is deferred. Do not enable or expand it as part of this baseline.

## DualEye standalone single-board voice (separate experiment)

The [standalone voice checkpoint](docs/DUALEYE-STANDALONE.md) documents the 11 November 2026 hardware-verified `DualEye-V2` microphone-to-Voxtral-to-Mistral-to-Voxtral-TTS pipeline, using only the Waveshare ESP32-S3 DualEye onboard microphone and speaker. Three clear spoken turns were accepted with zero final I2S TX underruns while the dual eyes and gyro remained active (during-voice median 18.3 frame pairs/s). The implementation was committed as `8351b04` on `experiment/dualeye-standalone`; its measured evidence is in [the standalone acceptance record](evidence/20261111T184245Z-dualeye-standalone/summary.md).

**Do not confuse this standalone experiment with the flashed two-board V2 baseline above.** The S31 + ESP-NOW baseline, pinned graphics submodule, and its historical acceptance remain separate. Local edits after the standalone acceptance commit are not covered by its evidence and must be separately committed before considering the workspace fully synchronized.

## Reproduce the working set

```powershell
git clone --branch v2-hardware-2026-11-11 --recurse-submodules https://github.com/LeiEugeb-ballz/JAFR-Two-Board-Companion.git
cd JAFR-Two-Board-Companion
```

Open `Jafr.code-workspace`. S31 source is `S31-Live-Brain`; installed DualEye source is `DualEye-V2`. The pinned `DualEye-Live-Graphics` submodule remains the historical reference. Install their documented toolchains and provision network/provider credentials locally. Build caches, keys, NVS dumps and conversation archives are not shipped. The current COM13 Field Console receiver is separate from the pinned submodule. Use `DualEye-V2/build.ps1` to build it; its radio channel must match S31 (currently 5).

S31: `cd S31-Live-Brain; .\build_s31.ps1` builds without flashing. The board hosts its own WebUI at its DHCP address; the dedicated Ethernet configuration uses `http://192.168.77.2/`. Choose Direct Cloud, provision a Mistral key and use S31 primary output. Do not assume a previously recorded Wi-Fi IP remains current.

## Baseline and evidence

Read [V2 acceptance](docs/V2-BASELINE.md) for measured versus owner-reported acceptance, exact revision/hash, and remaining limitations. [S31 guide](S31-Live-Brain/README.md) covers firmware and optional gateway setup. [Eye API](EYE_CONTROL.md) documents the accepted command protocol. [Historical notes](docs/HISTORY-pre-v2.md) retain prior recovery/build records; their addresses and running-image claims are historical.

Existing checkpoint tags and the DualEye submodule pin remain immutable. New firmware work uses separate branches, measured validation and explicit scope planning. No redesign, model migration, BLE work or new hardware is part of this release.
