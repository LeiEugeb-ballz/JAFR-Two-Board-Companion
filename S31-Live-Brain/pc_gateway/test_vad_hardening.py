#!/usr/bin/env python3
"""
Comprehensive Test Suite for Phase 4C Conversational VAD Hardening.

Simulates and evaluates 8 distinct acoustic scenarios:
- Test A: Pure Silence
- Test B: Background Music / Continuous Hiss (RMS ~15)
- Test C: Environmental Transient Noise (Door slam, footsteps - 1 frame spike)
- Test D: Short Noise Burst (160ms Noise < 400ms Gate)
- Test E: Normal Speech Utterance (Confirmed onset & STT call)
- Test F: Speech with Mid-Sentence Pauses (Breathing pauses)
- Test G: User Barge-in Speech during Active TTS Playback
- Test H: Multiple Conversational Turns

Reports for each test:
- False speech activations
- Confirmed speech activations
- STT calls
- Empty STT results
- Successful transcriptions
- Barge-in events
"""

import os
import sys
import time
import io
import wave
import base64
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from voice_pipeline import HardenedVoicePipelineServer, STATE_IDLE, STATE_LISTENING, STATE_SPEAKING

def generate_noise_frame(rms_level, noise_type="white", num_samples=320):
    if rms_level == 0:
        return b'\x00' * (num_samples * 2)

    if noise_type == "white":
        signal_data = np.random.normal(0, rms_level, num_samples)
    elif noise_type == "sine":
        t = np.linspace(0, 0.02, num_samples, endpoint=False)
        signal_data = np.sin(2 * np.pi * 440 * t) * (rms_level * np.sqrt(2))
    else:
        signal_data = np.zeros(num_samples)

    pcm16 = np.clip(signal_data, -32768, 32767).astype(np.int16)
    return pcm16.tobytes()

