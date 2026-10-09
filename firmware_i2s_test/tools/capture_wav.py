# /// script
# dependencies = ["pyserial"]
# ///
"""Enregistre le flux audio de firmware_i2s_test.

Usage : uv run tools/capture_wav.py PORT DUREE SORTIE [pcm|g722] [chemin_ffmpeg]
  pcm  (defaut) : PCM 16 kHz -> SORTIE.wav
  g722          : trames G.722 de 20 ms -> SORTIE.g722, puis decodage par ffmpeg
                  -> SORTIE.wav (si ffmpeg est trouve)
"""
import math
import os
import struct
import subprocess
import sys
import time
import wave

import serial

MAGIC = bytes([0xA5, 0x5A, 0xC3, 0x3C])
HEADER_LEN = 8
RATE = 16000
DEFAULT_FFMPEG = os.path.join(os.path.expanduser("~"), "softs_portable", "ffmpeg", "bin", "ffmpeg.exe")


def read_stream(port, duration, command):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.5
    # DTR/RTS pilotent le reset sur l'USB-Serial/JTAG de l'ESP32-S3 : les
    # laisser inactifs a l'ouverture pour ne pas redemarrer la carte.
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.5)
    ser.reset_input_buffer()
    ser.write(command)
    print("Flux %s demande, enregistrement de %.1f s..." % (command.decode(), duration))

    buf = bytearray()
    payload = bytearray()
    stats = {"frames": 0, "lost": 0, "resyncs": 0, "sizes": set()}
    last_seq = None
    end = time.time() + duration
    while time.time() < end:
        buf += ser.read(4096)
        while True:
            i = buf.find(MAGIC)
            if i < 0:
                del buf[:-3]
                break
            if i > 0:
                if stats["frames"] > 0:
                    stats["resyncs"] += 1
                del buf[:i]
            if len(buf) < HEADER_LEN:
                break
            seq, n = struct.unpack_from("<HH", buf, 4)
            if n > 2000:
                del buf[:1]
                stats["resyncs"] += 1
                continue
            if len(buf) < HEADER_LEN + n:
                break
            payload += buf[HEADER_LEN:HEADER_LEN + n]
            del buf[:HEADER_LEN + n]
            if last_seq is not None:
                stats["lost"] += (seq - last_seq - 1) & 0xFFFF
            last_seq = seq
            stats["frames"] += 1
            stats["sizes"].add(n)
    ser.close()
    return bytes(payload), stats


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


def report_level(samples):
    rms = math.sqrt(sum(s * s for s in samples) / len(samples))
    peak = max(abs(s) for s in samples)
    print("Niveau : rms %.1f dBFS, pic %.1f dBFS" % (
        20 * math.log10(max(rms, 1e-9) / 32768), 20 * math.log10(max(peak, 1) / 32768)))


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM15"
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
    out = sys.argv[3] if len(sys.argv) > 3 else "capture_16k"
    mode = sys.argv[4] if len(sys.argv) > 4 else "pcm"
    ffmpeg = sys.argv[5] if len(sys.argv) > 5 else DEFAULT_FFMPEG
    base = os.path.splitext(out)[0]

    payload, st = read_stream(port, duration, b"G" if mode == "g722" else b"S")
    print("Trames recues : %d, perdues : %d, resynchronisations : %d, tailles : %s" % (
        st["frames"], st["lost"], st["resyncs"], sorted(st["sizes"])))
    if not payload:
        print("Aucune trame recue (carte deja en mode flux ? la redemarrer)")
        return 1

    if mode == "g722":
        g722_path = base + ".g722"
        with open(g722_path, "wb") as f:
            f.write(payload)
        print("G.722 : %d octets (%.2f s), fichier %s" % (len(payload), len(payload) / 8000, g722_path))
        if not os.path.exists(ffmpeg):
            print("ffmpeg introuvable, pas de decodage")
            return 0
        wav_path = base + ".wav"
        subprocess.run([ffmpeg, "-loglevel", "error", "-y", "-f", "g722", "-i", g722_path,
                        "-ar", str(RATE), wav_path], check=True)
        with wave.open(wav_path) as w:
            data = w.readframes(w.getnframes())
        samples = struct.unpack("<%dh" % (len(data) // 2), data)
        print("Decode par ffmpeg : %s" % wav_path)
    else:
        samples = struct.unpack("<%dh" % (len(payload) // 2), payload)
        wav_path = base + ".wav"
        write_wav(wav_path, samples)
        print("Echantillons : %d (%.2f s), fichier %s" % (len(samples), len(samples) / RATE, wav_path))
    report_level(samples)
    return 0


if __name__ == "__main__":
    sys.exit(main())
