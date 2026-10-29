"""Read one user-approved prototype texture; never modify the Noita install."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
ENTRY = "data/materials_gfx/rock.png"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path(r"E:\SteamLibrary\steamapps\common\Noita"))
    args = parser.parse_args()
    archive = args.source / "data/data.wak"
    archive_size = archive.stat().st_size
    with archive.open("rb") as stream:
        _, count, table_end, _ = struct.unpack("<4I", stream.read(16))
        if count > 100000 or table_end > archive_size:
            raise ValueError("Invalid WAK index header")
        chosen = None
        for _ in range(count):
            offset, size, name_size = struct.unpack("<3I", stream.read(12))
            if name_size > 1024 or stream.tell() + name_size > table_end:
                raise ValueError("Invalid WAK entry name")
            name = stream.read(name_size).decode("utf-8")
            if offset < table_end or offset + size > archive_size:
                raise ValueError("WAK entry is outside archive bounds")
            if name == ENTRY:
                chosen = offset, size
        if stream.tell() != table_end or chosen is None:
            raise ValueError("Stone texture missing or invalid WAK table length")
        stream.seek(chosen[0])
        data = stream.read(chosen[1])
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("Selected rock texture is not a PNG")
    destination = ROOT / "game/assets/prototype_noita"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "rock.png").write_bytes(data)
    manifest = {
        "purpose": "Temporary local prototype art, imported at the user's request; Noita / Nolla Games source asset",
        "source_install": str(args.source),
        "source_archive": "data/data.wak",
        "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        "archive_entry": ENTRY,
        "output_sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data),
    }
    (destination / "source.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Imported {ENTRY}: {len(data)} bytes; sha256={manifest['output_sha256']}")


if __name__ == "__main__":
    main()
