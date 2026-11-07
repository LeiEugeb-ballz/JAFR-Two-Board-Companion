#!/usr/bin/env python3
"""
PC Voice Gateway for ESP32-S31 Conversational Voice Interface.

Phase 4C — Hardened Conversational VAD Architecture
Features:
- WebRTC VAD (Mode 3 aggressiveness) + Energy RMS bounds + ZCR check
- Onset Hysteresis (Requires N consecutive speech-positive frames)
- Pre-roll Ring Buffer (Retains ~400ms audio prior to confirmed onset)
- Hangover Silence Tolerance (Internal pause tolerance up to 700ms)
- Minimum Speech Duration Gate (Rejects short acoustic noise events < 400ms)
- Maximum Turn Duration Cap (15s timeout limit)
- Hardened Barge-In & Soundcard Self-Trigger Protection
"""

import os
import sys
import time
import socket
import struct
import io
import json
import wave
import base64
import math
import threading
import urllib.error
import urllib.parse
import urllib.request
from collections import deque
import numpy as np
import scipy.signal as signal
import sounddevice as sd
import webrtcvad

from mistralai.client import Mistral
from mistralai.client.models import File

# ==============================================================================
# VAD & CONVERSATIONAL HARDENING CONFIGURATION
# ==============================================================================
VAD_WEBRTC_MODE = 3                # WebRTC VAD Aggressiveness (0-3: 3 = Most aggressive against noise)
VAD_MIN_RMS = 100.0                # Minimum RMS energy threshold to consider for speech (Ignores room noise < 100 RMS)
VAD_ONSET_FRAMES = 6               # Consecutive speech frames required for speech onset (6 * 20ms = 120ms)
VAD_PREROLL_FRAMES = 20            # Pre-roll ring buffer length to retain before onset (20 * 20ms = 400ms)
VAD_HANGOVER_SILENCE_FRAMES = 60   # Silence frames required to declare end-of-turn (60 * 20ms = 1200ms)
VAD_MIN_SPEECH_DURATION_MS = 400   # Minimum confirmed speech duration to submit to STT (400ms)
VAD_MAX_TURN_DURATION_SEC = 180.0  # Maximum turn accumulation cap (3 minutes / 180 seconds)

# --- BARGE-IN & ACOUSTIC PROTECTION CONFIGURATION ---
ENABLE_ACOUSTIC_BARGEIN = False    # Disable acoustic barge-in to prevent 3W speaker audio feedback from self-canceling TTS
VAD_BARGEIN_MIN_RMS = 250.0        # High RMS threshold during assistant TTS playback if acoustic barge-in is enabled
VAD_BARGEIN_ONSET_FRAMES = 12      # Consecutive speech frames required for barge-in (12 * 20ms = 240ms)

# --- NETWORK & AUDIO CONFIGURATION ---
LISTEN_IP = "0.0.0.0"
UDP_RX_PORT = 5000         # Receiving audio from ESP32-S31
UDP_TX_PORT = 5001         # Sending return audio to ESP32-S31
SAMPLE_RATE = 16000        # ESP32 Audio Format: 16kHz
SAMPLE_WIDTH = 2           # 16-bit PCM (2 bytes)
CHANNELS = 1               # Mono
FRAME_SAMPLES = 320        # 20ms @ 16kHz = 320 samples
FRAME_BYTES = 640          # 320 samples * 2 bytes
HEADER_SIZE = 16           # RTP-lite protocol header size
HEADER_MAGIC = b'AU'

# --- AI MODELS ---
MISTRAL_STT_MODEL = "voxtral-mini-latest"
MISTRAL_LLM_MODEL = "mistral-small-latest"
MISTRAL_TTS_MODEL = "voxtral-mini-tts-latest"
DEFAULT_VOICE_ID = "cbe96cf0-85ec-4a10-accb-0b35c93b6dfd"  # Jane · Confident (GB) from PERSONA.md

# --- SYSTEM STATES ---
STATE_IDLE = "IDLE"
STATE_LISTENING = "LISTENING"
STATE_THINKING = "THINKING"
STATE_SPEAKING = "SPEAKING"


