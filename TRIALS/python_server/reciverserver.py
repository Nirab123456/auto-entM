#!/usr/bin/env python3
"""
esp_receiver.py

TCP receiver for the ESP32 streamer (header format defined in the ESP sketch).
Plays received audio via sounddevice and writes to disk using soundfile.

Author: assistant
"""

import socket
import threading
import struct
import time
import numpy as np
import sounddevice as sd
import soundfile as sf
import queue
import sys
import signal

# --------------------------- CONFIG ---------------------------
LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 7000         # must match PC_PORT on ESP

# Buffering / playback config
SAMPLE_RATE = 48000        # must match the ESP setting
CHANNELS = 1               # mono (ESP sends only mic channel)
BYTES_PER_SAMPLE = 4       # 32-bit slots from ESP
BUFFER_SECONDS = 8         # ring buffer size in seconds (store several seconds to cope with jitter)
RING_BUFFER_SIZE = SAMPLE_RATE * BUFFER_SECONDS

# Jitter/playout: how far behind to play relative to the latest received sample (in seconds)
# choose e.g., 0.1..0.5 depending on network stability. 0.2s is a good default.
PLAYOUT_LATENCY_SECONDS = 0.20
PLAYOUT_LATENCY_FRAMES = int(PLAYOUT_LATENCY_SECONDS * SAMPLE_RATE)

# Header constants (must match ESP header)
HEADER_MAGIC = 0x45535032  # 'ESP2' low-endian encoded by ESP
HEADER_SIZE = 34
FORMAT_INT32_LEFT24 = 1

# Output file
OUT_FILENAME = "received_high_quality.wav"
OUT_SUBTYPE = "PCM_24"     # use 24-bit WAV (or "FLOAT" for float32)

# Timeout for writer to wait for missing packets before zero-filling (seconds)
WRITE_MISSING_TIMEOUT = 0.25

# ------------------------- GLOBAL STATE ------------------------
# ring buffer for playback (float32 normalized [-1.0, 1.0])
ring = np.zeros(RING_BUFFER_SIZE, dtype=np.float32)

# per-sample "availability" marker: stores packet seq that filled that sample, or -1 if empty
avail = np.full(RING_BUFFER_SIZE, -1, dtype=np.int32)

# lock protecting ring and avail
ring_lock = threading.Lock()

# bookkeeping for writing to disk
next_write_index = None   # absolute sample index we will write next to disk
write_lock = threading.Lock()

# highest received sample index (for monitoring)
highest_received_index = -1

# control for threads
shutdown_event = threading.Event()

# queue for control/log messages
log_q = queue.Queue()

# ------------------------- UTIL FUNCTIONS ----------------------

def log(msg, *args):
    ts = time.strftime("%H:%M:%S")
    line = f"[{ts}] " + (msg % args if args else msg)
    try:
        log_q.put_nowait(line)
    except queue.Full:
        pass

def int64_from_bytes(b, offset):
    # little-endian uint64
    return int.from_bytes(b[offset:offset+8], 'little', signed=False)

def int32_from_bytes(b, offset):
    return int.from_bytes(b[offset:offset+4], 'little', signed=False)

# ---------------------- NETWORK / PARSE ------------------------

def recvall(sock, n):
    """Receive exactly n bytes from socket or raise IOError"""
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise IOError("Socket closed")
        data.extend(chunk)
    return bytes(data)

