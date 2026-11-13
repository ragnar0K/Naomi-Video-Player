#!/usr/bin/env python3
"""
stream_client.py - Live Video & Audio Network Streaming Client for Sega NAOMI
-----------------------------------------------------------------------------
Streams any video file (mp4, mkv, avi, webm) over TCP port 10703 directly into
the running NAOMI Video Player's Net DIMM Cartridge RAM ringbuffer (0x04010000),
with full real-time arcade joystick seeking (-10s/+10s, -60s/+60s).

Usage:
    python3 stream_client.py <NAOMI_IP> <VIDEO_FILE> [--fps 15] [--start 0]
"""

import argparse
import os
import socket
import struct
import subprocess
import sys
import time
from typing import Optional

STREAM_INFO_ADDR   = 0x04000000
STREAM_RING_BASE   = 0x04010000
STREAM_RING_SLOTS  = 64
STREAM_SLOT_STRIDE = 0x00010000  # 64 KB per slot

STREAM_MAGIC_ACTIVE = 0x5354524D  # "STRM"
STREAM_MAGIC_RESET  = 0x52534554  # "RSET"

REG_STREAM_STATE = 0xC0DE10
REG_WRITE_SEQ    = 0xC0DE20
REG_READ_SEQ     = 0xC0DE30
REG_SEEK_REQ     = 0xC0DE40


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("Socket closed by Net DIMM")
        buf.extend(chunk)
    return bytes(buf)


class NetDimmStreamLink:
    """Lightweight, zero-dependency Net DIMM TCP 10703 client."""

    def __init__(self, ip: str, port: int = 10703):
        self.ip = ip
        self.port = port
        self.sock: Optional[socket.socket] = None

    def connect(self) -> None:
        if self.sock is not None:
            return
        for attempt in range(5):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(2.0)
                s.connect((self.ip, self.port))
                s.settimeout(5.0)
                # Startup NOP packet (0x01)
                s.sendall(struct.pack("<I", 0x01000000))
                self.sock = s
                return
            except OSError:
                time.sleep(0.15)
        raise ConnectionError(f"Could not connect to Net DIMM at {self.ip}:{self.port}")

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def __enter__(self) -> "NetDimmStreamLink":
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def set_time_limit(self, minutes: int = 10) -> None:
        assert self.sock is not None
        self.sock.sendall(struct.pack("<II", 0x17000004, minutes))

    def poke32(self, addr: int, val: int) -> None:
        assert self.sock is not None
        self.sock.sendall(struct.pack("<IIII", 0x1100000C, addr & 0xFFFFFFFF, 3, val & 0xFFFFFFFF))

    def peek32(self, addr: int) -> int:
        assert self.sock is not None
        self.sock.sendall(struct.pack("<III", 0x10000008, addr & 0xFFFFFFFF, 3))
        resp = recv_exact(self.sock, 12)
        hdr, _status, val = struct.unpack("<III", resp)
        if (hdr >> 24) != 0x10:
            raise RuntimeError(f"Unexpected peek response header: 0x{hdr:08X}")
        return val

    def upload_chunk(self, offset: int, data: bytes) -> None:
        assert self.sock is not None
        addr = 0
        total = len(data)
        seq = 1
        while addr < total:
            chunk = data[addr:addr + 0x8000]
            curlen = len(chunk)
            last = (addr + curlen) == total
            flags = 0x81 if last else 0x80
            payload = struct.pack("<IIH", seq, offset + addr, 0) + chunk
            hdr = (0x04 << 24) | (flags << 16) | len(payload)
            self.sock.sendall(struct.pack("<I", hdr) + payload)
            addr += curlen
            seq += 1


def probe_video_duration(video_path: str) -> float:
    try:
        out = subprocess.check_output(
            [
                "ffprobe", "-v", "error",
                "-show_entries", "format=duration",
                "-of", "default=noprint_wrappers=1:nokey=1",
                video_path,
            ],
            text=True,
        ).strip()
        return float(out)
    except Exception:
        return 3600.0


def ensure_encoder_built(root_dir: str) -> str:
    enc_bin = os.path.join(root_dir, "tools", "encode_nvp")
    enc_src = os.path.join(root_dir, "tools", "encode_nvp.c")
    if not os.path.exists(enc_bin) or os.path.getmtime(enc_src) > os.path.getmtime(enc_bin):
        print("Compiling host stream encoder tools/encode_nvp...")
        subprocess.check_call(["gcc", "-O3", "-Wall", enc_src, "-o", enc_bin])
    return enc_bin


def read_frame_packet(pipe) -> Optional[bytes]:
    hdr = pipe.read(12)
    if not hdr or len(hdr) < 12:
        return None
    audio_samples, flags, video_bytes, audio_bytes = struct.unpack("<HHII", hdr)
    payload = pipe.read(audio_bytes + video_bytes)
    if len(payload) < audio_bytes + video_bytes:
        return None
    pkt = hdr + payload
    if len(pkt) & 1:
        pkt += b"\x00"
    return pkt


