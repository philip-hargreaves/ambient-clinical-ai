"""Export shortlist candidates to OpenVINO IR with provenance.

  python export.py bge-base                 fp16 and int8
  python export.py bge-base --precision int8
  python export.py --role embedder          every enabled embedder
  python export.py --all
"""

import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

from common import CANDIDATES, candidate, candidate_dir, load_shortlist, log, write_json


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(hf_id: str) -> tuple[Path, str]:
    # Download in-process (IPv4 fix); local_dir needs no symlinks on exFAT
    from huggingface_hub import HfApi, snapshot_download
    local = CANDIDATES / ".src" / hf_id.replace("/", "--")
    snapshot_download(hf_id, local_dir=str(local),
                      allow_patterns=["*.json", "*.txt", "*.model", "*.safetensors", "*.py"])
    return local, HfApi().model_info(hf_id).sha


def export_one(entry: dict, precision: str) -> Path:
    out = candidate_dir(entry["id"], precision)
    if (out / "openvino_model.xml").exists():
        log(f"{out.name}: exists, skipped")
        return out
    snapshot, sha = download(entry["hf"])
    optimum_cli = Path(sys.executable).with_name("optimum-cli.exe")
    cmd = [str(optimum_cli), "export", "openvino", "--model", str(snapshot), "--task", entry["export_task"],
           "--weight-format", precision, str(out)]
    log(" ".join(cmd))
    subprocess.run(cmd, check=True)
    files = sorted(p for p in out.rglob("*") if p.is_file())
    write_json(out / "provenance.json", {
        "id": entry["id"],
        "hf": entry["hf"],
        "revision": sha,
        "precision": precision,
        "task": entry["export_task"],
        "command": " ".join(cmd),
        "exported_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "files": {p.relative_to(out).as_posix(): {"sha256": sha256(p), "bytes": p.stat().st_size} for p in files},
    })
    config = json.loads((out / "config.json").read_text(encoding="utf-8"))
    log(f"{out.name}: architectures={config.get('architectures')} model_type={config.get('model_type')}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ids", nargs="*")
    ap.add_argument("--role", choices=["embedder", "reranker", "baseline"])
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--precision", choices=["fp16", "int8", "both"], default="both")
    ap.add_argument("--include-disabled", action="store_true")
    args = ap.parse_args()

    entries = load_shortlist()
    if args.ids:
        entries = [candidate(i) for i in args.ids]
    elif args.role:
        entries = [e for e in entries if e["role"] == args.role]
    elif not args.all:
        ap.error("give ids, --role or --all")
    entries = [e for e in entries if e.get("enabled", True) or args.include_disabled or args.ids]
    precisions = ["fp16", "int8"] if args.precision == "both" else [args.precision]
    for entry in entries:
        for precision in precisions:
            export_one(entry, precision)


if __name__ == "__main__":
    main()
