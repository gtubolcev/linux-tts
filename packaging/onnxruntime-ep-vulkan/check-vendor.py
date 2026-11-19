"""check-vendor.py Cargo.lock VENDOR: every crates.io package of Cargo.lock
is in VENDOR with the same checksum, every file of a vendored crate matches
its .cargo-checksum.json, and VENDOR has nothing else."""
import hashlib, json, os, sys, tomllib

lock, vendor = sys.argv[1], sys.argv[2]
with open(lock, "rb") as f:
    pkgs = [p for p in tomllib.load(f)["package"]
            if p.get("source", "").startswith("registry+")]
want = {}
for p in pkgs:
    want.setdefault(p["name"], []).append((p["version"], p["checksum"]))

seen = set()
for d in sorted(os.listdir(vendor)):
    top = os.path.join(vendor, d)
    with open(os.path.join(top, ".cargo-checksum.json")) as f:
        ck = json.load(f)
    with open(os.path.join(top, "Cargo.toml"), "rb") as f:
        meta = tomllib.load(f)["package"]
    key = (meta["name"], meta["version"], ck["package"])
    if (meta["version"], ck["package"]) not in want.get(meta["name"], []):
        sys.exit(f"error: {d}: {key} is not in Cargo.lock")
    for rel, h in ck["files"].items():
        with open(os.path.join(top, rel), "rb") as f:
            if hashlib.sha256(f.read()).hexdigest() != h:
                sys.exit(f"error: {d}/{rel}: checksum mismatch")
    for root, _, files in os.walk(top):
        for fn in files:
            rel = os.path.relpath(os.path.join(root, fn), top)
            if rel != ".cargo-checksum.json" and rel not in ck["files"]:
                sys.exit(f"error: {d}/{rel}: not in .cargo-checksum.json")
    seen.add(key)
missing = [(p["name"], p["version"]) for p in pkgs
           if (p["name"], p["version"], p["checksum"]) not in seen]
if missing:
    sys.exit(f"error: not vendored: {missing}")
print(f"vendor: {len(seen)} crates match Cargo.lock")
