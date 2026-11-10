#!/usr/bin/env python3
"""Build/source gate and optional read-only live probe for S31 direct-cloud voice."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
S31 = ROOT / "S31-Live-Brain"
CLOUD = S31 / "main" / "jafr_cloud_voice.c"
MAIN = S31 / "main" / "main.c"
WEB = S31 / "main" / "jafr_webui.c"
BINARY = S31 / "build_s31_release_j" / "JAFR_S31_Live_Brain.bin"
APP_PARTITION_BYTES = 0x300000


def require(results: list[dict[str, object]], name: str, condition: bool, detail: str) -> None:
    results.append({"name": name, "passed": bool(condition), "detail": detail})


def source_gate(results: list[dict[str, object]]) -> None:
    cloud = CLOUD.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")
    web = WEB.read_text(encoding="utf-8")

    required_cloud = {
        "STT websocket": "audio/transcriptions/realtime?model=voxtral-mini-transcribe-realtime-3",
        "TTS websocket": "audio/speech/realtime?model=voxtral-mini-tts-3-0",
        "streaming chat": 'cJSON_AddBoolToObject(root, "stream", true)',
        "STT PCM append": '"input_audio.append"',
        "turn completion": '"turn.end"',
        "TTS text append": '"input_text.append"',
        "TTS generation flush": '"input_text.flush"',
        "TTS PCM delta": '"speech.audio.delta"',
        "TLS certificate bundle": "esp_crt_bundle_attach",
        "write-only NVS key": 'nvs_set_str(handle, "api_key", api_key)',
    }
    for label, token in required_cloud.items():
        require(results, label, token in cloud, token)

    require(results, "24 kHz to 16 kHz conversion", "groups * 2" in cloud and "total / 3" in cloud,
            "three 24 kHz samples produce two 16 kHz samples")
    require(results, "accepted audio route reused", "route_response_pcm" in main and
            "jafr_ble_audio_submit_pcm" in main and "esp_codec_dev_write" in main,
            "cloud playback uses the existing S31/DualEye selector")
    require(results, "eye states reused", "cloud_state_changed" in main and
            's31_graphics_expression("thinking"' in main,
            "direct cloud state drives the existing ESP-NOW expression path")
    require(results, "PC fallback retained", "jafr_cloud_voice_is_direct" in main and
            "sendto(sock" in main, "mic transport branches between direct cloud and UDP gateway")
    require(results, "WebUI voice endpoint", '"/api/voice"' in web, "POST /api/voice")
    status_body = web.split("static esp_err_t status_get", 1)[1].split("static esp_err_t", 1)[0]
    require(results, "API key absent from status JSON", '\\"api_key\\"' not in status_body,
            "status exposes only cloud_key_configured")

    suspicious = re.search(r"(?i)(?:sk-|mistral[_-]?key[^\n=]*=)[A-Za-z0-9_-]{20,}", cloud)
    require(results, "no hard-coded provider token", suspicious is None,
            "source contains no credential-shaped literal")


def binary_gate(results: list[dict[str, object]]) -> None:
    exists = BINARY.is_file()
    require(results, "release binary exists", exists, str(BINARY))
    if not exists:
        return
    data = BINARY.read_bytes()
    require(results, "binary fits app partition", len(data) <= APP_PARTITION_BYTES,
            f"{len(data)} / {APP_PARTITION_BYTES} bytes")
    results.append({"name": "binary SHA256", "passed": True,
                    "detail": hashlib.sha256(data).hexdigest().upper()})


def live_probe(results: list[dict[str, object]], base_url: str) -> None:
    url = base_url.rstrip("/") + "/api/status"
    with urllib.request.urlopen(url, timeout=5) as response:
        status = json.load(response)
    required = {
        "voice_mode", "cloud_state", "cloud_key_configured", "cloud_stt_connected",
        "cloud_tts_connected", "cloud_chunks_sent", "cloud_chunks_dropped",
        "cloud_turns_completed", "audio_route", "audio_output", "espnow_ready",
    }
    missing = sorted(required.difference(status))
    require(results, "live status schema", not missing,
            "complete" if not missing else "missing: " + ", ".join(missing))
    require(results, "live status does not expose key", "api_key" not in status,
            "only configuration presence may be reported")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", help="Optional S31 URL for a read-only /api/status probe")
    parser.add_argument("--json", action="store_true", help="Emit structured JSON")
    args = parser.parse_args()

    results: list[dict[str, object]] = []
    source_gate(results)
    binary_gate(results)
    if args.base_url:
        try:
            live_probe(results, args.base_url)
        except Exception as exc:
            require(results, "live status probe", False, f"{type(exc).__name__}: {exc}")

    passed = sum(1 for item in results if item["passed"])
    payload = {"passed": passed, "total": len(results), "ok": passed == len(results), "checks": results}
    if args.json:
        print(json.dumps(payload, indent=2))
    else:
        for item in results:
            print(f"[{'PASS' if item['passed'] else 'FAIL'}] {item['name']}: {item['detail']}")
        print(f"\n{passed}/{len(results)} checks passed")
    return 0 if payload["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
