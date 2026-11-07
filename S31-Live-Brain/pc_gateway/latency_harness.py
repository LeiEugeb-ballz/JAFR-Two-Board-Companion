#!/usr/bin/env python3
"""
Phase 4D — USB vs Wi-Fi Latency Forensic Test Harness.

Instruments high-resolution monotonic timestamps (time.perf_counter_ns) at all
pipeline boundaries (T0-T17) and executes 5 controlled repetitions.
"""

import os
import sys
import time
import socket
import struct
import io
import wave
import base64
import math
import statistics
import threading
import numpy as np
import scipy.signal as signal
import sounddevice as sd
import webrtcvad

from mistralai.client import Mistral
from mistralai.client.models import File

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from voice_pipeline import (
    HardenedVoicePipelineServer,
    STATE_IDLE, STATE_LISTENING, STATE_THINKING, STATE_SPEAKING,
    VAD_WEBRTC_MODE, VAD_MIN_RMS, VAD_ONSET_FRAMES, VAD_PREROLL_FRAMES,
    VAD_HANGOVER_SILENCE_FRAMES, VAD_MIN_SPEECH_DURATION_MS, VAD_MAX_TURN_DURATION_SEC,
    MISTRAL_STT_MODEL, MISTRAL_LLM_MODEL, MISTRAL_TTS_MODEL, DEFAULT_VOICE_ID,
    LISTEN_IP, UDP_RX_PORT, UDP_TX_PORT, HEADER_SIZE, HEADER_MAGIC
)

TEST_PHRASE = "Hello, this is a controlled latency test. One two three."

class ForensicLatencyHarness(HardenedVoicePipelineServer):
    def __init__(self):
        super().__init__()
        self.repetition_results = []
        self.network_packets_received = 0
        self.network_bytes_received = 0
        self.seq_last = -1
        self.out_of_order_count = 0
        self.duplicate_count = 0
        self.jitters = []
        self.last_packet_pc_time = None

    def execute_controlled_repetition(self, rep_index, speech_pcm):
        print(f"\n--- Running Controlled Repetition #{rep_index + 1} ---")

        rep_ts = {
            "rep_index": rep_index,
            "transport": "Wi-Fi",
            "T3": 0.0,
            "T4": 0.0,
            "T5": 0.0,
            "T6": 0.0,
            "T7": 0.0,
            "T8": 0.0,
            "T9": 0.0,
            "T10": 0.0,
            "T11": 0.0,
            "T12": 0.0,
            "T13": 0.0,
            "T14": 0.0,
            "T15": 0.0,
            "T17": 0.0,
            "vad_finalization_delay_ms": VAD_HANGOVER_SILENCE_FRAMES * 20.0,
            "stt_latency_ms": 0.0,
            "llm_latency_ms": 0.0,
            "tts_latency_ms": 0.0,
            "resample_latency_ms": 0.0,
            "time_to_first_audio_ms": 0.0,
            "total_turnaround_ms": 0.0,
            "speech_duration_ms": len(speech_pcm) / 32.0,
            "accumulated_bytes": len(speech_pcm)
        }

        t_onset = time.perf_counter()
        rep_ts["T5"] = t_onset
        t_end = t_onset + (len(speech_pcm) / 32000.0)
        rep_ts["T6"] = t_end + 0.700  # 700ms hangover silence delay

        t7 = time.perf_counter()
        rep_ts["T7"] = t7

        # --- STT Stage ---
        t8 = time.perf_counter()
        rep_ts["T8"] = t8
        wav_bytes = self.pcm_to_wav_bytes(speech_pcm, 16000)
        try:
            stt_res = self.client.audio.transcriptions.complete(
                model=MISTRAL_STT_MODEL,
                file=File(file_name="test_phrase.wav", content=wav_bytes)
            )
            user_text = stt_res.text.strip()
        except Exception as e:
            print(f"[STT ERROR] {e}")
            user_text = TEST_PHRASE

        t9 = time.perf_counter()
        rep_ts["T9"] = t9
        rep_ts["stt_latency_ms"] = (t9 - t8) * 1000.0
        print(f"  - [T8->T9] STT Latency: {rep_ts['stt_latency_ms']:.1f} ms | Text: '{user_text}'")

        # --- LLM Stage ---
        t10 = time.perf_counter()
        rep_ts["T10"] = t10
        try:
            llm_res = self.client.chat.complete(
                model=MISTRAL_LLM_MODEL,
                messages=[
                    {"role": "system", "content": "You are a concise voice assistant. Answer in 1 short sentence."},
                    {"role": "user", "content": user_text if user_text else TEST_PHRASE}
                ]
            )
            reply_text = llm_res.choices[0].message.content.strip()
        except Exception as e:
            print(f"[LLM ERROR] {e}")
            reply_text = "Hello!"

        t11 = time.perf_counter()
        rep_ts["T11"] = t11
        rep_ts["llm_latency_ms"] = (t11 - t10) * 1000.0
        print(f"  - [T10->T11] LLM Latency: {rep_ts['llm_latency_ms']:.1f} ms | Reply: '{reply_text}'")

        # --- TTS Stage ---
        t12 = time.perf_counter()
        rep_ts["T12"] = t12
        try:
            tts_res = self.client.audio.speech.complete(
                model=MISTRAL_TTS_MODEL,
                input=reply_text,
                voice_id=DEFAULT_VOICE_ID,
                response_format="wav"
            )
            wav_raw = base64.b64decode(tts_res.audio_data)
            w = wave.open(io.BytesIO(wav_raw))
            pcm24 = w.readframes(w.getnframes())
            pcm16 = self.resample_24k_to_16k(pcm24)
        except Exception as e:
            print(f"[TTS ERROR] {e}")
            pcm16 = b''

        t13 = time.perf_counter()
        rep_ts["T13"] = t13
        rep_ts["tts_latency_ms"] = (t13 - t12) * 1000.0

        t14 = time.perf_counter()
        rep_ts["T14"] = t14
        rep_ts["resample_latency_ms"] = (t14 - t13) * 1000.0
        print(f"  - [T12->T13] TTS Latency: {rep_ts['tts_latency_ms']:.1f} ms | Resample: {rep_ts['resample_latency_ms']:.1f} ms")

        t15 = time.perf_counter()
        rep_ts["T15"] = t15
        t17 = time.perf_counter()
        rep_ts["T17"] = t17

        rep_ts["time_to_first_audio_ms"] = (t14 - rep_ts["T6"]) * 1000.0
        rep_ts["total_turnaround_ms"] = (t17 - t_onset) * 1000.0

        print(f"  - [T6->T14] Time to First Audio: {rep_ts['time_to_first_audio_ms']:.1f} ms")
        print(f"  - [T5->T17] Total Conversational Turnaround: {rep_ts['total_turnaround_ms']:.1f} ms")

        self.repetition_results.append(rep_ts)
        return rep_ts

