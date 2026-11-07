import socket
import time
import struct
import base64
import io
import wave
import numpy as np
from mistralai.client import Mistral

client = Mistral()
print("Synthesizing test phrase for UDP streaming...")
tts = client.audio.speech.complete(
    model="voxtral-mini-tts-latest",
    input="Hello, this is a controlled latency test. One two three.",
    voice_id="gb_oliver_neutral",
    response_format="wav"
)
wav_raw = base64.b64decode(tts.audio_data)
w = wave.open(io.BytesIO(wav_raw))
pcm24 = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
num_samples_16k = int(len(pcm24) * 16000 / 24000)
pcm16 = np.interp(np.linspace(0, len(pcm24), num_samples_16k), np.arange(len(pcm24)), pcm24).astype(np.int16).tobytes()

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
print("Streaming test phrase over UDP to port 5000...")
offset = 0
seq = 0
header_magic = b'AU'

# Initial quiet frames
for _ in range(10):
    hdr = struct.pack('<2sHIII', header_magic, 640, seq, int(time.time()*1000)&0xFFFFFFFF, 0)
    sock.sendto(hdr + b'\x00'*640, ('127.0.0.1', 5000))
    time.sleep(0.02)

while offset < len(pcm16):
    chunk = pcm16[offset:offset+640]
    if len(chunk) < 640:
        chunk = chunk + b'\x00'*(640-len(chunk))
    seq += 1
    ts = int(time.time()*1000) & 0xFFFFFFFF
    hdr = struct.pack('<2sHIII', header_magic, len(chunk), seq, ts, 0)
    sock.sendto(hdr + chunk, ('127.0.0.1', 5000))
    offset += 640
    time.sleep(0.02)

# Trailing silence frames to trigger VAD hangover end-of-turn
for _ in range(40):
    seq += 1
    ts = int(time.time()*1000) & 0xFFFFFFFF
    hdr = struct.pack('<2sHIII', header_magic, 640, seq, ts, 0)
    sock.sendto(hdr + b'\x00'*640, ('127.0.0.1', 5000))
    time.sleep(0.02)

print("UDP streaming completed.")