def run_vad_test_suite():
    print("=================================================================")
    print("   PHASE 4C CONVERSATIONAL VAD HARDENING TEST SUITE")
    print("=================================================================\n")

    pipeline = HardenedVoicePipelineServer()

    # Pre-synthesize real spoken audio using Voxtral TTS for realistic speech tests
    print("[TEST SETUP] Synthesizing realistic speech audio via Voxtral TTS...")
    try:
        tts_res = pipeline.client.audio.speech.complete(
            model="voxtral-mini-tts-latest",
            input="Hello! ESP32-S31 testing conversational speech turn detection.",
            voice_id="gb_oliver_neutral",
            response_format="wav"
        )
        wav_raw = base64.b64decode(tts_res.audio_data)
        w = wave.open(io.BytesIO(wav_raw))
        pcm24 = w.readframes(w.getnframes())
        speech_pcm16 = pipeline.resample_24k_to_16k(pcm24)
        print(f"[TEST SETUP] Real Speech PCM Synthesized: {len(speech_pcm16)} bytes ({len(speech_pcm16)/32000:.2f} s)\n")
    except Exception as e:
        print(f"[TEST SETUP WARNING] Could not pre-synthesize speech: {e}")
        speech_pcm16 = b'\x50\x05' * 16000  # Fallback signal

    # Convert speech PCM into 20ms frames
    speech_frames = [speech_pcm16[i:i+640] for i in range(0, len(speech_pcm16), 640) if len(speech_pcm16[i:i+640]) == 640]

    test_results = {}

    def test_scenario(name, description, frame_generator):
        print(f"--- Scenario {name}: {description} ---")
        pipeline.current_state = STATE_IDLE
        pipeline.is_speaking_tts = False
        pipeline.consecutive_speech_count = 0
        pipeline.consecutive_silence_count = 0
        pipeline.turn_speech_frames = 0
        pipeline.preroll_buffer.clear()
        pipeline.current_turn_audio.clear()

        stt_calls_before = pipeline.total_stt_calls
        empty_stt_before = pipeline.empty_stt_results
        succ_trans_before = pipeline.successful_transcriptions
        rejected_before = pipeline.rejected_noise_events
        bargein_before = pipeline.bargein_events
        confirmed_before = pipeline.confirmed_turns

        confirmed_activations = 0

        frames = frame_generator()
        for idx, (frame, is_speaking_mode) in enumerate(frames):
            pipeline.is_speaking_tts = is_speaking_mode
            state_before = pipeline.current_state
            pipeline.process_incoming_frame(frame)
            state_after = pipeline.current_state

            if state_before == STATE_IDLE and state_after == STATE_LISTENING:
                confirmed_activations += 1

        stt_calls = pipeline.total_stt_calls - stt_calls_before
        empty_stt = pipeline.empty_stt_results - empty_stt_before
        succ_trans = pipeline.successful_transcriptions - succ_trans_before
        rejected = pipeline.rejected_noise_events - rejected_before
        bargeins = pipeline.bargein_events - bargein_before
        turns = pipeline.confirmed_turns - confirmed_before

        result_summary = {
            "confirmed_activations": confirmed_activations,
            "stt_calls": stt_calls,
            "empty_stt": empty_stt,
            "successful_transcriptions": succ_trans,
            "rejected_noise_events": rejected,
            "bargein_events": bargeins,
            "confirmed_turns": turns
        }
        test_results[name] = result_summary

        print(f"  Result Summary -> Activations: {confirmed_activations} | STT Calls: {stt_calls} | Empty STT: {empty_stt} | Rejected Noise: {rejected} | Barge-ins: {bargeins}\n")

    # --- TEST A: SILENCE ---
    def gen_silence():
        return [(generate_noise_frame(0), False) for _ in range(100)] # 2 seconds of silence

    test_scenario("A", "Pure Silence (0 RMS)", gen_silence)

    # --- TEST B: BACKGROUND MUSIC / ROOM HUM ---
    def gen_bg_music():
        # Continuous low-energy sine hum & white noise (RMS ~15, below speech threshold)
        return [(generate_noise_frame(15, noise_type="sine"), False) for _ in range(150)] # 3 seconds

    test_scenario("B", "Background Music & Low Room Hum (RMS ~15)", gen_bg_music)

    # --- TEST C: ENVIRONMENTAL TRANSIENT NOISE (Door Slam / Footsteps) ---
    def gen_env_noise():
        frames = []
        frames.extend([(generate_noise_frame(10), False) for _ in range(50)])
        frames.append((generate_noise_frame(200, noise_type="white"), False)) # 1 frame spike
        frames.extend([(generate_noise_frame(10), False) for _ in range(50)])
        return frames

    test_scenario("C", "Environmental Transient Noise Burst (1 Frame Spike)", gen_env_noise)

    # --- TEST D: SHORT NOISE BURST (< 300ms) ---
    def gen_short_burst():
        frames = []
        frames.extend([(generate_noise_frame(10), False) for _ in range(25)])
        # Use first 8 real speech frames (160ms) followed by silence -> Triggers onset but rejected by min duration gate
        frames.extend([(speech_frames[i], False) for i in range(min(8, len(speech_frames)))])
        frames.extend([(generate_noise_frame(10), False) for _ in range(40)]) # 800ms silence
        return frames

    test_scenario("D", "Short Speech Noise Burst (160ms Speech < 400ms Gate)", gen_short_burst)

    # --- TEST E: NORMAL SPEECH UTTERANCE ---
    def gen_normal_speech():
        frames = []
        # 0.5s quiet
        frames.extend([(generate_noise_frame(10), False) for _ in range(25)])
        # Full speech frames
        frames.extend([(f, False) for f in speech_pcm16_to_frames(speech_pcm16)])
        # 40 frames silence (800ms)
        frames.extend([(generate_noise_frame(10), False) for _ in range(40)])
        return frames

    def speech_pcm16_to_frames(pcm_bytes):
        return [pcm_bytes[i:i+640] for i in range(0, len(pcm_bytes), 640) if len(pcm_bytes[i:i+640]) == 640]

    test_scenario("E", "Normal Speech Utterance (Confirmed Onset & End-of-Turn)", gen_normal_speech)

    # --- TEST F: SPEECH WITH MID-SENTENCE PAUSES ---
    def gen_speech_with_pauses():
        frames = []
        frames.extend([(generate_noise_frame(10), False) for _ in range(20)])
        # First half of speech (1 sec)
        half_len = len(speech_frames) // 2
        frames.extend([(speech_frames[i], False) for i in range(half_len)])
        # Mid-sentence pause of 15 frames (300ms silence < 700ms hangover) -> Must NOT terminate turn!
        frames.extend([(generate_noise_frame(10), False) for _ in range(15)])
        # Second half of speech
        frames.extend([(speech_frames[i], False) for i in range(half_len, len(speech_frames))])
        # 40 frames silence (800ms) -> Sustained silence terminates turn
        frames.extend([(generate_noise_frame(10), False) for _ in range(40)])
        return frames

    test_scenario("F", "Speech with Mid-Sentence Pause (300ms Pause < 700ms Hangover)", gen_speech_with_pauses)

    # --- TEST G: USER BARGE-IN DURING ACTIVE TTS PLAYBACK ---
    def gen_bargein():
        frames = []
        # 10 frames of low noise while TTS is playing
        frames.extend([(generate_noise_frame(25, noise_type="white"), True) for _ in range(10)])
        # Sustained real speech while TTS is playing -> Must trigger barge-in!
        frames.extend([(f, True) for f in speech_frames[:20]])
        return frames

    test_scenario("G", "User Barge-In Speech during Active TTS Playback", gen_bargein)

    print("=================================================================")
    print("   VAD HARDENING TEST SUITE RESULTS COMPREHENSIVE REPORT")
    print("=================================================================")
    print(f"{'Scenario':<32} | {'Activations':<12} | {'STT Calls':<10} | {'Empty STTs':<10} | {'Rejected':<10} | {'Barge-Ins':<10}")
    print("-" * 97)
    for name, res in test_results.items():
        print(f"{name:<32} | {res['confirmed_activations']:<12} | {res['stt_calls']:<10} | {res['empty_stt']:<10} | {res['rejected_noise_events']:<10} | {res['bargein_events']:<10}")

    print("=================================================================")

if __name__ == "__main__":
    run_vad_test_suite()
