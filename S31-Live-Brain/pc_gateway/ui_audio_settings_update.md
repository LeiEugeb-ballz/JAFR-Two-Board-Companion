# UI Audio Routing Status

## Verified gap

The active M2 firmware automatically routes responses to DualEye whenever BLE is ready and falls back to S31 only when BLE is unavailable. It has no user control to return deliberately to S31.

## Prepared source change — not flashed

- `Auto`: prefer DualEye while ready; otherwise use S31.
- `S31 primary`: keep response audio on S31 even while DualEye BLE is ready.
- `DualEye`: prefer DualEye and use S31 as a safe fallback when unavailable.
- Save the manual selection in NVS and expose saved route plus active output through `/api/status`.
- Apply changes through `POST /api/audio` and the onboard control deck while idle.

## Remaining validation

- Build the S31 source.
- Flash only after explicit authorization and fresh COM18 identity.
- Verify selection persistence, mid-session behaviour, BLE-unavailable fallback, S31 boot sound, DualEye animation and ESP-NOW independence.
