"""Fetch pinned standard Godot and only Windows x64 templates (stdlib only)."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "tools/versions.json").read_text(encoding="utf-8"))
VERSION = LOCK["godot"]
DEST = ROOT / "tools/_cache" / f"godot-{VERSION}"
BASE = f"https://github.com/godotengine/godot-builds/releases/download/{VERSION}/"
HASH_FILE = ROOT / "tools/artifact-hashes.json"


class RemoteArchive(io.RawIOBase):
    """Seekable HTTP ZIP; exact ranges plus zipfile CRC validation."""
    def __init__(self, url):
        self.position = 0
        with urllib.request.urlopen(urllib.request.Request(url, headers={"Range": "bytes=0-0"}), timeout=45) as response:
            if response.status != 206:
                raise RuntimeError("Template server must support HTTP ranges.")
            self.url = response.url
            self.length = int(response.headers["Content-Range"].split("/")[-1])

    def seekable(self):
        return True

    def seek(self, offset, whence=0):
        self.position = offset if whence == 0 else self.position + offset if whence == 1 else self.length + offset
        return self.position

    def tell(self):
        return self.position

    def read(self, size=-1):
        size = min(self.length - self.position, size if size >= 0 else self.length)
        if size <= 0:
            return b""
        start = self.position
        request = urllib.request.Request(self.url, headers={"Range": f"bytes={start}-{start + size - 1}"})
        with urllib.request.urlopen(request, timeout=45) as response:
            if response.status != 206 or not response.headers.get("Content-Range", "").startswith(f"bytes {start}-"):
                raise RuntimeError("Unexpected template HTTP range response.")
            data = response.read(size)
        if len(data) != size:
            raise RuntimeError("Incomplete template download.")
        self.position += len(data)
        return data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--record-hashes", action="store_true", help="Explicit maintainer operation for initial version pinning")
    args = parser.parse_args()
    expected = json.loads(HASH_FILE.read_text()) if HASH_FILE.exists() else {}
    if not expected and not args.record_hashes:
        raise RuntimeError("Missing committed artifact-hashes.json; restore it from source control.")
    DEST.mkdir(parents=True, exist_ok=True)
    if expected and not args.record_hashes and all(
        (DEST / name).is_file() and hashlib.sha256((DEST / name).read_bytes()).hexdigest() == digest
        for name, digest in expected["sha256"].items()
    ):
        print(f"Godot {VERSION}: all cached artifact hashes verified.", flush=True)
        return

    engine_name = f"Godot_v{VERSION}_win64.exe.zip"
    checksum_path = DEST / "SHA512-SUMS.txt"
    urllib.request.urlretrieve(BASE + "SHA512-SUMS.txt", checksum_path)
    sums = {}
    for line in checksum_path.read_text().splitlines():
        digest, name = line.split(maxsplit=1)
        sums[name.lstrip("*")] = digest
    engine_zip = DEST / engine_name
    if not engine_zip.exists() or hashlib.sha512(engine_zip.read_bytes()).hexdigest() != sums[engine_name]:
        print("Downloading standard Godot editor...", flush=True)
        urllib.request.urlretrieve(BASE + engine_name, engine_zip)
    if hashlib.sha512(engine_zip.read_bytes()).hexdigest() != sums[engine_name]:
        raise RuntimeError("Official editor SHA-512 mismatch.")
    recorded = {}

    def write_verified(name, data):
        digest = hashlib.sha256(data).hexdigest()
        if not args.record_hashes and expected.get("sha256", {}).get(name) != digest:
            raise RuntimeError(f"Pinned SHA-256 mismatch for {name}.")
        (DEST / name).write_bytes(data)
        recorded[name] = digest
        print(f"Verified {name}: {len(data)} bytes, sha256={digest}", flush=True)

    with zipfile.ZipFile(engine_zip) as archive:
        for info in archive.infolist():
            if info.filename.endswith(".exe"):
                write_verified(Path(info.filename).name, archive.read(info))
    template_name = f"Godot_v{VERSION}_export_templates.tpz"
    print("Fetching Windows x64 template entries from official ZIP...", flush=True)
    with zipfile.ZipFile(RemoteArchive(BASE + template_name)) as archive:
        for name in ("windows_debug_x86_64.exe", "windows_release_x86_64.exe", "version.txt"):
            write_verified(name, archive.read("templates/" + name))
    if (DEST / "version.txt").read_text().strip() != VERSION.replace("-", "."):
        raise RuntimeError("Template version mismatch.")
    for name in ("COPYRIGHT.txt", "LICENSE.txt"):
        with urllib.request.urlopen(f"https://raw.githubusercontent.com/godotengine/godot/{VERSION}/{name}", timeout=45) as response:
            write_verified(name, response.read())
    if args.record_hashes:
        HASH_FILE.write_text(json.dumps({"version": VERSION, "source": BASE, "editor_zip_sha512": sums[engine_name],
                                         "template_zip_sha512": sums[template_name], "sha256": recorded}, indent=2) + "\n")


if __name__ == "__main__":
    main()
