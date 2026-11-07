# JAFR graphics-awareness acceptance — 2026-11-05

## Hardware and firmware

- DualEye: COM13, ESP32-S3, STA MAC `28:84:85:57:2d:2c`
- Brain: COM18, ESP32-S31, MAC `30:ed:a0:f4:3c:0a`
- S31 image physically accepted on COM18 SHA256:
  `1A4B81225CED10C04D6093D73A230574FC0BF97318A51CFAC9B642730E142302`
- Final S31 host artifact SHA256:
  `7C6BE67FA95981DAED340DAFD42CF1C2449EACD0BEE1430265B7696283690C6F`
  This artifact adds the stale conversation-state watchdog. It was flashed and
  boot-accepted on COM18 on 2026-11-07.
- DualEye image SHA256:
  `A857500D07FDDA39E8419E93AE9B0DEDCC1572F9DE23CC13EDFC1B9F250CEBE2`
- Both flashes completed with esptool hash verification. DualEye NVS was not
  erased; it reconnected to `2.4GHz` on channel 1.

## Verified fixes

1. S31 `LOOK` format now matches the receiver: `LOOK x,y`.
2. `EXPRESSION` packets update the renderer's consumed `current_state_` rather
   than only the unused transition target.
3. Listening and speaking are explicit renderer states; thinking, neutral and
   existing emotion states remain mapped.
4. S31 reports both local ESP-NOW submission and Wi-Fi MAC unicast delivery.
5. Gateway control packets synchronize idle/listening/thinking/speaking state.
   Empty STT, rejected noise, TTS completion and LLM failure explicitly return
   the hardware to neutral.
6. The DualEye build helper now correctly removes broken linker search paths on
   Windows. Its previous empty-replacement check was always true and skipped the
   repair.
7. The final S31 source returns processing/thinking to idle after 12 seconds if
   no gateway state arrives, preventing a false local VAD event from leaving the
   visible system stuck.

## Live evidence

COM13 logged successful receipt, parsing and renderer application for:

```text
EXPRESSION listening -> External eye state 1
EXPRESSION thinking  -> External eye state 2
EXPRESSION speaking  -> External eye state 3
EXPRESSION neutral   -> External eye state 0
```

Animation telemetry continued after the state cycle. During the capture:

- free internal SRAM: about 190 KB
- minimum observed internal SRAM: about 87 KB
- PSRAM free: about 8.38 MB
- allocation failures: 0

The live S31 status endpoint at `http://192.168.88.242/api/status` reported the
gateway locked to `192.168.88.180`, ESP-NOW ready, last state neutral, microphone
live, and confirmed unicast deliveries. The dedicated Ethernet address remains
`192.168.77.2` and correctly reports link down while unplugged.

MAC delivery proves that the frame reached the configured Eyes radio. COM13's
parse and renderer logs provide the separate application-level proof.

## Deferred without scope creep

- Bluetooth audio routing: later isolated feature.
- Persona/voice tweak: remove repeated spoken narration of LED colours. Preserve
  the current persona and pipeline; refine rather than restructure.
- Final subjective naturalness acceptance still belongs to a spoken user test.

## PC-side closeout after hardware disconnect

- S31 reproducible build: PASS, 1,161,008-byte app, 63% app partition free.
- DualEye reproducible build: PASS, 3,880,752-byte app, 6% app partition free.
- Gateway Python syntax compilation: PASS for the pipeline and host harnesses.
- Current gateway process: stopped; no board is connected, so no fresh serial,
  radio, microphone, speaker, touch, or WebUI hardware claim was added.

## Final S31 flash acceptance — 2026-11-07

- COM18 identified the expected ESP32-S31, MAC `30:ed:a0:f4:3c:0a`.
- Bootloader and partition table already matched flash; the changed application
  sectors were written and esptool reported `Hash of data verified`.
- Clean boot loaded the factory app at `0x30000`, reported 16 MB flash, and
  initialized ES8311 microphone/DAC, Wi-Fi, ESP-NOW, Ethernet and the HTTP deck.
- The board connected to `2.4GHz` at `192.168.88.242` and automatically
  discovered the canonical gateway at `192.168.88.180`.
- Live `/api/status`: microphone unmuted, ESP-NOW ready, 3 accepted / 0 failed,
  3 MAC-delivered / 0 delivery failures, last state neutral, and 276,536 bytes
  free heap.
- The canonical gateway is running from `S31-Live-Brain/pc_gateway`, bound to
  UDP 5000, and acknowledged the board's discovery packet.
- A fresh spoken response/playback turn was not performed during this flash
  acceptance; the transport and control plane are live and ready for it.
