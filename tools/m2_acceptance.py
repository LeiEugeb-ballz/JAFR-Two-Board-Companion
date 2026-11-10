#!/usr/bin/env python3
"""Lightweight Jaf-R M2 acceptance harness. Never resets or flashes hardware."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TONE_SCRIPT = ROOT / "S31-Live-Brain" / "pc_gateway" / "send_m2_tone.py"
LOSS_COUNTERS = ("invalid_frames", "queue_drops", "sequence_gaps", "playback_drops")


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def get_json(url: str, timeout: float = 5.0) -> dict[str, Any]:
    request = urllib.request.Request(url, headers={"Cache-Control": "no-store"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def post_form(url: str, values: dict[str, str], timeout: float = 5.0) -> dict[str, Any]:
    body = urllib.parse.urlencode(values).encode("ascii")
    request = urllib.request.Request(
        url,
        data=body,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def git_state(path: Path) -> dict[str, Any]:
    def run(*args: str) -> str:
        completed = subprocess.run(
            ["git", "-C", str(path), *args],
            text=True,
            capture_output=True,
            check=False,
        )
        return (completed.stdout or completed.stderr).strip()

    return {
        "root": str(path),
        "revision": run("rev-parse", "HEAD"),
        "branch": run("branch", "--show-current"),
        "status": run("status", "--short", "--branch"),
        "submodules": run("submodule", "status"),
    }


def gate(name: str, passed: bool, observed: Any, expected: str) -> dict[str, Any]:
    return {
        "name": name,
        "pass": bool(passed),
        "observed": observed,
        "expected": expected,
    }


def counter_delta(before: dict[str, Any], after: dict[str, Any], key: str) -> int:
    return int(after.get(key, 0)) - int(before.get(key, 0))


def evaluate_baseline(
    s31_before: dict[str, Any],
    s31_after: dict[str, Any],
    dual_before: dict[str, Any],
    dual_after: dict[str, Any],
    min_mtu: int,
    min_fps: float,
    min_frames: int,
    min_internal: int,
    min_stack: int,
) -> list[dict[str, Any]]:
    frame_delta = counter_delta(dual_before, dual_after, "eye_frames")
    checks = [
        gate("s31_ble_initialized", s31_after.get("ble_audio_initialized") is True, s31_after.get("ble_audio_initialized"), "true"),
        gate("s31_ble_connected", s31_after.get("ble_audio_connected") is True, s31_after.get("ble_audio_connected"), "true"),
        gate("s31_ble_ready", s31_after.get("ble_audio_ready") is True, s31_after.get("ble_audio_ready"), "true"),
        gate("s31_ble_mtu", int(s31_after.get("ble_audio_mtu", 0)) >= min_mtu, s31_after.get("ble_audio_mtu"), f">= {min_mtu}"),
        gate("s31_espnow_ready", s31_after.get("espnow_ready") is True, s31_after.get("espnow_ready"), "true"),
        gate("s31_ble_drop_delta", counter_delta(s31_before, s31_after, "ble_audio_dropped") == 0, counter_delta(s31_before, s31_after, "ble_audio_dropped"), "== 0"),
        gate("s31_ble_failure_delta", counter_delta(s31_before, s31_after, "ble_audio_failures") == 0, counter_delta(s31_before, s31_after, "ble_audio_failures"), "== 0"),
        gate("dualeye_initialized", dual_after.get("initialized") is True, dual_after.get("initialized"), "true"),
        gate("dualeye_connected", dual_after.get("connected") is True, dual_after.get("connected"), "true"),
        gate("dualeye_mtu", int(dual_after.get("mtu", 0)) >= min_mtu, dual_after.get("mtu"), f">= {min_mtu}"),
        gate("dualeye_eye_fps", float(dual_after.get("eye_fps", 0.0)) >= min_fps, dual_after.get("eye_fps"), f">= {min_fps}"),
        gate("dualeye_frame_delta", frame_delta >= min_frames, frame_delta, f">= {min_frames}"),
        gate("dualeye_allocation_failures", int(dual_after.get("allocation_failures", -1)) == 0, dual_after.get("allocation_failures"), "== 0"),
        gate("dualeye_internal_minimum", int(dual_after.get("internal_minimum", 0)) >= min_internal, dual_after.get("internal_minimum"), f">= {min_internal}"),
        gate("dualeye_decode_stack", int(dual_after.get("decode_stack_free", 0)) >= min_stack, dual_after.get("decode_stack_free"), f">= {min_stack}"),
        gate("dualeye_server_stack", int(dual_after.get("server_stack_free", 0)) >= min_stack, dual_after.get("server_stack_free"), f">= {min_stack}"),
    ]
    for key in LOSS_COUNTERS:
        delta = counter_delta(dual_before, dual_after, key)
        checks.append(gate(f"dualeye_{key}_delta", delta == 0, delta, "== 0"))
    return checks


def run_baseline(args: argparse.Namespace, out: Path) -> dict[str, Any]:
    s31_url = f"http://{args.s31_host}/api/status"
    dual_url = f"http://{args.dualeye_host}:{args.dualeye_port}/ble/audio/status"
    s31_before = get_json(s31_url, args.timeout)
    dual_before = get_json(dual_url, args.timeout)
    time.sleep(args.sample_seconds)
    s31_after = get_json(s31_url, args.timeout)
    dual_after = get_json(dual_url, args.timeout)
    checks = evaluate_baseline(
        s31_before,
        s31_after,
        dual_before,
        dual_after,
        args.min_mtu,
        args.min_fps,
        args.min_frames,
        args.min_internal,
        args.min_stack,
    )
    result = {
        "phase": "baseline",
        "started_utc": utc_now(),
        "sample_seconds": args.sample_seconds,
        "s31_before": s31_before,
        "s31_after": s31_after,
        "dualeye_before": dual_before,
        "dualeye_after": dual_after,
        "checks": checks,
        "pass": all(item["pass"] for item in checks),
    }
    write_json(out / "baseline.json", result)
    return result


def run_stress(args: argparse.Namespace, out: Path) -> dict[str, Any]:
    dual_url = f"http://{args.dualeye_host}:{args.dualeye_port}/ble/audio/status"
    before = get_json(dual_url, args.timeout)
    started = time.perf_counter()
    failures: list[str] = []
    for index in range(args.stress_requests):
        try:
            get_json(dual_url, args.timeout)
        except Exception as exc:  # preserve every failed request
            failures.append(f"{index + 1}: {exc}")
    time.sleep(2.0)
    after = get_json(dual_url, args.timeout)
    elapsed = time.perf_counter() - started
    frame_delta = counter_delta(before, after, "eye_frames")
    checks = [
        gate("stress_http_failures", not failures, len(failures), "== 0"),
        gate("stress_eye_frames_advance", frame_delta > 0, frame_delta, "> 0"),
        gate("stress_eye_fps", float(after.get("eye_fps", 0.0)) >= args.min_fps, after.get("eye_fps"), f">= {args.min_fps}"),
        gate("stress_allocation_failures", int(after.get("allocation_failures", -1)) == 0, after.get("allocation_failures"), "== 0"),
        gate("stress_internal_minimum", int(after.get("internal_minimum", 0)) >= args.min_internal, after.get("internal_minimum"), f">= {args.min_internal}"),
        gate("stress_connected", after.get("connected") is True, after.get("connected"), "true"),
        gate("stress_mtu", int(after.get("mtu", 0)) >= args.min_mtu, after.get("mtu"), f">= {args.min_mtu}"),
    ]
    for key in LOSS_COUNTERS:
        delta = counter_delta(before, after, key)
        checks.append(gate(f"stress_{key}_delta", delta == 0, delta, "== 0"))
    result = {
        "phase": "stress",
        "started_utc": utc_now(),
        "requests": args.stress_requests,
        "elapsed_seconds": round(elapsed, 3),
        "request_failures": failures,
        "before": before,
        "after": after,
        "checks": checks,
        "pass": all(item["pass"] for item in checks),
    }
    write_json(out / "stress.json", result)
    return result


def run_tone(args: argparse.Namespace, out: Path) -> dict[str, Any]:
    tone_script = Path(args.tone_script).resolve()
    if not tone_script.is_file():
        raise FileNotFoundError(f"Tone helper not found: {tone_script}")
    s31_url = f"http://{args.s31_host}/api/status"
    dual_url = f"http://{args.dualeye_host}:{args.dualeye_port}/ble/audio/status"
    s31_before = get_json(s31_url, args.timeout)
    dual_before = get_json(dual_url, args.timeout)
    command = [
        sys.executable,
        str(tone_script),
        "--host",
        args.s31_host,
        "--duration",
        str(args.tone_seconds),
        "--frequency",
        str(args.tone_frequency),
        "--amplitude",
        str(args.tone_amplitude),
    ]
    completed = subprocess.run(command, text=True, capture_output=True, check=False)
    helper_result: dict[str, Any] | None = None
    parse_error: str | None = None
    try:
        helper_result = json.loads(completed.stdout)
    except Exception as exc:
        parse_error = str(exc)
    s31_after = get_json(s31_url, args.timeout)
    dual_after = get_json(dual_url, args.timeout)
    frames_sent = int((helper_result or {}).get("frames_sent", 0))
    expected_frames = max(1, int(args.tone_seconds * 45))
    s31_sent_delta = counter_delta(s31_before, s31_after, "ble_audio_sent")
    dual_received_delta = counter_delta(dual_before, dual_after, "received_frames")
    dual_decoded_delta = counter_delta(dual_before, dual_after, "decoded_frames")
    eye_frame_delta = counter_delta(dual_before, dual_after, "eye_frames")
    checks = [
        gate("tone_helper_exit", completed.returncode == 0, completed.returncode, "== 0"),
        gate("tone_helper_json", parse_error is None, parse_error, "valid JSON"),
        gate("tone_frames_sent", frames_sent >= expected_frames, frames_sent, f">= {expected_frames}"),
        gate("tone_s31_sent", s31_sent_delta >= max(1, frames_sent - 2), s31_sent_delta, f">= {max(1, frames_sent - 2)}"),
        gate("tone_s31_drop_delta", counter_delta(s31_before, s31_after, "ble_audio_dropped") == 0, counter_delta(s31_before, s31_after, "ble_audio_dropped"), "== 0"),
        gate("tone_s31_failure_delta", counter_delta(s31_before, s31_after, "ble_audio_failures") == 0, counter_delta(s31_before, s31_after, "ble_audio_failures"), "== 0"),
        gate("tone_dualeye_received", dual_received_delta >= max(1, frames_sent - 2), dual_received_delta, f">= {max(1, frames_sent - 2)}"),
        gate("tone_dualeye_decoded", dual_decoded_delta >= max(1, frames_sent - 2), dual_decoded_delta, f">= {max(1, frames_sent - 2)}"),
        gate("tone_eye_frames_advance", eye_frame_delta >= max(1, int(args.tone_seconds * 10)), eye_frame_delta, f">= {max(1, int(args.tone_seconds * 10))}"),
        gate("tone_eye_fps", float(dual_after.get("eye_fps", 0.0)) >= args.min_fps, dual_after.get("eye_fps"), f">= {args.min_fps}"),
        gate("tone_allocation_failures", int(dual_after.get("allocation_failures", -1)) == 0, dual_after.get("allocation_failures"), "== 0"),
        gate("tone_internal_minimum", int(dual_after.get("internal_minimum", 0)) >= args.min_internal, dual_after.get("internal_minimum"), f">= {args.min_internal}"),
    ]
    for key in LOSS_COUNTERS:
        delta = counter_delta(dual_before, dual_after, key)
        checks.append(gate(f"tone_{key}_delta", delta == 0, delta, "== 0"))
    result = {
        "phase": "tone",
        "started_utc": utc_now(),
        "command": command,
        "helper_stdout": completed.stdout,
        "helper_stderr": completed.stderr,
        "helper_result": helper_result,
        "parse_error": parse_error,
        "s31_before": s31_before,
        "s31_after": s31_after,
        "dualeye_before": dual_before,
        "dualeye_after": dual_after,
        "checks": checks,
        "pass": all(item["pass"] for item in checks),
    }
    write_json(out / "tone.json", result)
    return result


def run_route(args: argparse.Namespace, out: Path) -> dict[str, Any]:
    """Exercise the prepared output selector and restore the original saved route."""
    status_url = f"http://{args.s31_host}/api/status"
    audio_url = f"http://{args.s31_host}/api/audio"
    original = get_json(status_url, args.timeout)
    original_route = original.get("audio_route", "auto")
    snapshots: dict[str, dict[str, Any]] = {}
    restore_error: str | None = None
    try:
        for route in ("s31", "dualeye", "auto"):
            post_form(audio_url, {"route": route}, args.timeout)
            time.sleep(0.25)
            snapshots[route] = get_json(status_url, args.timeout)
    finally:
        try:
            post_form(audio_url, {"route": original_route}, args.timeout)
        except Exception as exc:
            restore_error = str(exc)

    checks = [
        gate("route_s31_saved", snapshots.get("s31", {}).get("audio_route") == "s31", snapshots.get("s31", {}).get("audio_route"), "s31"),
        gate("route_s31_active", snapshots.get("s31", {}).get("audio_output") == "s31", snapshots.get("s31", {}).get("audio_output"), "s31"),
        gate("route_dualeye_saved", snapshots.get("dualeye", {}).get("audio_route") == "dualeye", snapshots.get("dualeye", {}).get("audio_route"), "dualeye"),
        gate("route_dualeye_ready", snapshots.get("dualeye", {}).get("ble_audio_ready") is True, snapshots.get("dualeye", {}).get("ble_audio_ready"), "true"),
        gate("route_dualeye_active", snapshots.get("dualeye", {}).get("audio_output") == "dualeye", snapshots.get("dualeye", {}).get("audio_output"), "dualeye"),
        gate("route_auto_saved", snapshots.get("auto", {}).get("audio_route") == "auto", snapshots.get("auto", {}).get("audio_route"), "auto"),
        gate("route_auto_prefers_dualeye", snapshots.get("auto", {}).get("audio_output") == "dualeye", snapshots.get("auto", {}).get("audio_output"), "dualeye"),
        gate("route_original_restored", restore_error is None, restore_error, f"restore {original_route}"),
    ]
    result = {
        "phase": "route",
        "started_utc": utc_now(),
        "original_route": original_route,
        "snapshots": snapshots,
        "restore_error": restore_error,
        "checks": checks,
        "pass": all(item["pass"] for item in checks),
        "physical_audio_confirmation": "pending",
    }
    write_json(out / "route.json", result)
    return result


def write_operator_checklist(path: Path) -> None:
    path.write_text(
        """# Jaf-R M2 operator acceptance

