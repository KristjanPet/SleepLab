"""Record framed SleepLab STM32 USB CDC mono PCM16 into a playable WAV.

Examples:
    python tools/record_audio.py --port COM8 --seconds 10
    python tools/record_audio.py --port COM8 --seconds 10 --play
"""

import argparse
import struct
import time
import wave

import serial

MAGIC = b"SLAB"
RATE = 32106               # Approximate actual I2S frame rate shown by CubeMX
FRAMES_PER_PACKET = 256
PAYLOAD_BYTES = FRAMES_PER_PACKET * 2
PACKET_BYTES = 8 + PAYLOAD_BYTES


def packets(port, stop_at):
    """Yield (sequence_number, pcm16_le_bytes). Recover packet boundaries."""
    buffer = bytearray()
    while time.monotonic() < stop_at:
        start = buffer.find(MAGIC)
        if start < 0:
            # Keep the last three bytes in case the header straddles reads.
            if len(buffer) > 3:
                del buffer[:-3]
        elif start > 0:
            del buffer[:start]

        if buffer.startswith(MAGIC) and len(buffer) >= PACKET_BYTES:
            sequence = struct.unpack_from("<I", buffer, 4)[0]
            pcm = bytes(buffer[8:PACKET_BYTES])
            del buffer[:PACKET_BYTES]
            yield sequence, pcm
            continue

        data = port.read(4096)
        if data:
            buffer.extend(data)


def play_wav(filename):
    try:
        import sounddevice as sd
    except ImportError as exc:
        raise SystemExit("Install optional playback support: pip install sounddevice") from exc

    with wave.open(filename, "rb") as wav:
        with sd.RawOutputStream(samplerate=wav.getframerate(),
                                channels=1, dtype="int16", latency="high") as output:
            while True:
                data = wav.readframes(2048)
                if not data:
                    break
                output.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8", help="STM32 USB CDC port")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--output", default="sleeplab.wav")
    ap.add_argument("--play", action="store_true", help="Play WAV after recording")
    args = ap.parse_args()

    if args.seconds <= 0:
        ap.error("--seconds must be positive")

    packets_needed = max(1, round(args.seconds * RATE / FRAMES_PER_PACKET))
    missing = 0
    received = 0
    previous = None

    print(f"Recording {packets_needed * FRAMES_PER_PACKET / RATE:.2f}s from {args.port} ...")
    print("Close the serial terminal first; only this program may open the COM port.")

    with serial.Serial(args.port, baudrate=115200, timeout=0.05) as port:
        # Baudrate setting does not control native USB CDC transmission speed.
        with wave.open(args.output, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(RATE)

            start = time.monotonic()
            max_wall_seconds = args.seconds + 15.0
            for sequence, pcm in packets(port, start + max_wall_seconds):
                if previous is not None:
                    gap = (sequence - previous - 1) & 0xFFFFFFFF
                    if gap:
                        print(f"Missing audio packets: {gap} at sequence {sequence}")
                        missing += gap
                        if gap <= 1000:
                            wav.writeframesraw(b"\x00" * (gap * PAYLOAD_BYTES))
                            received += gap
                        else:
                            print("Large gap; resynchronizing without inserting silence.")

                wav.writeframesraw(pcm)
                received += 1
                previous = sequence
                if received >= packets_needed:
                    break
                if time.monotonic() - start > max_wall_seconds:
                    print("Stopped: not enough audio arrived from the STM32.")
                    break

    print(f"Saved {args.output}; audio packets missing: {missing}")
    if args.play:
        play_wav(args.output)


if __name__ == "__main__":
    main()
