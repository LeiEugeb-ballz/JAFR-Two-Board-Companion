# DualEye standalone voice hardware acceptance

11 November 2026, SAST. Waveshare SKU 32265, ESP32-S3 rev 0.2, COM13, USB serial and Wi-Fi MAC 28:84:85:57:2d:2c.

## Build and recovery

- Arduino 3.3.9 on Pioarduino 55.03.39, with the same Espressif WebSocket client 1.8.0 used by the S31.
- Esptool wrote and hash-verified the 2,012,752-byte app at 0x10000. SHA-256 **CED1A23A899E29C7727F502F7CF44BEC9F92D63AB3A90541A482FA76B4390912**.
- The hardware baseline is tag **v2-hardware-2026-11-11** at c3864636285fb42b172a09a24b3da6fbf864ceb0. The rollback image matched the board's app digest before the first write. SHA-256 **0D4CBDAFE9E00040C55FC35B816920F9A8C3DD09615A998CECD559FDBB4A1EBB**.
- Local bench artifacts are under DualEye-V2/.pio/bench. The installed app is standalone-audio-dma.bin, the rollback is v2-hardware-rollback.bin, and the final serial trace is audio-dma-serial.log.

## Firmware and playback finding

The display DMA, artwork, eye renderer, animation, and IMU code match the baseline. The tilt-triggered sound call was removed. The existing ES8311 speaker driver gained the Waveshare demo's unity-gain volume setting. The ES7210 uses Waveshare's 16 kHz capture configuration and the board's shared I2S clocks.

The board streams mic PCM directly to Voxtral Realtime Transcribe 3, collects the Mistral SSE reply, and sends the reply text to Voxtral Mini TTS 3. It decodes streaming PCM into a bounded PSRAM buffer and plays it through the board's codec and speaker. USB provisioned the credentials into NVS. The desktop test control issued LAN start requests. The ESP-NOW receiver was disabled and the PC audio gateway was absent.

Opening breakup persisted in the external-power trial and in the display-hold trial. The display-hold run registered 13 and 36 additional I2S TX queue underruns on its two turns. The four 192-frame descriptors represented 48 ms of audio, with three completed-descriptor queue slots. The correction increased the descriptor count to twelve and adjusted the amplifier drain time. That gives 144 ms of DMA audio and eleven completed-descriptor queue slots. The two-second TTS prebuffer was the same for both tests. All three spoken turns on the animated build registered zero TX underruns. This identifies the failing DMA refill window. The individual source of the service delays was not measured.

An earlier diagnostic build reset once during startup. Its underrun callback had called a flash-resident atomic accessor from an IRAM interrupt. The corrected callback uses an inline atomic load, verified in the linked ELF before flashing.

## Spoken turns

The operator heard all three replies and reported each good, including the opening of the counting reply. The serial trace confirms live onboard capture, Voxtral transcripts, Mistral replies, Voxtral PCM, and return to Ready after each turn.

| Question | Mistral reply | PCM samples played at 16 kHz | Start to first TTS PCM | Start to Ready |
| --- | --- | ---: | ---: | ---: |
| Count from one to five | One, two, three, four, five. | 62,720 | 15.19 s | 20.95 s |
| First five letters | A, B, C, D, E. | 51,200 | 15.17 s | 20.29 s |
| Spell “voice” backwards | ecioV | 19,200 | 13.94 s | 16.35 s |

All three turns recorded zero RX overruns, capture drops, playback drops, playback gaps, and I2S TX underruns. Minimum free internal RAM was 24,384 bytes. The Ready snapshots reported 8,121,408 bytes free PSRAM.

The physical tilt registered about −28.8° pitch and 1.00 gaze blend after the final turn. Steady baseline FPS was 19.1–19.8 frame pairs/s, median 19.6 across 93 samples. The spoken turns ran at 17.8–19.3, median 18.3 across 34 samples.

The continuous serial trace covered 320.8 seconds after the last reply, through 20:48:06 SAST. It recorded one boot for the full run and zero resets or crash markers. The 193 post-turn samples ranged from 18.7 to 19.5 frame pairs/s, with a median of 19.3. A LAN status read also showed Ready, IMU calibrated, and full gaze blend during the physical tilt.