- [ ] Audio is clear through the DualEye speaker during the tone/voice phase.
- [ ] Both eyes remain visibly animated during audio playback.
- [ ] Listening/thinking/speaking expressions arrive over ESP-NOW.
- [ ] Left display touch remains responsive.
- [ ] Right display touch remains responsive.
- [ ] S31 boot sound still plays locally.
- [ ] S31 local speaker is used when BLE/DualEye is unavailable.
- [ ] WebUI can explicitly select S31 primary output and the next response uses it. (Prepared source; active firmware validation pending.)
- [ ] WebUI can explicitly select DualEye output and retains the selection. (Prepared source; active firmware validation pending.)

Operator notes:
""",
        encoding="utf-8",
    )


def self_test() -> int:
    s31_before = {"ble_audio_dropped": 0, "ble_audio_failures": 0}
    s31_after = {
        "ble_audio_initialized": True,
        "ble_audio_connected": True,
        "ble_audio_ready": True,
        "ble_audio_mtu": 247,
        "ble_audio_dropped": 0,
        "ble_audio_failures": 0,
        "espnow_ready": True,
    }
    dual_before = {"eye_frames": 100, **{key: 0 for key in LOSS_COUNTERS}}
    dual_after = {
        "initialized": True,
        "connected": True,
        "mtu": 247,
        "eye_fps": 15.6,
        "eye_frames": 164,
        "allocation_failures": 0,
        "internal_minimum": 20000,
        "decode_stack_free": 2200,
        "server_stack_free": 1900,
        **{key: 0 for key in LOSS_COUNTERS},
    }
    passing = evaluate_baseline(s31_before, s31_after, dual_before, dual_after, 179, 14.5, 50, 16384, 1024)
    if not all(item["pass"] for item in passing):
        raise AssertionError("known-good fixture failed")
    dual_after["allocation_failures"] = 1
    failing = evaluate_baseline(s31_before, s31_after, dual_before, dual_after, 179, 14.5, 50, 16384, 1024)
    if all(item["pass"] for item in failing):
        raise AssertionError("known-bad fixture passed")
    print("self-test PASS: known-good accepted; allocation regression rejected")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", choices=("baseline", "stress", "tone", "route", "all"), default="baseline")
    parser.add_argument("--s31-host", default="192.168.88.242")
    parser.add_argument("--dualeye-host", default="192.168.88.145")
    parser.add_argument("--dualeye-port", type=int, default=3233)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--sample-seconds", type=float, default=4.0)
    parser.add_argument("--stress-requests", type=int, default=50)
    parser.add_argument("--tone-seconds", type=float, default=4.0)
    parser.add_argument("--tone-frequency", type=float, default=440.0)
    parser.add_argument("--tone-amplitude", type=int, default=2400)
    parser.add_argument("--tone-script", default=str(DEFAULT_TONE_SCRIPT))
    parser.add_argument("--min-mtu", type=int, default=179)
    parser.add_argument("--min-fps", type=float, default=14.5)
    parser.add_argument("--min-frames", type=int, default=50)
    parser.add_argument("--min-internal", type=int, default=16 * 1024)
    parser.add_argument("--min-stack", type=int, default=1024)
    parser.add_argument("--output")
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.self_test:
        return self_test()
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = Path(args.output).resolve() if args.output else ROOT / "evidence" / "m2-acceptance" / timestamp
    out.mkdir(parents=True, exist_ok=False)
    manifest = {
        "schema_version": 1,
        "started_utc": utc_now(),
        "phase": args.phase,
        "s31_status": f"http://{args.s31_host}/api/status",
        "dualeye_status": f"http://{args.dualeye_host}:{args.dualeye_port}/ble/audio/status",
        "tone_transport": f"udp://{args.s31_host}:5001",
        "reset_performed": False,
        "flash_performed": False,
        "thresholds": {
            "min_mtu": args.min_mtu,
            "min_fps": args.min_fps,
            "min_frames": args.min_frames,
            "min_internal": args.min_internal,
            "min_stack": args.min_stack,
        },
        "repositories": [
            git_state(ROOT),
            git_state(ROOT / "S31-Live-Brain"),
            git_state(ROOT / "DualEye-Live-Graphics"),
        ],
    }
    write_json(out / "manifest.json", manifest)
    write_operator_checklist(out / "operator-checklist.md")

    phases = ("baseline", "stress", "tone", "route") if args.phase == "all" else (args.phase,)
    results: list[dict[str, Any]] = []
    try:
        for phase in phases:
            if phase == "baseline":
                results.append(run_baseline(args, out))
            elif phase == "stress":
                results.append(run_stress(args, out))
            elif phase == "tone":
                results.append(run_tone(args, out))
            elif phase == "route":
                results.append(run_route(args, out))
    except Exception as exc:
        results.append({"phase": "harness", "pass": False, "error": str(exc), "checks": []})

    checks = [check for result in results for check in result.get("checks", [])]
    passed = bool(results) and all(result.get("pass") is True for result in results)
    summary = {
        "completed_utc": utc_now(),
        "automated_pass": passed,
        "phases": [{"phase": result.get("phase"), "pass": result.get("pass"), "error": result.get("error")} for result in results],
        "checks_passed": sum(1 for item in checks if item.get("pass")),
        "checks_failed": sum(1 for item in checks if not item.get("pass")),
        "physical_operator_status": "pending",
        "speaker_selection_status": (
            "automated control-plane pass; physical audio pending"
            if any(result.get("phase") == "route" and result.get("pass") for result in results)
            else "not run or failed; active firmware validation pending"
        ),
        "evidence_directory": str(out),
    }
    write_json(out / "summary.json", summary)
    print(json.dumps(summary, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())

