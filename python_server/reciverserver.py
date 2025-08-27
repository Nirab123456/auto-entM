#!/usr/bin/env python3
"""
esp_udp_receiver.py

Receive raw PCM UDP packets from the ESP32 streamer and play + optionally save to WAV.

Default params target the ESP sketch:
  - sample_rate = 16000
  - channels = 2
  - sample_width = 16-bit (dtype=int16)
  - UDP port = 7000

If audio sounds garbled, try switching sample rate or channel count to match sender.
"""

import argparse
import socket
import threading
import queue
import time
import wave
import sys
import os
import signal

try:
    import sounddevice as sd
except Exception as e:
    sd = None

# -----------------------
# Configuration / CLI
# -----------------------
parser = argparse.ArgumentParser(description="ESP32 UDP audio receiver (PCM16 stereo 16k default).")
parser.add_argument("--port", type=int, default=7000, help="UDP port to listen on (default 7000)")
parser.add_argument("--rate", type=int, default=16000, help="Sample rate in Hz (default 16000)")
parser.add_argument("--channels", type=int, default=2, help="Number of channels (default 2)")
parser.add_argument("--dtype", type=str, default="int16", help="Data type (default int16). Use 'int16' for PCM16.")
parser.add_argument("--no-play", action="store_true", help="Don't play audio live (only record)")
parser.add_argument("--out", type=str, default="esp_recording.wav", help="WAV output filename")
parser.add_argument("--inspect", action="store_true", help="Print hex dump of first packet for inspection")
args = parser.parse_args()

UDP_IP = "0.0.0.0"
UDP_PORT = args.port
SAMPLE_RATE = args.rate
CHANNELS = args.channels
DTYPE = args.dtype
OUT_WAV = args.out
PLAY = not args.no_play
INSPECT = args.inspect

print(f"CONFIG -> port={UDP_PORT}, rate={SAMPLE_RATE}, channels={CHANNELS}, dtype={DTYPE}, play={PLAY}, out='{OUT_WAV}'")

# -----------------------
# Thread-safe queue used to pass packets to audio writer/player
# -----------------------
packet_q = queue.Queue(maxsize=1000)
stop_event = threading.Event()
first_packet_checked = threading.Event()

# Stats
stats = {"packets": 0, "bytes": 0, "start": time.time()}

# -----------------------
# UDP listener thread
# -----------------------
def udp_listener():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind((UDP_IP, UDP_PORT))
    sock.settimeout(1.0)
    print(f"[UDP] Listening on {UDP_IP}:{UDP_PORT} ...")
    while not stop_event.is_set():
        try:
            data, addr = sock.recvfrom(65536)
        except socket.timeout:
            continue
        except Exception as e:
            print("[UDP] Listener error:", e)
            break

        stats["packets"] += 1
        stats["bytes"] += len(data)

        # Inspect first packet if requested
        if INSPECT and not first_packet_checked.is_set():
            print("[INSPECT] first packet len:", len(data))
            hexdump = " ".join(f"{b:02X}" for b in data[:32])
            print("[INSPECT] first 32 bytes:", hexdump)
            first_packet_checked.set()

        # non-blocking push
        try:
            packet_q.put_nowait(data)
        except queue.Full:
            # drop oldest packet then put (to keep fresh audio)
            try:
                _ = packet_q.get_nowait()
                packet_q.put_nowait(data)
            except Exception:
                pass

    sock.close()
    print("[UDP] listener stopped.")

# -----------------------
# Audio player + WAV writer
# -----------------------
def player_and_writer():
    # open WAV file
    wf = wave.open(OUT_WAV, "wb")
    sampwidth_bytes = 2 if DTYPE in ("int16", "int16le", "int16be") else 2
    wf.setnchannels(CHANNELS)
    wf.setsampwidth(sampwidth_bytes)
    wf.setframerate(SAMPLE_RATE)
    print(f"[WAV] Writing to: {os.path.abspath(OUT_WAV)} (channels={CHANNELS}, width={sampwidth_bytes}B, rate={SAMPLE_RATE})")

    # start player if requested
    stream = None
    if PLAY:
        if sd is None:
            print("[ERROR] sounddevice not available. Install with: pip install sounddevice")
            stop_event.set()
            wf.close()
            return
        try:
            stream = sd.RawOutputStream(samplerate=SAMPLE_RATE, channels=CHANNELS, dtype=DTYPE)
            stream.start()
            print("[PLAY] Audio playback started (sounddevice).")
        except Exception as e:
            print("[ERROR] cannot open audio output:", e)
            print("-> Will only record to WAV.")
            stream = None

    last_stats = time.time()
    try:
        while not stop_event.is_set() or not packet_q.empty():
            try:
                data = packet_q.get(timeout=0.5)
            except queue.Empty:
                continue

            # Write to WAV directly
            try:
                wf.writeframes(data)
            except Exception as e:
                print("[WAV] write error:", e)

            # Play live if stream available
            if stream is not None:
                try:
                    stream.write(data)
                except Exception as e:
                    # print once per second max to avoid spam
                    if time.time() - last_stats > 1.0:
                        print("[PLAY] stream.write error:", e)
                        last_stats = time.time()

            # periodic stats
            if time.time() - stats["start"] >= 1.0:
                print(f"[STATS] pkts/sec ~ {stats['packets']:.0f}, bytes/sec ~ {stats['bytes']}")
                stats["packets"] = 0
                stats["bytes"] = 0
                stats["start"] = time.time()

    except KeyboardInterrupt:
        print("[MAIN] KeyboardInterrupt in player.")
    finally:
        print("[WAV] closing file...")
        wf.close()
        if stream is not None:
            stream.stop()
            stream.close()
        print("[WAV] finished.")

# -----------------------
# Main startup / signal handling
# -----------------------
def on_sigint(signum, frame):
    print("\n[MAIN] SIGINT received, stopping...")
    stop_event.set()

signal.signal(signal.SIGINT, on_sigint)

if __name__ == "__main__":
    t_udp = threading.Thread(target=udp_listener, daemon=True)
    t_play = threading.Thread(target=player_and_writer, daemon=True)
    t_udp.start()
    t_play.start()

    # wait until both threads finish (stop_event will be set via Ctrl+C)
    try:
        while t_udp.is_alive() and t_play.is_alive():
            time.sleep(0.3)
    except KeyboardInterrupt:
        stop_event.set()

    print("[MAIN] Exiting.")
