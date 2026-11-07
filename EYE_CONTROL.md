# JAFR eye-control path

## What currently drives the eyes

Conversation-state changes originate in `S31-Live-Brain/pc_gateway/voice_pipeline.py`.
The gateway sends a UDP control packet to the S31 on port 5001:

- magic: `CO`
- version: `1`
- flags: `0x04`
- one-byte state: `0 neutral`, `1 listening`, `2 thinking`, `3 speaking`

The S31 maps those states to text commands and sends each command over ESP-NOW
to the DualEye STA MAC `28:84:85:57:2d:2c`:

```text
EXPRESSION neutral
EXPRESSION listening
EXPRESSION thinking
EXPRESSION speaking
```

The complete frozen S31-to-DualEye command vocabulary is:

```text
EXPRESSION <name>
LOOK <x>,<y>          # each coordinate -100..100
BLINK
SLEEP
WAKE
```

Every command is newline-terminated. The S31 submits both broadcast and unicast
ESP-NOW copies; only the configured DualEye unicast MAC contributes to the
delivery counter.

## Deliberate control API

The running S31 control deck exposes `POST /api/eyes`:

```powershell
$base = 'http://192.168.88.242/api/eyes'
curl.exe -X POST $base -d 'action=blink'
curl.exe -X POST $base -d 'action=expression&value=happy'
curl.exe -X POST $base -d 'action=expression&value=surprised'
curl.exe -X POST $base -d 'action=expression&value=neutral'
curl.exe -X POST $base -d 'action=sleep'
curl.exe -X POST $base -d 'action=wake'
curl.exe -X POST $base -d 'action=look&x=-40&y=-20'
```

The WebUI at `http://192.168.88.242/` contains equivalent buttons. A successful
response is `{"ok":true,"queued":true}`. HTTP 409 means the command was a
duplicate, rate-limited, unavailable or the graphics queue was full.

Check transport evidence with:

```powershell
curl.exe http://192.168.88.242/api/status
```

For a valid radio test, `espnow_accepted` and `espnow_delivered` must increase,
`espnow_failed` and `espnow_delivery_failed` must remain unchanged, and
`espnow_last` must show the requested command. That proves MAC delivery. Watching
the displays or capturing COM13 `RX PARSE: SUCCESS` provides render-level proof.

## Voice-model hardware identity

The gateway now defines the ESP32-S31 as Jaf-R's voice brain and the Waveshare
DualEye as her physical animated eyes. It exposes `control_eyes` for deliberate
blink, expression, sleep and wake actions. Automatic listening/thinking/speaking
animation remains independent and needs no model tool call.

The RGB LED tool is hidden during normal conversation and becomes available only
when the user's speech explicitly mentions the LED or status light. The persona
forbids unsolicited LED-colour narration.

Direct gaze is available in the WebUI/API for testing but is not exposed to the
voice model yet. `LOOK` selects manual gaze and the currently flashed DualEye has
no command to release it back to organic random gaze; exposing that prematurely
would make the eyes look worse after one command.

## Evidence from 2026-11-07

The currently flashed automatic state channel was exercised through listening,
thinking, speaking and neutral. S31 status changed from 108 accepted / 108
delivered to 112 accepted / 112 delivered with zero failures. This proves the
existing gateway-to-S31-to-DualEye radio path before the new API image is flashed.

The eye-control image was then flashed to COM18 and esptool verified the write.
The canonical gateway was restarted from the consolidated project and discovered
by the S31. Direct API tests for blink, happy, surprised and neutral all returned
HTTP 200 with `queued:true`.

Final transport status after acceptance:

```text
espnow_accepted: 12
espnow_delivered: 12
espnow_failed: 0
espnow_delivery_failed: 0
espnow_last: EXPRESSION neutral
free_heap: approximately 275 KB
```

COM13 supplied application-level evidence, not just radio acknowledgement:

```text
EXPRESSION happy     -> External eye state 4 -> RX PARSE: SUCCESS
EXPRESSION surprised -> External eye state 7 -> RX PARSE: SUCCESS
EXPRESSION neutral   -> External eye state 0 -> RX PARSE: SUCCESS
```

The live gateway process contains the new physical-body grounding and
`control_eyes` tool schema. A spoken user command remains the final subjective
persona/tool-selection test; the firmware, HTTP bridge, ESP-NOW delivery and
DualEye parsing/render-state application are accepted.

