# JAFR S31 Live Brain

This is the source tree matched to the ESP32-S31 Function-CoreBoard-1 formerly
seen on COM18. The preserved `build_s31_uart` image matches the firmware that was
running before the November 2026 network and control-plane work.

## Current source capabilities

- ES8311 microphone and 3 W speaker audio path
- UDP PCM voice transport on ports 5000/5001
- automatic PC gateway discovery at startup and every 30 seconds, with a saved
  manual IPv4 fallback
- onboard HTTP control deck with live packet, microphone, heap, Wi-Fi, Ethernet,
  and gateway status
- onboard YT8531 Ethernet through the verified RGMII board pin map; wired link
  is preferred while Wi-Fi remains available
- 16 MB flash layout with 3 MB factory recovery app, two 4 MB OTA slots, and
  4.8 MB local storage
- ESP-NOW graphics-state transmitter to the DualEye board
- MAC-level ESP-NOW delivery counters in the WebUI
- runtime microphone mute in the WebUI (speaker and Eyes remain active)
- explicit gateway state synchronization so empty/rejected/failed turns return
  the Eyes to neutral
- a 12-second stale processing/thinking watchdog for false local VAD events
- a deliberate DualEye HTTP control surface at `POST /api/eyes`, WebUI test
  buttons, and a voice-model `control_eyes` tool
- persona grounding that treats the DualEye as Jaf-R's physical eyes and hides
  the diagnostic LED tool unless the user explicitly asks for it
- persisted response routing (`Auto`, `S31 primary`, `DualEye`) through
  `/api/audio`; it was built, flashed and passed the automated route gate.
  DualEye playback was physically heard while the eyes stayed animated;
  physical S31/fallback route confirmation remains pending
- isolated direct-cloud voice through Mistral realtime STT/TTS WebSockets and
  streamed chat completions; the existing PC gateway remains the fallback and
  the accepted S31/DualEye speaker selector is reused
- WebUI voice-mode selection and write-only key provisioning through
  `POST /api/voice`; status reports connection/counter state, never the key
- bounded eight-turn direct-mode RAM memory (about 3.5 KB maximum), with no
  summarizer call and no per-turn NVS writes
- runtime persona and accepted-facts editing through `GET/POST /api/persona`;
  `memory.txt` can be imported by the browser and is trimmed to the device cap
- direct mode is declared active only after both realtime STT and TTS session
  handshakes; missing/invalid credentials stay visibly degraded to PC fallback
- queued TTS playback keeps codec writes out of WebSocket callbacks; the WebUI
  exposes first-audio latency, reconnect/failure counters, playback drops,
  internal/PSRAM heap floors, largest internal block and allocation failures

Bluetooth earbud output is a planned isolated feature. It is not included in
this firmware yet. Direct-cloud mode remains physically unaccepted until the
live COM18/API/audio test is completed. Conversation memory is intentionally
RAM-only and is lost at restart; persona/facts and the key are persisted in
ordinary NVS. Provisioning uses the board's HTTP-only local WebUI. Those are
explicit validation/security limits, not production claims. Device AEC remains
disabled until a hardware measurement proves acceptable internal-SRAM margin.

## Local conversation memory

The PC gateway uses three bounded local files beside `voice_pipeline.py`:

- `memory.txt`: compact durable facts and accepted decisions, loaded at
  startup with a 6,000-character cap;
- `recent_memory.jsonl`: the ten most recent compact turn summaries loaded on
  restart;
- `conversation_archive.jsonl`: full append-only user/assistant turns, never
  transmitted wholesale and searched only through the `search_memory` tool.

The gateway keeps only a bounded live API history and makes no extra model call
for memory. These files are local and ignored by Git.

## Build

Run from PowerShell:

```powershell
.\build_s31.ps1
```

The helper uses ESP-IDF 6.2 with the required ESP32-S31 `--preview` flag. It also
uses `--no-configdep` because ESP-IDF's config dependency wrapper mishandles the
space in `Project TakeOver`. Normal compiler dependency tracking and ccache stay
enabled.

The direct-cloud branch builds
`build_s31_release_j/JAFR_S31_Live_Brain.bin` at 1,820,896 bytes (SHA256
`34D7669B6583F6587BDA52FAC4AD02EDE6C6DB05BB8E85D0E990A3AA63C69AA6`).
It has 42% of the 3 MB app partition free. This image is not yet flashed. The
previous watchdog revision was flashed and boot-accepted on COM18 on
2026-11-07; esptool verified that earlier written hash and the board then
reported healthy WebUI/ESP-NOW status.

The accepted eye-control image is 1,164,976 bytes, SHA256
`9269E605AB9C7015E8356360416C3E7D3186DC17624CCCFBD515A9D91EE65781`.
It builds successfully with 63% app-partition headroom and was flashed to COM18
on 2026-11-07. Esptool verified the write; the WebUI, gateway discovery and
DualEye command path were physically accepted afterward.

To flash after the S31 is connected and its port is confirmed:

```powershell
.\build_s31.ps1 -Flash -Port COM18
```

Flashing the new build writes a new bootloader, partition table, and app because
the source now uses the board's verified physical 16 MB flash. Keep the full
board dump in `..\Recovery-Private` before doing this.

## Control deck

After flashing, open `http://<S31 Wi-Fi or Ethernet address>/`. Automatic mode
discovers the PC voice gateway from its reply packet and keeps checking so it can
recover when DHCP moves the PC. Manual mode stores an IPv4 address in NVS and
applies it after restart.

The direct-cloud controls accept a Mistral key, store it in ordinary NVS, and
never return it through the status endpoint. The page is HTTP, not HTTPS, so
enter the key only on a trusted local network. `PC fallback` remains the default
until a key is saved and direct mode is explicitly selected.

Run the non-destructive build/source gate with:

```powershell
python ..\tools\s31_direct_cloud_acceptance.py
```

After flashing, add `--base-url http://<S31-address>` for a read-only live
status-schema probe. The harness does not change voice mode or credentials.

The page was physically accepted over Wi-Fi on 2026-11-05 at
`http://192.168.88.242/`. The dedicated Ethernet path was accepted earlier at
`http://192.168.77.2/`; it correctly reports `UNPLUGGED` when the cable is absent.
ESP-NOW delivery and mic mute are reported from live firmware, not static UI data.
