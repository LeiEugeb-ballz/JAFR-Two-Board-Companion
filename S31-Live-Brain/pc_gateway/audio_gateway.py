#!/usr/bin/env python3
"""
PC Voice Gateway for ESP32-S31 Audio Terminal (Phase 3)
Receives 16kHz 16-bit mono PCM stream over UDP from ESP32-S31,
inspects sequence continuity, latency, jitter, RMS/VAD levels,
saves WAV audio log, and sends test PCM audio back to ESP32.
"""

import socket
import struct
import time
import math
import wave
import threading
import sys
import os

# Protocol Header Definition (16 bytes)
# struct audio_frame_header_t {
#     uint8_t  magic[2];     // 'A', 'U' (0x41, 0x55)
#     uint8_t  version;      // 0x01
#     uint8_t  flags;        // Bit 0: Direction (0=ESP->PC, 1=PC->ESP), Bit 1: VAD
#     uint32_t seq_num;      // Sequence number
#     uint32_t timestamp_ms; // ESP32 timestamp in ms
#     uint16_t payload_len;  // Payload length (640 bytes)
#     uint16_t crc16;        // Header/payload CRC or checksum
# };
HEADER_FORMAT = "<2sBBIIHH"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
MAGIC = b"AU"
VERSION = 1

UDP_LISTEN_PORT = 5000  # Port to receive ESP32 audio
UDP_SEND_PORT = 5001    # Port on ESP32 to send return audio

