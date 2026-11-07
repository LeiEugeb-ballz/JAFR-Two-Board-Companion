# Jaf-R mission forecast

Established 2026-11-07. This document governs the next two hardware goals.
Functional evidence and measured performance take priority over speculative
recovery work.

## Immutable accepted baseline

- Top-level tag: `checkpoint-2026-11-07`
- Top-level commit: `109d3a36e108d269716649db8532217f82852a62`
- Pinned DualEye revision: `f3c5c424d3ca6b1b7775c24396ee18c3c08f001b`
- DualEye tag: `dualeye-accepted-2026-11-07`

Neither tag may be moved, replaced or deleted. New work starts on a separate
branch from the most recent accepted revision. Experimental firmware must not
replace accepted behavior, and the top-level repository must continue to point
at the pinned DualEye revision until a replacement passes its milestone gate.

For DualEye changes, create a branch in the DualEye repository from
`dualeye-accepted-2026-11-07`. A top-level feature branch may point at the
experimental DualEye commit for testing, but `main` remains pinned until the
replacement is accepted.

## Goal A: deliberate audio routing

The S31 speaker is the primary output. It always plays the local boot sound,
regardless of the later conversation-output selection.

Conversation routing is deterministic:

| Bluetooth mode | User selection | Conversation output |
| --- | --- | --- |
| Disabled | Any stale Bluetooth selection | S31 speaker |
| Enabled | No explicit selection | DualEye BLE receiver |
| Enabled | Paired output explicitly selected | Selected output |

The explicit selection is retained in S31 NVS until the user changes it. The
boot sound does not follow this selection. Eye animation, touch and ESP-NOW
expression updates remain independent of the chosen speaker.

### DualEye path

DualEye provides BLE, not Bluetooth Classic A2DP and not LE Audio. Its audio
prototype is therefore a private BLE GATT service between S31 and DualEye, not a
claim of compatibility with ordinary Bluetooth audio sources.

Start with compressed 16 kHz mono voice. S31 packetizes encoded frames with a
sequence number and timestamp; DualEye buffers, decodes and sends PCM to its
existing ES8311 path. Buffering must absorb radio jitter without allowing audio
work to block the eye-render task.

Before the prototype is eligible to merge, record:

- encoded bitrate, ATT payload size and sustained useful throughput;
- packet loss, reorder count and decoder underruns;
- median and worst observed packet-to-speaker latency;
- DualEye free/minimum heap and largest free block before and during playback;
- eye frame-rate baseline and playback frame rate;
- a continuous voice playback run with eyes animating and ESP-NOW expressions
  still being accepted and rendered.

Initial engineering gates are zero progressive heap loss, zero crashes, no
audible repeated underruns during a ten-minute run, at least 30% measured BLE
throughput margin over the selected codec bitrate, and no more than 5% eye-FPS
regression from the measured pre-audio baseline. If those gates cannot be met,
the prototype remains experimental and the accepted submodule stays pinned.

### Ordinary earbud path

ESP32-S31 supports Bluetooth Classic. Implement conventional earbuds from the
official ESP-IDF A2DP Source example and APIs, initially using mandatory SBC:

- [ESP32-S31 A2DP API and source example](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/bluetooth/esp_a2dp.html)
- [ESP32-S31 capabilities](https://documentation.espressif.com/esp32-s31_datasheet_en.html)

Pairing, reconnect, output selection and AVRCP behavior must be measured on the
actual target earbuds. Do not reimplement A2DP over GATT.

## Goal B: PC-independent conversation

Direct S31-to-cloud conversation is the target production path. The PC gateway
remains available during migration but is no longer the assumed runtime once
standalone operation is demonstrated.

Implementation order:

1. Reuse the existing working cloud STT, LLM and TTS providers where their
   protocols fit the S31.
2. Prove direct microphone upload, STT, one LLM turn, streamed TTS playback and
   natural state transitions from the S31.
3. Preserve the current persona, voice behavior, dashboard, mic mute and eye
   expression channel.
4. Provision credentials outside source control and prevent keys from appearing
   in status output or logs.
5. Demonstrate repeated normal conversations with the PC gateway stopped and
   the PC switched off.

Network fallback, elaborate recovery systems and secondary gateways do not
precede a working direct cloud round trip. They are considered only after the
primary standalone path is measured and stable.

## Frozen graphics interface

Do not modify the accepted graphics command vocabulary or parsing behavior:

```text
EXPRESSION <name>
LOOK <x>,<y>
BLINK
SLEEP
WAKE
```

This is the inherited UART-style newline-delimited command grammar currently
carried over ESP-NOW. Audio routing must not change that grammar, the ESP-NOW
state channel, organic idle motion, touch behavior or display ownership.

## Additional ESP32-S3 SuperMini boards

- Reserve one for later deterministic servo/stepper motion control.
- Do not allocate either board to voice or audio without measurements showing
  an actual S31/DualEye resource or timing limit.
- Extra hardware is not a substitute for profiling the current boards.

## Milestones and acceptance gates

Each milestone gets its own branch and evidence record. A build is not physical
proof; acceptance requires the listed hardware observation.

1. `feature/m1-dualeye-local-audio`
   - Prove DualEye ES8311 initialization and local buffered voice playback.
   - Confirm both eyes continue animating and touch remains responsive.
2. `feature/m2-dualeye-ble-audio`
   - Prove compressed S31-to-DualEye GATT audio under the measurement gates
     above while ESP-NOW expressions continue independently.
3. `feature/m3-s31-a2dp-source`
   - Prove pairing and stable S31-to-earbud SBC playback using ESP-IDF A2DP
     Source on at least one real earbud set.
4. `feature/m4-speaker-routing-ui`
   - Add WebUI Bluetooth enable, scan/pair, output selection, connection state
     and measured-route diagnostics. Verify default and retained selection.
5. `feature/m5-s31-direct-cloud`
   - Prove direct STT, LLM and TTS with the existing persona and eye states.
6. `feature/m6-pc-off-acceptance`
   - Cold boot and complete normal conversations with the PC switched off.

For every milestone, capture the source commit, build result, flashed target,
boot evidence, functional result, performance measurements, regressions and
remaining limitations. Only an accepted milestone may be merged and become the
base for the next feature branch.
