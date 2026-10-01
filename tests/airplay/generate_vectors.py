"""Generate protocol fixtures from the PoC without device or network access."""
import importlib.util
import json
from pathlib import Path
import struct
import sys

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("airplay_poc", root / "airplay_poc.py")
poc = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = poc
spec.loader.exec_module(poc)

salt = bytes(range(16))
private = bytes(range(1, 33))
server = pow(5, 0x123456789ABCDEF, poc.SRP_N).to_bytes(384, "big")
public, proof, expected, shared = poc.srp_challenge(salt, server, int.from_bytes(private, "big"))
key = bytes(range(32))
pcm = struct.pack("<" + "h" * 704, *(((i * 997) % 65536) - 32768 for i in range(704)))
records = poc.HapRecords(key, key)
record_plain = bytes(i % 256 for i in range(2051))
vectors = {
    "salt": salt.hex(), "private": private.hex(), "server": server.hex(),
    "public": public.hex(), "proof": proof.hex(), "expected": expected.hex(), "shared": shared.hex(),
    "writeKey": poc.control_key(shared, "Write").hex(), "readKey": poc.control_key(shared, "Read").hex(),
    "pcm": pcm.hex(), "alac": poc.alac_frame(pcm).hex(),
    "hapPlain": record_plain.hex(), "hapWire": records.encode(record_plain).hex(),
    "audio": poc.audio_packet(key, pcm, 65535, 0xFFFFFFFF, 0, True).hex(),
    "audioWrapped": poc.audio_packet(key, pcm, 65536, 0x10000015F, 1, False).hex(),
    "ptpHeader": poc.ptp_header(8, 96, 0x456789ABCDEF0123, 65535).hex(),
    "ptpTime": poc.ptp_time(1790723456123456789).hex(),
    "syncBefore": poc.sync_packet(0x456789ABCDEF0123, 0xFFFFFF00, 1790723456123456789,
                                  1790723454123456788, 44100, True).hex(),
    "syncAfter": poc.sync_packet(0x456789ABCDEF0123, 0xFFFFFF00, 1790723456123456789,
                                 1790723456234567890, 44100, False).hex(),
}
path = Path(__file__).with_name("vectors.json")
path.write_text(json.dumps(vectors, indent=2) + "\n", encoding="utf-8")
print(f"Generated {len(vectors)} PoC fixtures: {path}")
