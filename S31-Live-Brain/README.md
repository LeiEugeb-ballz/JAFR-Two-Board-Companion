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

Bluetooth earbud output is a planned isolated feature. It is not included in
this firmware yet.

## Build

Run from PowerShell:

```powershell
.\build_s31.ps1
```

The helper uses ESP-IDF 6.2 with the required ESP32-S31 `--preview` flag. It also
uses `--no-configdep` because ESP-IDF's config dependency wrapper mishandles the
space in `Project TakeOver`. Normal compiler dependency tracking and ccache stay
enabled.

The final host-built image is
`build_s31_release_j/JAFR_S31_Live_Brain.bin` (SHA256
`7C6BE67FA95981DAED340DAFD42CF1C2449EACD0BEE1430265B7696283690C6F`).
It has 63% of the 3 MB app partition free. The watchdog revision was flashed and
boot-accepted on COM18 on 2026-11-07; esptool verified the written hash. The
board then connected to the gateway and reported healthy WebUI/ESP-NOW status.

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
applies it after restart. The page never stores API keys.

The page was physically accepted over Wi-Fi on 2026-11-05 at
`http://192.168.88.242/`. The dedicated Ethernet path was accepted earlier at
`http://192.168.77.2/`; it correctly reports `UNPLUGGED` when the cable is absent.
ESP-NOW delivery and mic mute are reported from live firmware, not static UI data.
