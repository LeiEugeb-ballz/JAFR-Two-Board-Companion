"""Send a deterministic 16 kHz mono test tone to the S31 UDP audio input."""

from __future__ import annotations

import argparse
import json
import math
import socket
import struct
import time
import urllib.request


SAMPLE_RATE = 16_000
SAMPLES_PER_FRAME = 320
FRAME_BYTES = SAMPLES_PER_FRAME * 2
UDP_PORT = 5001


def packet(magic: bytes, flags: int, sequence: int, payload: bytes) -> bytes:
    timestamp_ms = int(time.time() * 1000) & 0xFFFFFFFF
    header = struct.pack(
        "<2sBBIIHH", magic, 1, flags, sequence, timestamp_ms, len(payload), 0
    )
    return header + payload


def status(host: str) -> dict:
    with urllib.request.urlopen(f"http://{host}/api/status", timeout=3) as response:
        return json.load(response)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="192.168.88.242")
    parser.add_argument("--duration", type=float, default=2.0)
    parser.add_argument("--frequency", type=float, default=440.0)
    parser.add_argument("--amplitude", type=int, default=2400)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (args.host, UDP_PORT)
    sequence = 1
    before = status(args.host)

    sock.sendto(packet(b"CO", 0x04, sequence, bytes((3,))), target)
    sequence += 1
    start = time.perf_counter()
    next_frame = start
    sample_index = 0
    frames_sent = 0

    while time.perf_counter() - start < args.duration:
        samples = (
            int(
                args.amplitude
                * math.sin(2.0 * math.pi * args.frequency * (sample_index + i) / SAMPLE_RATE)
            )
            for i in range(SAMPLES_PER_FRAME)
        )
        pcm = struct.pack("<320h", *samples)
        sock.sendto(packet(b"AU", 0x01, sequence, pcm), target)
        sequence = (sequence + 1) & 0xFFFFFFFF
        sample_index += SAMPLES_PER_FRAME
        frames_sent += 1
        next_frame += 0.020
        time.sleep(max(0.0, next_frame - time.perf_counter()))

    sock.sendto(packet(b"CO", 0x04, sequence, bytes((0,))), target)
    time.sleep(1.0)
    after = status(args.host)
    print(json.dumps({"frames_sent": frames_sent, "before": before, "after": after}, indent=2))


if __name__ == "__main__":
    main()