def handle_client_connection(client_sock, client_addr):
    """Main loop for receiving packets from ESP and placing them into the ring buffer."""
    global next_write_index, highest_received_index
    client_sock.settimeout(5.0)
    log("Client connected from %s:%d", client_addr[0], client_addr[1])

    # when first packet arrives, we'll initialize next_write_index to first_sample_index
    first_packet_seen = False

    try:
        while not shutdown_event.is_set():
            # read header
            header = recvall(client_sock, HEADER_SIZE)
            if len(header) != HEADER_SIZE:
                raise IOError("Header truncated")

            # parse header (little-endian as in ESP sketch)
            magic = int.from_bytes(header[0:4], 'little', signed=False)
            if magic != HEADER_MAGIC:
                log("Invalid magic 0x%08X != expected 0x%08X - resyncing", magic, HEADER_MAGIC)
                # try to re-sync: continue (could read again)
                continue

            seq = int.from_bytes(header[4:8], 'little', signed=False)
            first_sample_index = int.from_bytes(header[8:16], 'little', signed=False)
            timestamp_us = int.from_bytes(header[16:24], 'little', signed=False)
            frames = int.from_bytes(header[24:26], 'little', signed=False)
            channels = header[26]
            bytes_per_sample = header[27]
            sample_rate = int.from_bytes(header[28:32], 'little', signed=False)
            format_id = int.from_bytes(header[32:34], 'little', signed=False)

            # basic checks
            if sample_rate != SAMPLE_RATE:
                log("WARNING: sample_rate mismatch: packet %d has %d, expected %d", seq, sample_rate, SAMPLE_RATE)
            if channels != CHANNELS:
                log("WARNING: channels mismatch: packet %d has %d, expected %d", seq, channels, CHANNELS)
            if bytes_per_sample != BYTES_PER_SAMPLE:
                log("WARNING: bytes_per_sample mismatch: %d != %d", bytes_per_sample, BYTES_PER_SAMPLE)

            # compute payload size
            payload_bytes = frames * channels * bytes_per_sample

            # receive payload
            payload = recvall(client_sock, payload_bytes)

            # Convert payload (32-bit little endian words containing left-aligned 24-bit samples)
            # payload is consecutive int32 for each sample (mono). We'll interpret as little-endian int32 array.
            int32_arr = np.frombuffer(payload, dtype='<i4')  # little-endian int32
            # Convert left-aligned 24-bit to signed 24-bit -> right-shift 8 bits (arithmetic)
            # Numpy right-shift on signed int behaves as arithmetic shift in practice
            shifted = (int32_arr >> 8).astype(np.int32)
            # normalize to float32 in range ~[-1, 1]
            float_arr = shifted.astype(np.float32) / 8388608.0  # 2^23

            # if this is the first packet we see, initialize next_write_index and playback base
            if not first_packet_seen:
                with write_lock:
                    next_write_index = first_sample_index
                # start playback index a little behind the first sample to allow buffer fill
                # the playback callback computes its own start based on next_write_index
                first_packet_seen = True
                log("First packet: seq=%d first_sample_index=%d frames=%d ts_us=%d", seq, first_sample_index, frames, timestamp_us)

            # Put samples into ring buffer in correct absolute positions
            with ring_lock:
                # update highest_received_index for monitoring
                end_index = first_sample_index + frames - 1
                if end_index > highest_received_index:
                    highest_received_index = end_index

                # write into ring with modulo wrapping
                pos = int(first_sample_index % RING_BUFFER_SIZE)
                if pos + frames <= RING_BUFFER_SIZE:
                    ring[pos:pos+frames] = float_arr
                    avail[pos:pos+frames] = seq
                else:
                    # split across wrap
                    first_len = RING_BUFFER_SIZE - pos
                    ring[pos:pos+first_len] = float_arr[:first_len]
                    avail[pos:pos+first_len] = seq
                    rest = frames - first_len
                    ring[0:rest] = float_arr[first_len:]
                    avail[0:rest] = seq

            # Wake writer thread if new data is available
            # We'll use a simple notify via writing to log queue (writer polls)
            log("pkt seq=%d idx=%d frames=%d ts_us=%d", seq, first_sample_index, frames, timestamp_us)

    except Exception as e:
        log("Client connection ended or error: %s", str(e))
    finally:
        try:
            client_sock.shutdown(socket.SHUT_RDWR)
        except Exception:
            pass
        client_sock.close()
        log("Client socket closed")

