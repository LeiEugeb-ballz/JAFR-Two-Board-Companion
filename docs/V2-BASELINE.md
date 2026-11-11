> Superseded for installed hardware by [V2 flashed carry-on point](V2-FLASHED.md). This file preserves the earlier documentation-only freeze.

# V2 working-project acceptance - 2026-11-11

Owner requested closing the current phase and accepts the prior working ESP-NOW reactions, smooth gyro/no-gyro eyes and independent cloud conversation. This record accepts that report without inventing new hardware measurements.

## Frozen implementation

- S31 firmware source checkpoint: `9263876`.
- Verified S31 app SHA256: `A349BF1E4541D56976B2E5880DB076BA4E7FE5804C8082DD5B5C6AEF48AB0E9B` (1,842,512 bytes).
- DualEye submodule: `904212bb7cd9a62d9bb674411ab4e36866f027dc`, unchanged.
- Preserve `checkpoint-2026-11-07`, the submodule's accepted tags and `s31-standalone-voice-2026-11-11`.
- No firmware, GPIO assignments, network routes or persona changed for this documentation freeze. No flash or rebuild performed.

## Acceptance ledger

| Area | Evidence and scope |
| --- | --- |
| Standalone voice | Saved gateway-off checkpoint records UDP 5000 unbound, three completed direct turns and zero mic/playback drops. Owner confirmed clear audio and independence. |
| Current source/artifact checks | Re-run on freeze: 34/34 source/artifact checks passed; these are not acoustic or hardware tests. |
| Eye reactions and gyro/no-gyro rendering | Owner accepts previous checkpoint operation. Recent COM13 blink failure remains a distinct current-image mismatch observation; it is not erased by historical acceptance. |
| Phone console and frame rate | Prior COM13 serial observation approximately 20 frame pairs/s (39-40 panel frames/s), web enabled. No before/after candidate comparison was performed. |
| New DualEye receiver | Separate unflashed experimental candidate; not included in the accepted deployed baseline. It must match the S31 radio channel and pass visible Blink/state acceptance before deployment. |
| Unwanted voice turns / echo | Speaker-active versus isolated quiet-window test and intended-interruption regression have not been performed. No echo fix is claimed. Owner accepts closing this phase with this documented limitation. |

## Recovery and development rules

The private S31 snapshot `Recovery-Private/S31-standalone-voice-2026-11-11` contains app and sdkconfig, not a complete fresh flash/NVS dump. Older private full dumps have their own manifests. Keep credentials and dumps out of GitHub.

COM18 is the S31 and COM13 the DualEye in this session; identify hardware again before future flashing. Do not install stock eye firmware or treat a build pass as physical acceptance. The current COM13 Field Console renderer and the historical pinned Jaf-R renderer are different source lines; this baseline does not claim a single freshly verified restore image for both.

Next sessions are scoped refinements. First priorities, if resumed: reconcile the live eye image with its reaction receiver and measure echo without losing intentional interruptions. BLE audio, architectural changes and rebuilds require deliberate planning; preserve accepted behavior and all existing tags.