def start_stream_session(
    link: NetDimmStreamLink,
    enc_bin: str,
    video_path: str,
    fps: int,
    total_frames: int,
    start_frame: int,
):
    start_sec = start_frame / float(fps)
    title_bytes = os.path.basename(video_path).encode("utf-8")[:63]
    title_bytes = title_bytes.ljust(64, b"\x00")

    sinfo = struct.pack(
        "<IHHHHII64s",
        STREAM_MAGIC_ACTIVE,
        320,
        240,
        fps,
        16000,
        total_frames,
        start_frame,
        title_bytes,
    )

    proc = subprocess.Popen(
        [enc_bin, "--stream", video_path, f"{start_sec:.2f}", str(fps)],
        stdout=subprocess.PIPE,
        bufsize=262144,
    )

    # Pre-buffer initial 15 frames (1 second) before signaling reset to NAOMI
    initial_frames = []
    for _ in range(15):
        assert proc.stdout is not None
        pkt = read_frame_packet(proc.stdout)
        if not pkt:
            break
        initial_frames.append(pkt)

    with link:
        link.set_time_limit(10)
        link.upload_chunk(STREAM_INFO_ADDR, sinfo)
        for idx, pkt in enumerate(initial_frames):
            slot_addr = STREAM_RING_BASE + ((idx % STREAM_RING_SLOTS) * STREAM_SLOT_STRIDE)
            link.upload_chunk(slot_addr, pkt)
        link.poke32(REG_SEEK_REQ, 0)
        link.poke32(REG_WRITE_SEQ, len(initial_frames))
        link.poke32(REG_STREAM_STATE, STREAM_MAGIC_RESET)

    return proc, len(initial_frames)


def main() -> int:
    parser = argparse.ArgumentParser(description="Stream any video file live to Sega NAOMI Video Player over Net DIMM.")
    parser.add_argument("ip", help="IP address of the Sega NAOMI Net DIMM")
    parser.add_argument("video", help="Path to video file (.mp4, .mkv, .avi, .webm)")
    parser.add_argument("--fps", type=int, default=15, help="Target video FPS (default: 15)")
    parser.add_argument("--start", type=float, default=0.0, help="Start time in seconds (default: 0)")
    args = parser.parse_args()

    if not os.path.isfile(args.video):
        print(f"Error: video file '{args.video}' not found.", file=sys.stderr)
        return 1

    root_dir = os.path.dirname(os.path.abspath(__file__))
    enc_bin = ensure_encoder_built(root_dir)

    duration_sec = probe_video_duration(args.video)
    total_frames = max(1, int(duration_sec * args.fps))
    start_frame = max(0, int(args.start * args.fps))

    print(f"=== NAOMI Live Video Streamer ===")
    print(f"Target Net DIMM : {args.ip}:10703")
    print(f"Video File      : {os.path.basename(args.video)} ({int(duration_sec)//60:02d}:{int(duration_sec)%60:02d})")
    print(f"Stream Format   : 320x240 Full-Res 4-Color BTC @ {args.fps} fps + 16kHz 16-bit Stereo PCM")

    link = NetDimmStreamLink(args.ip)
    proc, write_seq = start_stream_session(
        link, enc_bin, args.video, args.fps, total_frames, start_frame
    )
    cur_base_frame = start_frame

    try:
        while True:
            with link:
                link.set_time_limit(10)
                read_seq = link.peek32(REG_READ_SEQ)
                seek_req = link.peek32(REG_SEEK_REQ)

            # Handle live joystick seek request from the NAOMI arcade cabinet
            if (seek_req & 0x80000000) != 0 and seek_req != 0xFFFFFFFF:
                target_frame = seek_req & 0x7FFFFFFF
                if target_frame >= total_frames:
                    target_frame = 0
                print(f"\n[NAOMI Joystick Seek] Jumping to {target_frame // args.fps // 60:02d}:{(target_frame // args.fps) % 60:02d}...")
                proc.terminate()
                proc.wait()
                cur_base_frame = target_frame
                proc, write_seq = start_stream_session(
                    link, enc_bin, args.video, args.fps, total_frames, cur_base_frame
                )
                continue

            if read_seq == 0xFFFFFFFF:
                read_seq = 0

            buffered = write_seq - read_seq
            # Keep up to 44 frames (~3 seconds) buffered ahead in the 64-slot ringbuffer
            if buffered < 44:
                to_send = min(6, 48 - buffered)
                batch = []
                for _ in range(to_send):
                    assert proc.stdout is not None
                    pkt = read_frame_packet(proc.stdout)
                    if not pkt:
                        break
                    batch.append(pkt)

                if not batch:
                    # End of movie reached -> loop back to beginning
                    print("\nEnd of movie reached, looping back to 00:00...")
                    proc.terminate()
                    proc.wait()
                    cur_base_frame = 0
                    proc, write_seq = start_stream_session(
                        link, enc_bin, args.video, args.fps, total_frames, 0
                    )
                    continue

                with link:
                    for pkt in batch:
                        slot_addr = STREAM_RING_BASE + ((write_seq % STREAM_RING_SLOTS) * STREAM_SLOT_STRIDE)
                        link.upload_chunk(slot_addr, pkt)
                        write_seq += 1
                    link.poke32(REG_WRITE_SEQ, write_seq)

                cur_sec = (cur_base_frame + read_seq) // args.fps
                tot_sec = total_frames // args.fps
                print(
                    f"Streaming: {cur_sec // 60:02d}:{cur_sec % 60:02d} / {tot_sec // 60:02d}:{tot_sec % 60:02d} "
                    f"| Buffered: {write_seq - read_seq:2d}/{STREAM_RING_SLOTS} frames\r",
                    end="",
                    flush=True,
                )
            else:
                time.sleep(0.08)

    except KeyboardInterrupt:
        print("\nStopping stream client...")
    finally:
        proc.terminate()

    return 0


if __name__ == "__main__":
    sys.exit(main())