# ---------------------- PLAYBACK (sounddevice) ----------------------

# playback state
playback_start_index = None  # absolute sample index being played at start
playback_index_lock = threading.Lock()
playback_index = 0           # next sample index the callback should output

def audio_callback(outdata, frames, time_info, status):
    """sounddevice callback. Must be very fast and non-blocking."""
    global playback_index, playback_start_index
    # outdata is Nx1 float32 buffer
    if status:
        log("sounddevice status: %s", status)

    # Try to get lock quickly; if not available, output silence to avoid blocking
    got = ring_lock.acquire(False)
    if not got:
        outdata.fill(0.0)
        return
    try:
        # If playback_start_index not initialized, we can't play; fill silence
        if playback_start_index is None:
            outdata.fill(0.0)
            return

        # read samples from ring according to playback_index
        buf = np.empty((frames,), dtype=np.float32)
        for i in range(frames):
            idx = playback_index + i
            pos = int(idx % RING_BUFFER_SIZE)
            # if available marker is -1 (no data) then output 0
            if avail[pos] == -1:
                buf[i] = 0.0
            else:
                buf[i] = ring[pos]
        playback_index += frames
        outdata[:] = buf.reshape(outdata.shape)  # assign
    finally:
        ring_lock.release()

def start_playback_stream():
    """Start the sounddevice OutputStream with the callback."""
    global playback_start_index, playback_index
    # Wait until we have at least some data to start playback
    # We want to start playback at (highest_received_index - playout_latency)
    startup_wait = 0.0
    while next_write_index is None and not shutdown_event.is_set():
        time.sleep(0.01)
        startup_wait += 0.01
        if startup_wait > 5.0:
            log("No packets received yet - waiting for first packet")
            startup_wait = 0.0

    # Determine initial playback_index: set to first observed sample + playout latency
    # with playground := playback_index_lock:  # just to show intent; will not be used
    #     pass

    # compute initial index: we use next_write_index + PLAYOUT_LATENCY_FRAMES to allow buffer fill
    with write_lock:
        base = next_write_index if next_write_index is not None else 0
    with playback_index_lock:
        playback_start_index = base + PLAYOUT_LATENCY_FRAMES
        playback_index = playback_start_index
    log("Starting playback: playback_start_index=%d (latency frames=%d)", playback_start_index, PLAYOUT_LATENCY_FRAMES)

    # open sounddevice output stream, mono
    stream = sd.OutputStream(samplerate=SAMPLE_RATE, channels=1, dtype='float32', callback=audio_callback, blocksize=1024)
    stream.start()
    log("sounddevice output stream started")
    return stream

# ------------------------- WRITER THREAD --------------------------