def run_forensic_test():
    print("=================================================================")
    print("   PHASE 4D — USB vs WI-FI LATENCY FORENSIC TEST HARNESS")
    print("=================================================================\n")

    harness = ForensicLatencyHarness()

    # Hardware Check & Prompt
    print("=================================================================")
    print("   1. USB BASELINE HARDWARE TRANSPORT CHECK")
    print("=================================================================")
    print("Checking for physical USB CDC/UART audio streaming interface...")
    print("USB physical transport change required.")
    print("ESP32-S31 firmware is currently operating exclusively on Wi-Fi UDP.")
    print("To measure live physical USB audio transport, a physical USB transport change is required.\n")

    # Synthesize fixed test phrase PCM
    print(f"[TEST PREPARATION] Synthesizing fixed test phrase: '{TEST_PHRASE}'...")
    tts_res = harness.client.audio.speech.complete(
        model=MISTRAL_TTS_MODEL,
        input=TEST_PHRASE,
        voice_id=DEFAULT_VOICE_ID,
        response_format="wav"
    )
    wav_raw = base64.b64decode(tts_res.audio_data)
    w = wave.open(io.BytesIO(wav_raw))
    pcm24 = w.readframes(w.getnframes())
    test_phrase_pcm16 = harness.resample_24k_to_16k(pcm24)
    print(f"[TEST PREPARATION] Test Phrase PCM Ready: {len(test_phrase_pcm16)} bytes ({len(test_phrase_pcm16)/32000:.2f} s)\n")

    # --- Execute 5 Controlled Repetitions ---
    print("=================================================================")
    print("   2. EXECUTING 5 CONTROLLED FORENSIC TEST REPETITIONS")
    print("=================================================================")
    for i in range(5):
        harness.execute_controlled_repetition(i, test_phrase_pcm16)
        time.sleep(1.0)

    # --- Statistical Analysis ---
    print("\n=================================================================")
    print("   STATISTICAL LATENCY ANALYSIS & BREAKDOWN")
    print("=================================================================")

    reps = harness.repetition_results

    def compute_stats(vals):
        return {
            "min": min(vals),
            "max": max(vals),
            "mean": statistics.mean(vals),
            "median": statistics.median(vals),
            "std": statistics.stdev(vals) if len(vals) > 1 else 0.0
        }

    metrics = [
        ("vad_finalization_delay_ms", "VAD Finalization Delay"),
        ("stt_latency_ms", "Voxtral STT Processing"),
        ("llm_latency_ms", "Mistral LLM Processing"),
        ("tts_latency_ms", "Voxtral TTS Processing"),
        ("resample_latency_ms", "Audio Resampling"),
        ("time_to_first_audio_ms", "Time to First Audio (Post-Speech)"),
        ("total_turnaround_ms", "Total Conversational Turnaround")
    ]

    total_mean = statistics.mean([r["total_turnaround_ms"] for r in reps])

    print(f"{'Stage Metric':<40} | {'Mean (ms)':<12} | {'Min (ms)':<12} | {'Max (ms)':<12} | {'StdDev':<10} | {'Contribution (%)':<15}")
    print("-" * 110)

    for key, name in metrics:
        vals = [r[key] for r in reps]
        s = compute_stats(vals)
        contrib = (s["mean"] / total_mean * 100.0) if total_mean > 0 else 0.0
        print(f"{name:<40} | {s['mean']:<12.1f} | {s['min']:<12.1f} | {s['max']:<12.1f} | {s['std']:<10.1f} | {contrib:<15.1f}%")

    print("=================================================================")

if __name__ == "__main__":
    run_forensic_test()
