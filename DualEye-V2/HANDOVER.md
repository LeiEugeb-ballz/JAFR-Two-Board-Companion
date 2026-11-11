# DualEye Field Console handover

## Where the work is

- Web UI candidate and single Arduino sketch: `C:\Users\txr45\Documents\DD\WaveshareUncannyEyes-WebUI`.
- Accepted gyro firmware, baseline and original recovery image: `C:\Users\txr45\Documents\DD\WaveshareUncannyEyes`.
- Target: Waveshare ESP32-S3-DualEye-Touch-LCD-1.28, SKU 32265, USB COM13, MAC `28:84:85:57:2d:2c`.
- LCD and MPU wiring follow the accepted gyro build. LCD1 connector positions 7 and 8 go to the sensor SDA/GPIO11 and SCL/GPIO10. Sensor address `0x68`, chip ID `0x72`.
- Preserved accepted gyro app SHA-256: `cfcf825476417308704590b5e662f736d7a2aa057af931a6d7be5d3162c6982b`.
- Candidate compiled successfully and both output images and checksums are saved in `firmware`. `build.log` records PlatformIO platform 55.03.39 and Arduino 3.3.9.
- Chrome with a simulated board API passed desktop/mobile layout, joystick and release, mode changes, wink, recenter, brightness, preset save/load, and defaults. The mock is browser evidence only.
- Embedded upstream artwork and pixel renderer matched the accepted source byte for byte. The accepted gyro-only median was 14.6 frame pairs/s.
- The page hosts its own AP, `DualEye-2D2C`, and serves the Field Console at `http://192.168.4.1/`. The final app broadcasts an open network as requested. If a phone cached the old password, forget the network and reconnect. Voice bay is reserved for future onboard mic/speaker work.

## Current bench state

- The first Web UI app used PSRAM composition and typically ran at 12.8 frame pairs/s. It was rolled back successfully. Exact images remain in `firmware`, with evidence in `psram-flash.log`, `stability.log`, and `rollback-serial.log`.
- The current Web UI app uses two internal 40-row DMA buffers and six transfers per eye. Eye geometry, pixel-color calculations and artwork were retained. It built successfully and esptool hash-verified the application-only write to `0x10000` on COM13. Exact images are under `firmware/stripe`. See `stripe-build.log`, `stripe-flash.log`, and `stripe-stability.log`.
- Boot initialized the AP, calibrated the IMU, and reported about 20 frame pairs/s and 204 KiB free internal RAM. The protected stripe build reported roughly 19–20 frame pairs/s during the idle soak. The open-access app then flashed and hash-verified. It started the same AP and calibrated the gyro. Exact final images are in `firmware/open`. See `open-build.log`, `open-flash.log`, and `open-stability.log`. The user opened the console on a phone and confirmed both eyes looked smooth and responded.
- Optional UI authoring files are in `ui`. Run `python ui/update-ui.py` to repack changed HTML, then run `./build.ps1`. The Arduino sketch is self-contained.
- Laptop was on battery during the work. Recheck power before any further flash.

## Resume safely

1. Confirm stable laptop and board power, then check COM13's USB serial/MAC again.
2. Read `flash.log` and `stability.log` if present. A verified write to `0x10000` means the console app was flashed. An incomplete log alone cannot prove the active app. Read-only digest verification resolves it.
3. The installed open build has already booted, calibrated the IMU and passed the user visual check. Join the board AP and open the console. Rerun the visual and serial checks if firmware changes.
4. If the console regresses from the accepted eyes, restore the known gyro app. With COM13 verified and the existing partition layout, the rollback command is below.

```powershell
$tool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$rollback = 'C:\Users\txr45\Documents\DD\WaveshareUncannyEyes\imu\firmware.bin'
python $tool --chip esp32s3 --port COM13 --baud 460800 write-flash --flash-size keep 0x10000 $rollback
```






## Final acceptance

- The accepted pre-voice app is `firmware/open/firmware.bin` at `0x10000`. Esptool verified the write on COM13. SHA-256 `2a715e7977d373472e3e5b4df1631d75ec5dcbea7061f3deb5f59a78b51d9e83`.
- The 110-second run recorded 61 steady frame-rate samples, 17.0–19.8 frame pairs/s, median 18.8. One client connected and the board applied 250 control commands. Minimum free internal RAM was 201,268 bytes. One boot banner and no reset or halt were observed in the run.
- User confirmed the phone console controls both physical eyes and that they still look good. Detailed preset, brightness and recenter UI paths passed the browser fixture. Long-duration operation and voice I/O are outside this bench test.
- The onboard microphone and speaker/amp are reserved for future work. The next feature can start from the Voice bay in `ui/console.html`.

If the installed layout matches the current 16 MiB PlatformIO build, an application-only reinstall is:

```powershell
$tool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
python $tool --chip esp32s3 --port COM13 --baud 460800 write-flash --flash-size keep 0x10000 firmware/open/firmware.bin
```

## Onboard voice follow-up

- User specified audio through the onboard amplifier and an external speaker. Browser playback is excluded.
- The voice candidate adds only the ES8311 output path, gyro-triggered phrase detection, and the dashboard controls. It uses I2S GPIO12/13/14/16 and amplifier-enable GPIO9, with the codec on the established Wire bus at 0x18. The audio worker is separate from the renderer.
- The accepted open Web UI remains the rollback target at `firmware/open/firmware.bin`, and its source and HTML are archived alongside it. The voice retry image is built and flashed. See `voice-build.log`, `voice-flash.log`, and `voice-stability.log` for the latest evidence.
- Onboard microphone capture, a real mic mute, and wake word “Jafr” with a randomized reply remain a separate task. User asked for microphone control. Avoid implying that speaker mute affects a microphone.

### Voice bench checkpoint

The first speaker candidate flashed and preserved the eye and gyro operation, but it reported “board DAC unavailable.” The retry starts the audio clock before reading the ES8311 ID registers individually and logs the failed stage if it cannot initialize. Exact rollback for eyes and Web UI is still `firmware/open/firmware.bin`. Wake word scope is only “Jafr,” one randomly selected short spoken acknowledgment, then return to listening. The microphone mute must control the real capture path.


### Voice build on the board

The retry app `firmware/voice-retry/firmware.bin` was hash-verified at `0x10000` on COM13. Its SHA-256 is `bd31ddbb886ce0bd12859deb8d4718efca27cf42d48de3da6d936a1b87791f73`. The board read ES8311 ID `83:11` at I²C address `0x18`, reported its DAC ready and calibrated the gyro. The open Wi-Fi AP and existing display paths booted normally. The ESP32 plays stored voice clips through the ES8311 and the onboard amp to the speaker header. A phone is used only to control the dashboard. Physical audio and gesture confirmation await the external speaker after the assembly has dried.

If rollback is needed, confirm COM13 and write the accepted `firmware/open/firmware.bin` to `0x10000`. That version already passed the user’s visual check.


The 60-second voice firmware run showed one boot, no resets or halts, and 34 steady frame-rate samples from 19.2 to 19.8 pairs/s (median 19.7). The speaker was unavailable during the run. Once it is connected, use Voice bay → Test wow first, then test forward tilt, backward tilt, and repeated side-to-side turns. Use Reverse tilt if the two pitch reactions are swapped. Voice mute controls speaker playback. Mic input is currently off. The planned mic mute and “Jafr” wake-only acknowledgment remain open.
