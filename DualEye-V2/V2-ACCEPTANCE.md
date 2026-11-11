> Flashed and physically accepted for Blink/animation/gyro on 2026-11-11. See [measured record](../docs/V2-FLASHED.md) for limits and remaining checks.

# Jaf-R V2 interaction candidate

The source starts from the Uncanny Eyes Field Console, with its existing
artwork, renderer, IMU settings and web controls. The Jaf-R receiver accepts
unicast Blink and neutral, listening, thinking, speaking and error states
from the current S31. It queues commands for the render loop. Bluetooth
speaker routing is outside this candidate.

Before installing, identify the exact COM13 app and preserve a verified
rollback image. The candidate's `JAFR_RADIO_CHANNEL` must match the S31's
actual 2.4 GHz Wi-Fi channel. It currently uses measured S31 channel 5. If the S31 changes channel, align the two radios before
retesting. MAC-level delivery confirms the radio received a frame; visible
Blink and the render command counter confirm that the application acted.

V2 release gates:

1. Confirm idle animation, IMU and phone console against the current
   COM13 build. Compare the frame-pair rate before and after.
2. Confirm one visible blink per command, followed by visible conversation
   states during a direct Mistral/Voxtral turn.
3. Isolate unwanted voice turns by comparing quiet windows with the
   S31 speaker active and isolated. Verify any echo fix also preserves
   a normal follow-up and intended interruptions.
4. Repeat with the PC gateway stopped and confirm clear S31 audio.
5. Update the repository and docs and create the V2 baseline after physical
   acceptance. Keep the existing accepted checkpoint tags and pinned
   DualEye submodule revision intact.
