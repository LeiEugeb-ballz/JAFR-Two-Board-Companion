# Jaf-R V2 - two-board voice companion

Working-project baseline, accepted by the owner on 2026-11-11. This is an ongoing hardware project, not a claim that every experimental feature is production certified.

## What works

- S31 runs Mistral/Voxtral speech recognition, conversational responses and streamed speech directly over the network. A PC gateway is optional.
- S31 is the primary speaker. Its WebUI provides network, voice route, persona, microphone mute, eye controls and live telemetry.
- The owner accepts the prior DualEye ESP-NOW reactions and smooth eye rendering with and without gyro. Preserve those accepted implementations.
- BLE speaker routing is deferred. Do not enable or expand it as part of this baseline.

## Reproduce the working set

```powershell
git clone --branch v2-baseline-2026-11-11 --recurse-submodules https://github.com/LeiEugeb-ballz/JAFR-Two-Board-Companion.git
cd JAFR-Two-Board-Companion
```

Open `Jafr.code-workspace`. S31 source is `S31-Live-Brain`; accepted historical DualEye source is the pinned `DualEye-Live-Graphics` submodule. Install their documented toolchains and provision network/provider credentials locally. Build caches, keys, NVS dumps and conversation archives are not shipped. Do not blindly flash either board: the current COM13 Field Console image is a separate renderer, not proven identical to the pinned submodule.

S31: `cd S31-Live-Brain; .\build_s31.ps1` builds without flashing. The board hosts its own WebUI at its DHCP address; the dedicated Ethernet configuration uses `http://192.168.77.2/`. Choose Direct Cloud, provision a Mistral key and use S31 primary output. Do not assume a previously recorded Wi-Fi IP remains current.

## Baseline and evidence

Read [V2 acceptance](docs/V2-BASELINE.md) for measured versus owner-reported acceptance, exact revision/hash, and remaining limitations. [S31 guide](S31-Live-Brain/README.md) covers firmware and optional gateway setup. [Eye API](EYE_CONTROL.md) documents the accepted command protocol. [Historical notes](docs/HISTORY-pre-v2.md) retain prior recovery/build records; their addresses and running-image claims are historical.

Existing checkpoint tags and the DualEye submodule pin remain immutable. New firmware work uses separate branches, measured validation and explicit scope planning. No redesign, model migration, BLE work or new hardware is part of this release.