class AudioGateway:
    def __init__(self, listen_port=UDP_LISTEN_PORT, wav_filename="captured_esp32_audio.wav"):
        self.listen_port = listen_port
        self.wav_filename = wav_filename
        self.running = False
        
        # Statistics
        self.total_packets = 0
        self.total_bytes = 0
        self.lost_packets = 0
        self.last_seq = None
        self.start_time = None
        self.last_packet_time = None
        self.jitter_ms = 0.0
        
        # Signal Metrics
        self.last_rms = 0.0
        self.last_pk_pk = 0
        self.last_min = 0
        self.last_max = 0
        
        # WAV output
        self.wav_file = None
        
        # Remote ESP32 address
        self.esp32_addr = None

    def start(self):
        self.running = True
        
        # Open WAV file for writing
        self.wav_file = wave.open(self.wav_filename, "wb")
        self.wav_file.setnchannels(1)       # Mono
        self.wav_file.setsampwidth(2)      # 16-bit PCM (2 bytes)
        self.wav_file.setframerate(16000)   # 16 kHz
        
        # Create UDP socket
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", self.listen_port))
        
        pc_ip = "192.168.88.240"
        esp_ip = "192.168.88.242"
        print(f"[PC GATEWAY] Listening on {pc_ip}:{self.listen_port}")
        print(f"[PC GATEWAY] Waiting for ESP32-S31 at {esp_ip}")
        print(f"[PC GATEWAY] Logging audio to '{self.wav_filename}'")
        
        # Start receiver thread
        self.rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self.rx_thread.start()
        
        # Start stats reporter thread
        self.stats_thread = threading.Thread(target=self._stats_loop, daemon=True)
        self.stats_thread.start()
        
        # Start TX return stream thread
        self.tx_thread = threading.Thread(target=self._tx_loop, daemon=True)
        self.tx_thread.start()

    def _rx_loop(self):
        self.start_time = time.time()
        
        while self.running:
            try:
                data, addr = self.sock.recvfrom(2048)
                now = time.time()
                
                if self.esp32_addr != addr:
                    self.esp32_addr = addr
                    print(f"[PC GATEWAY] ESP32-S31 CONNECTED: {addr[0]}:{addr[1]}")
                
                if len(data) < HEADER_SIZE:
                    print(f"[PC GATEWAY] Warning: Packet too short ({len(data)} bytes)")
                    continue
                
                header_data = data[:HEADER_SIZE]
                payload = data[HEADER_SIZE:]
                
                magic, version, flags, seq_num, timestamp_ms, payload_len, crc16 = struct.unpack(HEADER_FORMAT, header_data)
                
                if magic != MAGIC:
                    print(f"[PC GATEWAY] Invalid magic header: {magic}")
                    continue
                
                # Jitter calculation
                if self.last_packet_time is not None:
                    delta = (now - self.last_packet_time) * 1000.0  # ms
                    expected = 20.0  # 20ms frame
                    self.jitter_ms = 0.9 * self.jitter_ms + 0.1 * abs(delta - expected)
                self.last_packet_time = now
                
                # Sequence continuity & drop calculation
                if self.last_seq is not None:
                    expected_seq = (self.last_seq + 1) & 0xFFFFFFFF
                    if seq_num != expected_seq:
                        gap = (seq_num - self.last_seq - 1) & 0xFFFFFFFF
                        if gap < 1000:  # sanity check
                            self.lost_packets += gap
                self.last_seq = seq_num
                
                self.total_packets += 1
                self.total_bytes += len(data)
                
                # Calculate PCM Signal Metrics
                if len(payload) >= 2:
                    samples = struct.unpack(f"<{len(payload)//2}h", payload)
                    self.last_min = min(samples)
                    self.last_max = max(samples)
                    self.last_pk_pk = self.last_max - self.last_min
                    
                    sum_sq = sum(s * s for s in samples)
                    self.last_rms = math.sqrt(sum_sq / len(samples))
                    
                    # Write to WAV file
                    if self.wav_file:
                        self.wav_file.writeframes(payload)
                        
            except Exception as e:
                if self.running:
                    print(f"[PC GATEWAY] RX Error: {e}")

    def _stats_loop(self):
        last_count = 0
        last_time = time.time()
        
        while self.running:
            time.sleep(2.0)
            now = time.time()
            dt = now - last_time
            last_time = now
            
            pkts = self.total_packets - last_count
            last_count = self.total_packets
            
            fps = pkts / dt if dt > 0 else 0
            kbps = (pkts * 656 * 8) / (dt * 1000) if dt > 0 else 0
            
            loss_pct = (self.lost_packets / (self.total_packets + self.lost_packets) * 100.0) if (self.total_packets + self.lost_packets) > 0 else 0.0
            
            if self.total_packets > 0:
                print(f"[PC GATEWAY TELEMETRY] Pkts: {self.total_packets:6d} | Rate: {fps:5.1f} pkt/s ({kbps:5.1f} kbps) | "
                      f"Loss: {self.lost_packets} ({loss_pct:4.2f}%) | Jitter: {self.jitter_ms:4.1f}ms | "
                      f"PCM RMS: {self.last_rms:6.1f} | Pk-Pk: {self.last_pk_pk:5d} [Min: {self.last_min:5d}, Max: {self.last_max:5d}]")

    def _tx_loop(self):
        """Sends a continuous 440 Hz test sine tone (16kHz 16-bit mono) back to ESP32 on port UDP_SEND_PORT"""
        seq = 0
        sample_rate = 16000
        freq = 440.0  # 440 Hz A tone
        samples_per_frame = 320  # 20ms frame
        amplitude = 8000
        
        phase = 0.0
        phase_step = 2.0 * math.pi * freq / sample_rate
        
        while self.running:
            time.sleep(0.020)  # 20ms pace
            if self.esp32_addr is None:
                continue
            
            # Generate 320 sine PCM samples
            pcm_samples = []
            for _ in range(samples_per_frame):
                val = int(amplitude * math.sin(phase))
                pcm_samples.append(val)
                phase += phase_step
                if phase >= 2.0 * math.pi:
                    phase -= 2.0 * math.pi
            
            payload = struct.pack(f"<{len(pcm_samples)}h", *pcm_samples)
            timestamp_ms = int(time.time() * 1000) & 0xFFFFFFFF
            header = struct.pack(HEADER_FORMAT, MAGIC, VERSION, 1, seq, timestamp_ms, len(payload), 0)
            
            dest_addr = (self.esp32_addr[0], UDP_SEND_PORT)
            try:
                self.sock.sendto(header + payload, dest_addr)
                seq = (seq + 1) & 0xFFFFFFFF
            except Exception:
                pass

    def stop(self):
        self.running = False
        if hasattr(self, 'sock'):
            self.sock.close()
        if self.wav_file:
            self.wav_file.close()
            print(f"[PC GATEWAY] Saved WAV file to {self.wav_filename}")

if __name__ == "__main__":
    gateway = AudioGateway()
    gateway.start()
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\n[PC GATEWAY] Stopping gateway...")
        gateway.stop()
        sys.exit(0)