def writer_thread_fn():
    """Continuously write contiguous audio to disk based on next_write_index.
       We read from the ring buffer and write to the soundfile. If data is missing
       for longer than WRITE_MISSING_TIMEOUT, we write zeros to preserve timeline.
    """
    global next_write_index
    # open soundfile for writing
    sf_file = sf.SoundFile(OUT_FILENAME, mode='w', samplerate=SAMPLE_RATE, channels=1, subtype=OUT_SUBTYPE)
    log("Opened output file %s (samplerate=%d, subtype=%s)", OUT_FILENAME, SAMPLE_RATE, OUT_SUBTYPE)

    # Wait until writer has an initial base index
    while next_write_index is None and not shutdown_event.is_set():
        time.sleep(0.01)

    if shutdown_event.is_set():
        sf_file.close()
        return

    log("Writer starting at sample index %d", next_write_index)

    last_progress_time = time.time()
    while not shutdown_event.is_set():
        # attempt to write any contiguous region starting at next_write_index
        contiguous_frames = 0
        # collect frames into a list or array for a max chunk
        max_chunk = 8192
        chunk_list = []

        with ring_lock:
            # check availability of samples starting at next_write_index
            for i in range(max_chunk):
                idx = next_write_index + i
                pos = int(idx % RING_BUFFER_SIZE)
                if avail[pos] != -1:
                    chunk_list.append(ring[pos])
                else:
                    break
            contiguous_frames = len(chunk_list)

        if contiguous_frames > 0:
            # write contiguous_frames to disk
            arr = np.array(chunk_list, dtype=np.float32)
            sf_file.write(arr)
            with write_lock:
                next_write_index += contiguous_frames
            # mark avail entries as consumed (optional) to reduce memory retention
            with ring_lock:
                for i in range(contiguous_frames):
                    pos = int((next_write_index - contiguous_frames + i) % RING_BUFFER_SIZE)
                    avail[pos] = -1
            last_progress_time = time.time()
            continue

        # no contiguous data available at the moment
        # if nothing new for WRITE_MISSING_TIMEOUT, zero-fill one packet-worth to keep timeline moving
        if time.time() - last_progress_time > WRITE_MISSING_TIMEOUT:
            zeros = np.zeros(1024, dtype=np.float32)
            sf_file.write(zeros)
            with write_lock:
                next_write_index += 1024
            log("Writer: missing data for %.3fs -> zero-filled 1024 frames at idx=%d", WRITE_MISSING_TIMEOUT, next_write_index - 1024)
            last_progress_time = time.time()
            continue

        # otherwise wait briefly
        time.sleep(0.005)

    sf_file.close()
    log("Writer thread exiting, file closed.")

# ------------------------ TCP SERVER MAIN ------------------------

def tcp_server_thread():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((LISTEN_HOST, LISTEN_PORT))
    s.listen(1)
    s.settimeout(1.0)
    log("PCM SERVER listening on %s:%d", LISTEN_HOST, LISTEN_PORT)
    while not shutdown_event.is_set():
        try:
            client_sock, client_addr = s.accept()
            # handle single client in dedicated thread
            client_thread = threading.Thread(target=handle_client_connection, args=(client_sock, client_addr), daemon=True)
            client_thread.start()
        except socket.timeout:
            continue
        except Exception as e:
            log("Server accept error: %s", str(e))
            time.sleep(0.1)
    s.close()
    log("TCP server thread exiting")

# ------------------------- LOG PRINTER ----------------------------

def log_printer():
    while not shutdown_event.is_set():
        try:
            line = log_q.get(timeout=0.5)
            print(line)
        except queue.Empty:
            continue

# --------------------------- MAIN --------------------------------

def main():
    # catch signals
    def on_sigint(sig, frame):
        log("SIGINT received, shutting down...")
        shutdown_event.set()
    signal.signal(signal.SIGINT, on_sigint)
    signal.signal(signal.SIGTERM, on_sigint)

    # start threads
    threads = []
    t_srv = threading.Thread(target=tcp_server_thread, daemon=True)
    t_srv.start()
    threads.append(t_srv)

    t_writer = threading.Thread(target=writer_thread_fn, daemon=True)
    t_writer.start()
    threads.append(t_writer)

    t_log = threading.Thread(target=log_printer, daemon=True)
    t_log.start()
    threads.append(t_log)

    # Start playback (blocks until first packet arrives and next_write_index assigned)
    stream = start_playback_stream()

    # Main thread loops until shutdown
    try:
        while not shutdown_event.is_set():
            # print periodic status
            time.sleep(1.0)
            with ring_lock:
                hr = highest_received_index
            with write_lock:
                nw = next_write_index
            with playback_index_lock:
                pb = playback_index
            log("STAT highest_received=%d next_write=%s playback_index=%s", hr, str(nw), str(pb))
    except KeyboardInterrupt:
        shutdown_event.set()

    # cleanup
    stream.stop()
    stream.close()
    log("Playback stopped. Waiting for threads to exit...")
    time.sleep(0.5)

if __name__ == "__main__":
    main()
