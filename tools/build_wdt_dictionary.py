from pathlib import Path
import math
import struct

ROOT = Path(r"C:\WhistleEmbeddings")
RESULTS = ROOT / "results"
OUT_WDT = RESULTS / "commands.wdt"

THRESHOLDS_7 = {
    "hey_lisa_kids":   {"distance": 0.253520, "margin": 0.164960},
    "napred_kids":     {"distance": 0.358700, "margin": 0.004250},
    "pauziraj_kids":   {"distance": 0.288180, "margin": 0.071550},
    "predishna_kids":  {"distance": 0.318640, "margin": 0.129890},
    "sledvashta_kids": {"distance": 0.278600, "margin": 0.136000},
    "spri_kids":       {"distance": 0.335190, "margin": 0.061520},
    "zaspi_kids":      {"distance": 0.281640, "margin": 0.088800},
}

UNITS_9 = list(THRESHOLDS_7.keys()) + ["da_kids", "ne_kids"]
DIMENSIONS = 512

def load_raw_f32(path: Path):
    raw = path.read_bytes()
    if len(raw) == 0 or len(raw) % (DIMENSIONS * 4) != 0:
        raise ValueError(f"Invalid .f32 size: {path} ({len(raw)} bytes)")
    count = len(raw) // 4
    vals = struct.unpack("<" + "f" * count, raw)
    if not all(math.isfinite(x) for x in vals):
        raise ValueError(f"NaN/Inf in {path}")
    num_frames = count // DIMENSIONS
    return num_frames, raw

def pack_wdt(results_dir: Path, out_file: Path, class_order=UNITS_9):
    payload = bytearray()
    total_templates = 0

    for cmd in class_order:
        folder = results_dir / cmd
        files = sorted(folder.glob("*.f32"))
        if not files:
            raise FileNotFoundError(f"No .f32 templates found in {folder}")

        has_thresh = 1 if cmd in THRESHOLDS_7 else 0
        t_dist = THRESHOLDS_7[cmd]["distance"] if has_thresh else 0.0
        t_marg = THRESHOLDS_7[cmd]["margin"] if has_thresh else 0.0

        name_bytes = cmd.encode("ascii")[:31].ljust(32, b"\x00")
        payload += struct.pack("<32sB3xffI", name_bytes, has_thresh, t_dist, t_marg, len(files))

        for f in files:
            num_frames, raw = load_raw_f32(f)
            payload += struct.pack("<I", num_frames)
            payload += raw
            total_templates += 1

    header_size = 20
    total_size = header_size + len(payload)
    header = struct.pack("<4sIIII", b"WDT1", 1, len(class_order), DIMENSIONS, total_size)

    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_bytes(header + payload)
    print(f"Packed {total_templates} templates across {len(class_order)} classes into {out_file} ({total_size} bytes)")

if __name__ == "__main__":
    pack_wdt(RESULTS, OUT_WDT)
