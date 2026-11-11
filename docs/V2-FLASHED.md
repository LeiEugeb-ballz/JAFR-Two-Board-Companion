# Flashed V2 carry-on point - 2026-11-11

Operator physically accepted Blink, animated eyes and gyro after this flash.

## Installed firmware

- COM13 ESP32-S3, MAC 28:84:85:57:2d:2c: `DualEye-V2` Field Console plus queued ESP-NOW receiver. App flashed at 0x10000 and write hash verified. SHA256 `0D4CBDAFE9E00040C55FC35B816920F9A8C3DD09615A998CECD559FDBB4A1EBB`.
- COM18 ESP32-S31, MAC 30:ed:a0:f4:3c:0a: current standalone Mistral/Voxtral firmware retained. Verified byte-for-byte at factory offset 0x30000 against SHA256 `A349BF1E4541D56976B2E5880DB076BA4E7FE5804C8082DD5B5C6AEF48AB0E9B`. No unnecessary S31 reflash.
- S31 primary speaker selected and persisted. PC gateway stopped; UDP 5000 unbound, direct cloud active with both sessions connected. Mic restored to unmuted after the test.
- Current S31 DHCP address: http://192.168.88.138/ . Eye phone console remains the DualEye-2D2C AP, http://192.168.4.1/ . Addresses may change.

## Measured validation

- Eyes before: 20.1-20.3 frame pairs/s. After, steady 19.9-20.3; during state commands 19.5-19.7. The first interval after opening USB serial was lower and was excluded from steady-state comparison, not concealed as an all-sample minimum.
- One Blink request advanced render command count 13 to 14, with no duplicate execution observed.
- Thinking, speaking and neutral requests accepted and render-loop counter advanced; final radio snapshot 14 delivered, zero failed.
- Owner confirms visible Blink, animated eyes and gyro after flashing. Phone console interactive controls were preserved in source, not freshly operator-tested in this pass.
- Source/artifact plus live status schema checks passed; see v2-source-checks.json.
- Fresh end-to-end spoken turn after this particular eye flash was not observed. Standalone audible acceptance remains inherited from the verified unchanged S31 image and prior gateway-off tests.

## Boundaries

The current eye receiver handles BLINK and neutral/listening/thinking/speaking/error. Other historical LOOK, SLEEP, WAKE and extended expressions are not implemented in this Field Console candidate; do not claim every old dashboard action works. BLE audio remains deferred.

The radio channel is deliberately 5, matching the measured S31 access-point channel. If the router changes channels or S31 moves networks, this eye build needs channel alignment. This is not automatic network-independent eye pairing.

Unwanted-turn/echo isolation remains unperformed. No acoustic echo fix or new barge-in acceptance is claimed. The current primary-audio path is unchanged.

## Recovery and provenance

The previous COM13 app was digest-verified against the local voice-retry artifact before writing and copied to `Recovery-Private/DualEye-pre-v2-verified-app.bin`, SHA256 `BD31DDBB886CE0BD12859DEB8D4718EFCA27CF42D48DE3DA6D936A1B87791F73`. Restore that app at 0x10000 only after target identification. Bootloader, partitions and NVS were not rewritten. A slow full-flash read was cancelled and explicitly renamed INCOMPLETE; it is not a recovery image.

S31 verification initially used the wrong 0x10000 address and failed read-only; the actual partition table and boot log establish 0x30000, where verification passed. No write occurred at the wrong address.

Accepted historical tags and DualEye submodule revision 904212bb7cd9a62d9bb674411ab4e36866f027dc remain unchanged. `DualEye-V2` is now the installed graphics source; the historical submodule is a preserved reference. New features remain separate and planned.