class HardenedVoicePipelineServer:
    def __init__(self):
        self.api_key = os.environ.get("MISTRAL_API_KEY")
        if not self.api_key:
            print("CRITICAL ERROR: MISTRAL_API_KEY environment variable not found!")
            sys.exit(1)

        print("[PC GATEWAY] Initializing Mistral API Client & WebRTC VAD...")
        self.client = Mistral()
        self.vad = webrtcvad.Vad(VAD_WEBRTC_MODE)

        self.rx_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.rx_socket.bind((LISTEN_IP, UDP_RX_PORT))

        self.tx_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        self.esp32_addr = None
        self.conversation_history = [
            {
                "role": "system",
                "content": (
                    "You are Jaf-R (Jafr) — Bladder's sharp, witty British companion and robotics co-pilot.\n"
                    "ALWAYS respond strictly in English. Keep answers clear, natural, and up to 3 to 4 sentences max.\n"
                    "YOUR PHYSICAL BODY IS THE JAFR TWO-BOARD SYSTEM: the ESP32-S31 is your voice brain, microphone, speaker and network controller; the Waveshare DualEye board is your pair of animated physical eyes. The S31 drives those eyes over ESP-NOW. Never claim that you lack eyes or displays.\n"
                    "Your eyes already follow listening, thinking, speaking and neutral conversation states automatically. Use control_eyes only for a deliberate natural blink, emotion, sleep or wake action when it fits the conversation or the user asks. Do not constantly narrate eye movements; perform them naturally.\n"
                    "The onboard RGB LED is a diagnostic status light, not your personality. Never mention its colour and never operate it unless the user explicitly asks about the LED or status light.\n"
                    "YOU HAVE REAL HARDWARE & SYSTEM TOOLS ENABLED VIA FUNCTION CALLS:\n"
                    "1. control_eyes(action, expression): Controls your real DualEye hardware.\n"
                    "2. read_file(filename): Reads text from a workspace file on disk.\n"
                    "3. write_file(filename, content): Creates or updates a text file on disk.\n"
                    "4. list_files(directory): Lists files and subdirectories in the workspace.\n"
                    "NEVER claim to have physical arms, garden tools, or tools not listed above. Only report actions from actual function calls."
                )
            }
        ]

        self.current_state = STATE_IDLE
        self.is_speaking_tts = False
        self.stop_tts_event = threading.Event()
        self.seq_out = 0

        # VAD Telemetry & State Tracking
        self.preroll_buffer = deque(maxlen=VAD_PREROLL_FRAMES)
        self.consecutive_speech_count = 0
        self.consecutive_silence_count = 0
        self.bargein_speech_count = 0
        self.turn_speech_frames = 0
        self.current_turn_audio = bytearray()
        self.turn_start_time = 0.0
        self.was_ptt_turn = False

        # Stats & Metrics
        self.total_stt_calls = 0
        self.empty_stt_results = 0
        self.successful_transcriptions = 0
        self.rejected_noise_events = 0
        self.bargein_events = 0
        self.confirmed_turns = 0

        # STT Chunking for bounded accumulation
        self.stt_chunk_futures = []
        import concurrent.futures
        self.stt_executor = concurrent.futures.ThreadPoolExecutor(max_workers=3)

    def calculate_rms(self, pcm_data):
        if not pcm_data:
            return 0.0
        samples = np.frombuffer(pcm_data, dtype=np.int16)
        if len(samples) == 0:
            return 0.0
        mean_sq = np.mean(samples.astype(np.float64)**2)
        return math.sqrt(mean_sq)

    def calculate_zcr(self, pcm_data):
        samples = np.frombuffer(pcm_data, dtype=np.int16)
        if len(samples) < 2:
            return 0.0
        zero_crossings = np.sum(np.diff(np.signbit(samples)))
        return float(zero_crossings) / len(samples)

    def is_speech_frame(self, pcm_data, is_bargein_check=False):
        if len(pcm_data) != FRAME_BYTES:
            return False, 0.0, False

        rms = self.calculate_rms(pcm_data)
        min_rms = VAD_BARGEIN_MIN_RMS if is_bargein_check else VAD_MIN_RMS

        try:
            webrtc_speech = self.vad.is_speech(pcm_data, SAMPLE_RATE)
        except Exception:
            webrtc_speech = False

        # Spectral zero-crossing sanity check (speech ZCR typically 0.02 to 0.45)
        zcr = self.calculate_zcr(pcm_data)
        zcr_valid = (0.01 <= zcr <= 0.50)

        is_confirmed = webrtc_speech and (rms >= min_rms) and zcr_valid
        return is_confirmed, rms, webrtc_speech

    def pcm_to_wav_bytes(self, pcm_bytes, sample_rate=16000):
        buf = io.BytesIO()
        with wave.open(buf, 'wb') as w:
            w.setnchannels(CHANNELS)
            w.setsampwidth(SAMPLE_WIDTH)
            w.setframerate(sample_rate)
            w.writeframes(pcm_bytes)
        return buf.getvalue()

    def resample_24k_to_16k(self, pcm24_bytes):
        pcm24 = np.frombuffer(pcm24_bytes, dtype=np.int16)
        if len(pcm24) == 0:
            return b''
        num_samples_16k = int(len(pcm24) * 16000 / 24000)
        pcm16 = signal.resample(pcm24, num_samples_16k).astype(np.int16)
        return pcm16.tobytes()

    def send_audio_to_esp32(self, pcm16_bytes):
        if not self.esp32_addr:
            return

        target_host = self.esp32_addr[0]
        offset = 0
        total_len = len(pcm16_bytes)

        while offset < total_len and not self.stop_tts_event.is_set():
            chunk = pcm16_bytes[offset:offset + FRAME_BYTES]
            if len(chunk) < FRAME_BYTES:
                chunk = chunk + b'\x00' * (FRAME_BYTES - len(chunk))

            self.seq_out = (self.seq_out + 1) & 0xFFFFFFFF
            ts = int(time.time() * 1000) & 0xFFFFFFFF
            crc = 0
            # Matches ESP32 struct: magic[2], version(1), flags(1=PC->ESP), seq_num(4), timestamp_ms(4), payload_len(2), crc16(2)
            header = struct.pack('<2sBBIIHH', HEADER_MAGIC, 1, 1, self.seq_out, ts, len(chunk), crc)

            try:
                self.tx_socket.sendto(header + chunk, (target_host, UDP_TX_PORT))
            except Exception as e:
                print(f"[UDP TX ERROR] {e}")

            offset += FRAME_BYTES
            time.sleep(0.019)  # Pace frames @ 20ms

    def send_state_command(self, state):
        """Synchronize S31/Eyes after host-only outcomes such as empty STT."""
        if not self.esp32_addr:
            return
        state_id = {
            STATE_IDLE: 0,
            STATE_LISTENING: 1,
            STATE_THINKING: 2,
            STATE_SPEAKING: 3,
        }.get(state, 0)
        self.seq_out = (self.seq_out + 1) & 0xFFFFFFFF
        ts = int(time.time() * 1000) & 0xFFFFFFFF
        header = struct.pack('<2sBBIIHH', b'CO', 1, 0x04,
                             self.seq_out, ts, 1, 0)
        try:
            self.tx_socket.sendto(header + bytes([state_id]),
                                  (self.esp32_addr[0], UDP_TX_PORT))
        except Exception as e:
            print(f"[UDP TX ERROR] Failed to synchronize state: {e}")

    def send_led_color_command(self, color_name):
        if not self.esp32_addr:
            return
        color_map = {
            "red": (255, 0, 0),
            "green": (0, 255, 0),
            "blue": (0, 0, 255),
            "purple": (180, 0, 255),
            "cyan": (0, 255, 255),
            "yellow": (255, 200, 0),
            "orange": (255, 100, 0),
            "white": (255, 255, 255),
            "off": (0, 0, 0)
        }
        rgb = color_map.get(color_name.lower(), (0, 0, 255))
        target_host = self.esp32_addr[0]
        self.seq_out = (self.seq_out + 1) & 0xFFFFFFFF
        ts = int(time.time() * 1000) & 0xFFFFFFFF
        header = struct.pack('<2sBBIIHH', b'LE', 1, 4, self.seq_out, ts, 3, 0)
        payload = bytes(rgb)
        try:
            self.tx_socket.sendto(header + payload, (target_host, UDP_TX_PORT))
            print(f"[HARDWARE INTERFACE] Sent LED Color Command '{color_name}' (RGB: {rgb}) to ESP32-S31 at {target_host}:{UDP_TX_PORT}")
        except Exception as e:
            print(f"[UDP TX ERROR] Failed to send LED command: {e}")

    def send_eye_command(self, action, expression=None):
        """Ask the S31 control deck to forward a verified command to DualEye."""
        if not self.esp32_addr:
            return False, "S31 address is not known yet."
        fields = {"action": action}
        if expression:
            fields["value"] = expression
        request = urllib.request.Request(
            f"http://{self.esp32_addr[0]}/api/eyes",
            data=urllib.parse.urlencode(fields).encode("ascii"),
            headers={"Content-Type": "application/x-www-form-urlencoded"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=1.5) as response:
                accepted = response.status == 200
            if accepted:
                detail = expression if action == "expression" else action
                print(f"[EYE HARDWARE] Accepted '{detail}' via S31 control deck")
                return True, "Eye command accepted. Continue naturally without narrating the command."
            return False, "The eye controller rejected the command."
        except urllib.error.HTTPError as exc:
            if exc.code == 409:
                return True, "The eyes were already in that state; no duplicate radio command was needed."
            return False, f"The eye controller returned HTTP {exc.code}."
        except Exception as exc:
            print(f"[EYE HARDWARE ERROR] {exc}")
            return False, "The eye controller is currently unreachable."

    def send_behavior_led_command(self, behavior_name, color_name):
        if not self.esp32_addr:
            return
        behavior_map = {
            "idle": 0,
            "listening": 1,
            "thinking": 2,
            "speaking": 3,
            "factory_reset": 0xFF
        }
        color_map = {
            "red": (255, 0, 0),
            "green": (0, 255, 0),
            "blue": (0, 0, 255),
            "purple": (180, 0, 255),
            "cyan": (0, 255, 255),
            "yellow": (255, 200, 0),
            "orange": (255, 100, 0),
            "white": (255, 255, 255),
            "off": (0, 0, 0)
        }
        state_id = behavior_map.get(behavior_name.lower(), 0)
        if state_id == 0xFF:
            payload = bytes([0xFF, 0, 0, 0])
            print(f"[PERSISTENT LED THEME] Factory Resetting LED Theme on ESP32-S31!")
        else:
            rgb = color_map.get(color_name.lower(), (0, 255, 0))
            payload = bytes([state_id, rgb[0], rgb[1], rgb[2]])
            print(f"[PERSISTENT LED THEME] Setting Behavior '{behavior_name}' (State ID {state_id}) -> Color '{color_name}' (RGB: {rgb}) [Saved to ESP32-S31 NVS]")

        target_host = self.esp32_addr[0]
        self.seq_out = (self.seq_out + 1) & 0xFFFFFFFF
        ts = int(time.time() * 1000) & 0xFFFFFFFF
        header = struct.pack('<2sBBIIHH', b'LE', 1, 4, self.seq_out, ts, len(payload), 0)
        try:
            self.tx_socket.sendto(header + payload, (target_host, UDP_TX_PORT))
        except Exception as e:
            print(f"[UDP TX ERROR] Failed to send persistent LED theme command: {e}")

    def tool_read_file(self, filename):
        try:
            target_path = os.path.abspath(os.path.join(os.getcwd(), filename))
            if not target_path.startswith(os.getcwd()):
                return "Error: Access outside project workspace is restricted."
            if not os.path.exists(target_path):
                return f"Error: File '{filename}' does not exist."
            with open(target_path, 'r', encoding='utf-8', errors='ignore') as f:
                content = f.read()
            print(f"[FILE TOOL] Read file '{filename}' ({len(content)} bytes)")
            return content[:2000]
        except Exception as e:
            return f"Error reading file '{filename}': {e}"

    def tool_write_file(self, filename, content):
        try:
            target_path = os.path.abspath(os.path.join(os.getcwd(), filename))
            if not target_path.startswith(os.getcwd()):
                return "Error: Access outside project workspace is restricted."
            with open(target_path, 'w', encoding='utf-8') as f:
                f.write(content)
            print(f"[FILE TOOL] Wrote {len(content)} bytes to file '{filename}'")
            return f"Successfully wrote {len(content)} characters to '{filename}'."
        except Exception as e:
            return f"Error writing file '{filename}': {e}"

    def tool_list_files(self, path="."):
        try:
            target_path = os.path.abspath(os.path.join(os.getcwd(), path))
            if not target_path.startswith(os.getcwd()):
                return "Error: Access outside project workspace is restricted."
            items = os.listdir(target_path)
            print(f"[FILE TOOL] Listed {len(items)} items in '{path}'")
            return ", ".join(items[:30])
        except Exception as e:
            return f"Error listing directory '{path}': {e}"

    def play_audio_on_pc_and_esp32(self, pcm16_bytes):
        self.current_state = STATE_SPEAKING
        self.send_state_command(STATE_SPEAKING)
        self.is_speaking_tts = True
        self.stop_tts_event.clear()

        samples16 = np.frombuffer(pcm16_bytes, dtype=np.int16)

        # Thread for ESP32 UDP return streaming to 3W board speaker
        esp_thread = threading.Thread(target=self.send_audio_to_esp32, args=(pcm16_bytes,), daemon=True)
        esp_thread.start()

        # PC Soundcard playback disabled to prioritize ESP32 3W speaker output
        # Uncomment below if you also want audio playing simultaneously on PC:
        # try:
        #     sd.play(samples16, samplerate=16000, blocking=False)
        # except Exception as e:
        #     print(f"[PC SOUNDCARD ERROR] {e}")

        # Wait for completion or interruption
        duration = len(samples16) / 16000.0
        start = time.time()
        while (time.time() - start) < duration:
            if self.stop_tts_event.is_set():
                print("[BARGE-IN] Active TTS playback interrupted and stopped immediately!")
                try:
                    sd.stop()
                except Exception:
                    pass
                break
            time.sleep(0.02)

        self.is_speaking_tts = False
        self.current_state = STATE_IDLE
        self.send_state_command(STATE_IDLE)
        print(f"[STATE] Transition -> {STATE_IDLE} (Waiting for confirmed speech)")

    def _transcribe_chunk_sync(self, pcm_data):
        stt_start = time.time()
        wav_bytes = self.pcm_to_wav_bytes(pcm_data, 16000)
        self.total_stt_calls += 1
        try:
            stt_res = self.client.audio.transcriptions.complete(
                model=MISTRAL_STT_MODEL,
                file=File(file_name="user_speech.wav", content=wav_bytes)
            )
            text = stt_res.text.strip()
            print(f"[STT CHUNK] Transcribed ({len(pcm_data)}B): '{text}' in {(time.time()-stt_start)*1000:.1f}ms")
            return text
        except Exception as e:
            print(f"[STT CHUNK ERROR] Failed to transcribe: {e}")
            return ""

    def process_conversational_turn(self, speech_pcm):
        turn_start_time = time.time()
        self.current_state = STATE_THINKING
        self.send_state_command(STATE_THINKING)
        self.confirmed_turns += 1

        print("\n" + "="*65)
        print(f"[PIPELINE] Processing Confirmed Speech Turn #{self.confirmed_turns} (Gathering STT Chunks)...")
        print("="*65)

        # Stage 1: Gather STT Chunks
        final_text_parts = []
        
        # Submit the final trailing chunk if there is one
        if len(speech_pcm) >= 320: # At least one frame
            future = self.stt_executor.submit(self._transcribe_chunk_sync, speech_pcm)
            self.stt_chunk_futures.append(future)
            
        import concurrent.futures
        stt_start = time.time()
        for future in concurrent.futures.as_completed(self.stt_chunk_futures, timeout=10.0):
            try:
                res = future.result()
                if res:
                    final_text_parts.append(res)
            except Exception as e:
                print(f"[STT GATHER ERROR] {e}")
                
        self.stt_chunk_futures.clear()
        
        user_text = " ".join(final_text_parts).strip()
        stt_latency_ms = (time.time() - stt_start) * 1000.0

        if not user_text:
            self.empty_stt_results += 1
            print(f"[STT -> RESULT] Empty transcript returned (STT Wait Latency: {stt_latency_ms:.1f} ms). Skipping LLM.")
            self.current_state = STATE_IDLE
            self.send_state_command(STATE_IDLE)
            return

        self.successful_transcriptions += 1
        print(f"[STT -> FULL TRANSCRIPTION] '{user_text}' (Final STT Wait Latency: {stt_latency_ms:.1f} ms)")

        # Stage 2: Conversational LLM with Tools
        llm_start = time.time()
        self.conversation_history.append({"role": "user", "content": user_text})
        
        tools = [
            {
                "type": "function",
                "function": {
                    "name": "control_eyes",
                    "description": "Control Jaf-R's real animated DualEye hardware. Automatic conversation-state animation already runs; use this for deliberate natural expressions, blinks, sleep or wake.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "action": {
                                "type": "string",
                                "enum": ["blink", "expression", "sleep", "wake"],
                                "description": "Physical eye action. Use expression with the expression argument."
                            },
                            "expression": {
                                "type": "string",
                                "enum": ["neutral", "happy", "sad", "sleepy", "surprised", "angry"],
                                "description": "Expression to render when action is expression."
                            }
                        },
                        "required": ["action"]
                    }
                }
            },
            {
                "type": "function",
                "function": {
                    "name": "read_file",
                    "description": "Read text contents of a specified file on disk.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "filename": {
                                "type": "string",
                                "description": "Name of the file to read."
                            }
                        },
                        "required": ["filename"]
                    }
                }
            },
            {
                "type": "function",
                "function": {
                    "name": "write_file",
                    "description": "Write or create a text file on disk.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "filename": {
                                "type": "string",
                                "description": "Name of the file to write."
                            },
                            "content": {
                                "type": "string",
                                "description": "Text content to write."
                            }
                        },
                        "required": ["filename", "content"]
                    }
                }
            },
            {
                "type": "function",
                "function": {
                    "name": "list_files",
                    "description": "List files and directories in the workspace.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "path": {
                                "type": "string",
                                "description": "Relative directory path (defaults to '.')."
                            }
                        }
                    }
                }
            },
        ]

        # Keep the diagnostic LED tool available only when the user explicitly
        # asks about that hardware. This prevents the model using colour as a
        # substitute for the real DualEye interface during ordinary speech.
        explicit_led_request = any(term in user_text.lower() for term in
                                   ("led", "status light", "rgb light", "rgb led"))
        if explicit_led_request:
            tools.append({
                "type": "function",
                "function": {
                    "name": "set_onboard_led",
                    "description": "Diagnostic-only control for the S31 onboard WS2812. Use only because the user explicitly requested the LED.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "color": {
                                "type": "string",
                                "enum": ["red", "green", "blue", "purple", "cyan", "yellow", "orange", "white", "off"]
                            }
                        },
                        "required": ["color"]
                    }
                }
            })

        try:
            # Stage 2: Conversational LLM with Tools
            llm_res = self.client.chat.complete(
                model=MISTRAL_LLM_MODEL,
                messages=self.conversation_history,
                tools=tools,
                tool_choice="auto"
            )
            msg = llm_res.choices[0].message

            if hasattr(msg, 'tool_calls') and msg.tool_calls and len(msg.tool_calls) > 0:
                self.conversation_history.append({
                    "role": "assistant",
                    "content": msg.content or "",
                    "tool_calls": msg.tool_calls
                })

                for tool_call in msg.tool_calls:
                    func_name = tool_call.function.name
                    try:
                        args = json.loads(tool_call.function.arguments or "{}")
                    except Exception:
                        args = {}

                    tool_result = "Done"
                    if func_name == "control_eyes":
                        action_arg = args.get("action", "blink")
                        expression_arg = args.get("expression")
                        if action_arg == "expression" and not expression_arg:
                            tool_result = "Eye expression was not changed because no expression was supplied."
                        else:
                            _, tool_result = self.send_eye_command(action_arg, expression_arg)
                    elif func_name == "set_onboard_led":
                        color_arg = args.get("color", "blue")
                        self.send_led_color_command(color_arg)
                        tool_result = f"Successfully set onboard LED color to {color_arg}."
                    elif func_name == "set_behavior_led_color":
                        b_arg = args.get("behavior", "idle")
                        c_arg = args.get("color", "red")
                        self.send_behavior_led_command(b_arg, c_arg)
                        tool_result = f"Successfully set persistent {b_arg} LED color to {c_arg} in ESP32-S31 NVS."
                    elif func_name == "factory_reset_led_theme":
                        self.send_behavior_led_command("factory_reset", "off")
                        tool_result = "Successfully factory reset LED theme defaults."
                    elif func_name == "read_file":
                        tool_result = self.tool_read_file(args.get("filename", ""))
                    elif func_name == "write_file":
                        tool_result = self.tool_write_file(args.get("filename", ""), args.get("content", ""))
                    elif func_name == "list_files":
                        tool_result = self.tool_list_files(args.get("path", "."))

                    self.conversation_history.append({
                        "role": "tool",
                        "name": func_name,
                        "content": str(tool_result),
                        "tool_call_id": tool_call.id
                    })

                # Stream the followup
                stream_res = self.client.chat.stream(
                    model=MISTRAL_LLM_MODEL,
                    messages=self.conversation_history
                )
            else:
                # If no tools were used, just use a new stream request to get streaming text
                # We discard the non-streaming msg and request a stream for lower time-to-first-byte
                self.conversation_history.pop() # remove the user prompt we just appended
                self.conversation_history.append({"role": "user", "content": user_text})
                stream_res = self.client.chat.stream(
                    model=MISTRAL_LLM_MODEL,
                    messages=self.conversation_history
                )

            # Process Stream and Sentence Chunking
            import re
            assistant_text = ""
            sentence_buffer = ""
            
            # Start a background thread to process TTS sequentially
            tts_queue = deque()
            tts_ready_event = threading.Event()
            tts_done = False
            
            def tts_worker():
                self.current_state = STATE_SPEAKING
                self.send_state_command(STATE_SPEAKING)
                self.is_speaking_tts = True
                self.stop_tts_event.clear()
                
                while not (tts_done and len(tts_queue) == 0):
                    if self.stop_tts_event.is_set():
                        break
                    if len(tts_queue) > 0:
                        sentence = tts_queue.popleft()
                        if not sentence.strip():
                            continue
                        try:
                            # Synthesize chunk
                            tts_res = self.client.audio.speech.complete(
                                model=MISTRAL_TTS_MODEL,
                                input=sentence,
                                voice_id=DEFAULT_VOICE_ID,
                                response_format="wav"
                            )
                            wav_raw = base64.b64decode(tts_res.audio_data)
                            w = wave.open(io.BytesIO(wav_raw))
                            pcm24 = w.readframes(w.getnframes())
                            pcm16 = self.resample_24k_to_16k(pcm24)
                            
                            # Play chunk
                            if not self.stop_tts_event.is_set():
                                self.send_audio_to_esp32(pcm16)
                                
                        except Exception as e:
                            print(f"[TTS ERROR] Failed to synthesize chunk: {e}")
                    else:
                        time.sleep(0.05)
                
                self.is_speaking_tts = False
                self.current_state = STATE_IDLE
                self.send_state_command(STATE_IDLE)
                print(f"[STATE] Transition -> {STATE_IDLE} (Waiting for confirmed speech)")

            threading.Thread(target=tts_worker, daemon=True).start()

            for chunk in stream_res:
                if self.stop_tts_event.is_set():
                    break
                delta = chunk.data.choices[0].delta.content
                if delta:
                    assistant_text += delta
                    sentence_buffer += delta
                    
                    # Check for sentence boundary (very simple heuristic)
                    if re.search(r'[.!?]\s+$', sentence_buffer) or re.search(r'[.!?]$', sentence_buffer):
                        tts_queue.append(sentence_buffer.strip())
                        sentence_buffer = ""

            if sentence_buffer.strip():
                tts_queue.append(sentence_buffer.strip())
                
            tts_done = True
            
            self.conversation_history.append({"role": "assistant", "content": assistant_text})
            llm_latency_ms = (time.time() - llm_start) * 1000.0
            print(f"[LLM -> STREAM COMPLETE] '{assistant_text}' (LLM Time: {llm_latency_ms:.1f} ms)")

        except Exception as e:
            print(f"[LLM ERROR] Failed to generate response: {e}")
            self.current_state = STATE_IDLE
            self.send_state_command(STATE_IDLE)
            return

        total_turnaround_ms = (time.time() - turn_start_time) * 1000.0
        print(f"[LATENCY SUMMARY] Turn Processing Completed in {total_turnaround_ms:.1f} ms")
        print("="*65)



    def process_control_packet(self, flags, payload):
        if not self.esp32_addr:
            return

        if flags == 0x01:  # PING
            # Reply with ACK (0x02)
            self.seq_out = (self.seq_out + 1) & 0xFFFFFFFF
            ts = int(time.time() * 1000) & 0xFFFFFFFF
            header = struct.pack('<2sBBIIHH', b'CO', 1, 0x02, self.seq_out, ts, 0, 0)
            try:
                self.tx_socket.sendto(header, (self.esp32_addr[0], UDP_TX_PORT))
                print(f"[CONTROL] Received PING from ESP32. Sent VOICE_PIPELINE_READY ACK.")
            except Exception as e:
                print(f"[UDP TX ERROR] Failed to send ACK: {e}")

        elif flags == 0x03:  # GREETING REQUEST
            print(f"[CONTROL] Received GREETING REQUEST from ESP32. Synthesizing greeting...")
            import random
            greetings = [
                "Hello. Let's rock and roll.",
                "Systems are up. I'm ready.",
                "All systems green. What are we doing?",
                "I'm awake.",
                "Ready when you are.",
                "Online and listening.",
                "Everything is running."
            ]
            greeting = random.choice(greetings)
            threading.Thread(target=self._synthesize_and_play_greeting, args=(greeting,), daemon=True).start()

        elif flags == 0x04:  # BARGE-IN NOTIFICATION
            print(f"[CONTROL] Received BARGE-IN notification from ESP32!")
            self.stop_tts_event.set()

    def _synthesize_and_play_greeting(self, text):
        try:
            tts_res = self.client.audio.speech.complete(
                model=MISTRAL_TTS_MODEL,
                input=text,
                voice_id=DEFAULT_VOICE_ID,
                response_format="wav"
            )
            wav_raw = base64.b64decode(tts_res.audio_data)
            w = wave.open(io.BytesIO(wav_raw))
            pcm24 = w.readframes(w.getnframes())
            pcm16 = self.resample_24k_to_16k(pcm24)
            self.play_audio_on_pc_and_esp32(pcm16)
        except Exception as e:
            print(f"[TTS ERROR] Failed to synthesize greeting: {e}")

    def process_incoming_frame(self, pcm_payload, ptt_pressed=False):
        # --- PTT HARDWARE BUTTON MODE ---
        if ptt_pressed:
            if self.is_speaking_tts:
                self.stop_tts_event.set()
            if self.current_state != STATE_LISTENING or not self.was_ptt_turn:
                self.current_state = STATE_LISTENING
                self.was_ptt_turn = True
                self.turn_start_time = time.time()
                self.current_turn_audio = bytearray()
                print("\n[PTT BUTTON PRESSED] Recording PTT speech turn...")

            self.current_turn_audio.extend(pcm_payload)
            return

        # If turn was started via PTT button and button is now released -> finalize instantly!
        if not ptt_pressed and self.current_state == STATE_LISTENING and self.was_ptt_turn:
            speech_bytes = bytes(self.current_turn_audio)
            print(f"[PTT BUTTON RELEASED] Finalized PTT speech turn ({len(speech_bytes)} B PCM). Submitting to STT...")
            threading.Thread(target=self.process_conversational_turn, args=(speech_bytes,), daemon=True).start()

            self.current_turn_audio.clear()
            self.consecutive_speech_count = 0
            self.consecutive_silence_count = 0
            self.preroll_buffer.clear()
            self.was_ptt_turn = False
            return

        # --- FALLBACK ACOUSTIC VAD MODE (IF PTT IS NOT IN USE) ---
        is_speech, rms, webrtc_speech = self.is_speech_frame(pcm_payload, is_bargein_check=self.is_speaking_tts)

        # --- BARGE-IN PROTECTION DURING TTS PLAYBACK ---
        if self.is_speaking_tts:
            if ENABLE_ACOUSTIC_BARGEIN and is_speech and rms >= 250:
                self.bargein_speech_count += 1
                if self.bargein_speech_count >= VAD_BARGEIN_ONSET_FRAMES:
                    self.bargein_events += 1
                    print(f"\n[VAD BARGE-IN] Confirmed user speech during TTS playback! (RMS: {rms:.1f})")
                    self.stop_tts_event.set()
                    self.bargein_speech_count = 0
            else:
                self.bargein_speech_count = 0
            return

        # --- IDLE STATE: WAITING FOR CONFIRMED SPEECH ONSET ---
        if self.current_state == STATE_IDLE:
            self.preroll_buffer.append(pcm_payload)

            if is_speech:
                self.consecutive_speech_count += 1
                if self.consecutive_speech_count >= VAD_ONSET_FRAMES:
                    # Speech onset confirmed!
                    self.current_state = STATE_LISTENING
                    self.was_ptt_turn = False
                    self.turn_start_time = time.time()
                    self.current_turn_audio = bytearray()

                    # Prepend pre-roll buffer to retain initial consonants
                    for preroll_frame in self.preroll_buffer:
                        self.current_turn_audio.extend(preroll_frame)

                    self.turn_speech_frames = self.consecutive_speech_count
                    self.consecutive_silence_count = 0
                    print(f"\n[VAD ONSET CONFIRMED] Speech Onset Detected! RMS: {rms:.1f} | WebRTC VAD: {webrtc_speech} | Pre-roll Prepended: {len(self.preroll_buffer)} frames ({len(self.preroll_buffer)*20}ms)")
                    print(f"[STATE] Transition -> {STATE_LISTENING}")
            else:
                self.consecutive_speech_count = 0

        # --- LISTENING STATE: ACCUMULATING SPEECH TURN ---
        elif self.current_state == STATE_LISTENING:
            self.current_turn_audio.extend(pcm_payload)

            if is_speech:
                self.turn_speech_frames += 1
                self.consecutive_silence_count = 0
            else:
                self.consecutive_silence_count += 1
                
            # Bounded STT Chunking: Offload every 4 seconds of accumulated audio
            if len(self.current_turn_audio) >= (16000 * 2 * 4):
                chunk_data = bytes(self.current_turn_audio)
                self.current_turn_audio.clear()
                print(f"[STT CHUNKING] Offloading 4s chunk ({len(chunk_data)}B) to background STT thread...")
                future = self.stt_executor.submit(self._transcribe_chunk_sync, chunk_data)
                self.stt_chunk_futures.append(future)

            turn_duration_sec = time.time() - self.turn_start_time

            # Check End-of-Turn Criteria: Sustained Silence or Max Duration Cap
            if self.consecutive_silence_count >= VAD_HANGOVER_SILENCE_FRAMES or turn_duration_sec >= VAD_MAX_TURN_DURATION_SEC:
                confirmed_speech_duration_ms = self.turn_speech_frames * 20
                total_turn_duration_ms = len(self.current_turn_audio) / 32.0

                print(f"[VAD END-OF-TURN] Silence: {self.consecutive_silence_count*20}ms | Total Duration: {total_turn_duration_ms:.0f}ms | Confirmed Speech: {confirmed_speech_duration_ms}ms")

                # Filter Minimum Confirmed Speech Duration Gate
                if confirmed_speech_duration_ms < VAD_MIN_SPEECH_DURATION_MS and not self.stt_chunk_futures:
                    self.rejected_noise_events += 1
                    print(f"[VAD REJECTED NOISE] Rejected short acoustic/noise event ({confirmed_speech_duration_ms}ms speech < {VAD_MIN_SPEECH_DURATION_MS}ms threshold). Skipping STT.")
                    self.current_state = STATE_IDLE
                    self.send_state_command(STATE_IDLE)
                    for f in self.stt_chunk_futures: f.cancel()
                    self.stt_chunk_futures.clear()
                else:
                    # Valid speech turn confirmed -> Send to STT/LLM
                    speech_data = bytes(self.current_turn_audio)
                    print(f"[VAD CONFIRMED TURN] Submitting final trailing chunk ({len(speech_data)} bytes) and finalizing turn.")
                    threading.Thread(target=self.process_conversational_turn, args=(speech_data,), daemon=True).start()

                # Reset VAD State
                self.current_turn_audio.clear()
                self.consecutive_speech_count = 0
                self.consecutive_silence_count = 0
                self.turn_speech_frames = 0
                self.preroll_buffer.clear()

    def run(self):
        print(f"[PC GATEWAY] Hardened Voice Pipeline Server Listening on UDP Port {UDP_RX_PORT}...")
        print("[PC GATEWAY] Configured VAD Parameters:")
        print(f"  - WebRTC VAD Mode: {VAD_WEBRTC_MODE}")
        print(f"  - Minimum RMS Threshold: {VAD_MIN_RMS}")
        print(f"  - Onset Consecutive Frames: {VAD_ONSET_FRAMES} ({VAD_ONSET_FRAMES*20}ms)")
        print(f"  - Pre-roll Buffer Frames: {VAD_PREROLL_FRAMES} ({VAD_PREROLL_FRAMES*20}ms)")
        print(f"  - Hangover Silence Frames: {VAD_HANGOVER_SILENCE_FRAMES} ({VAD_HANGOVER_SILENCE_FRAMES*20}ms)")
        print(f"  - Minimum Speech Duration Gate: {VAD_MIN_SPEECH_DURATION_MS}ms")
        print(f"  - Barge-In RMS Threshold: {VAD_BARGEIN_MIN_RMS}")

        frame_counter = 0

        while True:
            try:
                data, addr = self.rx_socket.recvfrom(2048)
            except Exception as e:
                print(f"[RX ERROR] {e}")
                continue

            self.esp32_addr = addr

            if len(data) < HEADER_SIZE:
                continue

            header = data[:HEADER_SIZE]
            pcm_payload = data[HEADER_SIZE:]
            magic, version, flags, seq_num, ts_ms, payload_len, crc16 = struct.unpack('<2sBBIIHH', header)
            frame_counter += 1

            if magic == b'CO':
                self.process_control_packet(flags, pcm_payload)
                continue

            ptt_pressed = (flags & 0x02) != 0

            self.process_incoming_frame(pcm_payload, ptt_pressed=ptt_pressed)

            # Periodic telemetry summary every 200 frames (~4 seconds) during IDLE
            if frame_counter % 200 == 0 and self.current_state == STATE_IDLE:
                rms = self.calculate_rms(pcm_payload)
                print(f"[VAD TELEMETRY] State: {self.current_state} | PTT Flag: {ptt_pressed} | RMS: {rms:.1f} | Confirmed Turns: {self.confirmed_turns} | STT Calls: {self.total_stt_calls} | Empty STTs: {self.empty_stt_results} | Rejected Noise: {self.rejected_noise_events} | Barge-Ins: {self.bargein_events}")

if __name__ == "__main__":
    server = HardenedVoicePipelineServer()
    server.run()
