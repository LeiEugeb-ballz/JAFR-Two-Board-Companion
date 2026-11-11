# S31 standalone voice checkpoint — 2026-11-11

The S31 app flashed to COM18 and verified by esptool has SHA-256
`A349BF1E4541D56976B2E5880DB076BA4E7FE5804C8082DD5B5C6AEF48AB0E9B`.
The app and matching local build configuration were copied to
`Recovery-Private/S31-standalone-voice-2026-11-11`. That snapshot contains
the app and build settings, not a new flash/NVS dump. Keep it private.

The operator confirmed intelligible S31 playback. With UDP port 5000 unbound,
the S31 later reported three completed direct Mistral/Voxtral turns and zero
microphone or playback drops. The PC gateway was restarted later by request.

Open issue: unwanted turns with the S31 speaker active. Direct mode continues
streaming microphone PCM during playback, so speaker-to-microphone echo is a
candidate cause. It has not been isolated. At the checkpoint the S31 reported
zero cloud reconnects and zero microphone or playback drops.

DualEye BLE routing is on hold. COM13 is running a separate Uncanny Eyes build.
The Jaf-R DualEye submodule remains pinned at `904212b`.
