#!/usr/bin/env python3
"""
SignalSweep CYD - vendor database generator.

Builds include/vendor_db_data.h from:
  * IEEE registries (MAC prefix -> organisation)
      MA-L (24-bit)  https://standards-oui.ieee.org/oui/oui.csv
      MA-M (28-bit)  https://standards-oui.ieee.org/oui28/mam.csv
      MA-S (36-bit)  https://standards-oui.ieee.org/oui36/oui36.csv
  * Bluetooth SIG Assigned Numbers (YAML, public repository)
      company_identifiers.yaml, service_uuids.yaml, member_uuids.yaml

Run once on a machine with internet access (NOT required for normal builds):
    python tools/gen_vendor_tables.py

If a download fails (URL moved, corporate proxy, ...), download the files by
hand into one folder and point the script at it:
    python tools/gen_vendor_tables.py --local DIR
DIR may contain any of: oui.csv, mam.csv, oui36.csv,
company_identifiers.yaml, service_uuids.yaml, member_uuids.yaml
(Wireshark's "manuf" file is also accepted in place of the three CSVs.)
"""
import argparse, csv, io, os, re, sys, unicodedata, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "firmware-cyd", "include", "vendor_db_data.h")

IEEE = {
    "oui.csv":   ["https://standards-oui.ieee.org/oui/oui.csv"],
    "mam.csv":   ["https://standards-oui.ieee.org/oui28/mam.csv"],
    "oui36.csv": ["https://standards-oui.ieee.org/oui36/oui36.csv"],
}
MANUF = ["https://www.wireshark.org/download/automated/data/manuf"]
SIG_BASE = "https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/"
SIG = {
    "company_identifiers.yaml": [SIG_BASE + "company_identifiers/company_identifiers.yaml"],
    "service_uuids.yaml":       [SIG_BASE + "uuids/service_uuids.yaml"],
    "member_uuids.yaml":        [SIG_BASE + "uuids/member_uuids.yaml"],
}
UA = {"User-Agent": "Mozilla/5.0 (SignalSweep-CYD vendor table generator)"}
MAXLEN = 28

def fetch(name, urls, local):
    if local:
        p = os.path.join(local, name)
        if os.path.exists(p):
            print(f"  local  {name}")
            return open(p, encoding="utf-8", errors="replace").read()
    for u in urls:
        try:
            req = urllib.request.Request(u, headers=UA)
            with urllib.request.urlopen(req, timeout=60) as r:
                data = r.read().decode("utf-8", errors="replace")
            print(f"  ok     {name}  ({len(data)//1024} KB)")
            return data
        except Exception as e:
            print(f"  FAILED {u}: {e}")
    return None

SUFFIX = re.compile(r"[\s,.]+(co\.?,?\s*ltd|co|ltd|limited|inc|incorporated|corp|corporation|"
                    r"llc|l\.l\.c|gmbh|s\.?a|ag|b\.?v|n\.?v|pty|plc|kg|s\.?r\.?l|oy|ab|as|"
                    r"technologies|technology|electronics)\.?$", re.I)

def clean(name):
    name = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode()
    name = " ".join(name.replace('"', "'").replace("\\", "/").split())
    orig = name
    prev = None
    while prev != name:
        prev = name
        name = SUFFIX.sub("", name).strip(" ,.")
    return (name or orig)[:MAXLEN].strip()

def parse_ieee_csv(text):
    out = []
    for row in csv.DictReader(io.StringIO(text)):
        a = (row.get("Assignment") or "").strip().upper()
        org = (row.get("Organization Name") or "").strip()
        if a and org and re.fullmatch(r"[0-9A-F]+", a):
            out.append((int(a, 16), clean(org)))
    return out

def parse_manuf(text):
    l, m, s = [], [], []
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        addr = parts[0].strip()
        name = clean(parts[2] if len(parts) > 2 and parts[2].strip() else parts[1])
        bits = 24
        if "/" in addr:
            addr, b = addr.split("/")
            bits = int(b)
        hexs = re.sub(r"[^0-9A-Fa-f]", "", addr)
        val = int(hexs.ljust(12, "0"), 16)
        if bits == 24: l.append((val >> 24, name))
        elif bits == 28: m.append((val >> 20, name))
        elif bits == 36: s.append((val >> 12, name))
    return l, m, s

def parse_sig_yaml(text, key):
    """Minimal parser for the SIG YAML lists (no PyYAML dependency)."""
    out, cur = [], None
    for line in text.splitlines():
        mk = re.match(rf"\s*-?\s*{key}:\s*(0x[0-9A-Fa-f]+|\d+)", line)
        if mk:
            cur = int(mk.group(1), 0)
            continue
        mn = re.match(r"\s*name:\s*(.+)$", line)
        if mn and cur is not None:
            n = mn.group(1).strip()
            if len(n) >= 2 and n[0] == n[-1] and n[0] in "'\"":
                n = n[1:-1].replace("''", "'")
            out.append((cur, clean(n)))
            cur = None
    return out

class Blob:
    def __init__(self):
        self.parts, self.index, self.size = [], {}, 0
    def add(self, s):
        if s not in self.index:
            self.index[s] = self.size
            self.parts.append(s)
            self.size += len(s) + 1
        return self.index[s]

def dedupe(entries):
    d = {}
    for k, n in entries:
        d.setdefault(k, n)
    return sorted(d.items())

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--local", help="folder with pre-downloaded source files")
    args = ap.parse_args()

    print("IEEE MAC registries:")
    mal = mam = mas = None
    t = fetch("oui.csv", IEEE["oui.csv"], args.local)
    if t: mal = parse_ieee_csv(t)
    t = fetch("mam.csv", IEEE["mam.csv"], args.local)
    if t: mam = parse_ieee_csv(t)
    t = fetch("oui36.csv", IEEE["oui36.csv"], args.local)
    if t: mas = parse_ieee_csv(t)
    if not mal:
        print("IEEE MA-L unavailable, trying Wireshark manuf...")
        t = fetch("manuf", MANUF, args.local)
        if t:
            mal, mam2, mas2 = parse_manuf(t)
            mam = mam or mam2
            mas = mas or mas2
    if not mal:
        sys.exit("ERROR: no MAC vendor source. Use --local DIR (see header).")

    print("Bluetooth SIG assigned numbers:")
    cids = uuids = []
    t = fetch("company_identifiers.yaml", SIG["company_identifiers.yaml"], args.local)
    if t: cids = parse_sig_yaml(t, "value")
    u = []
    for f in ("service_uuids.yaml", "member_uuids.yaml"):
        t = fetch(f, SIG[f], args.local)
        if t: u += parse_sig_yaml(t, "uuid")
    uuids = [(k, n) for k, n in u if k <= 0xFFFF]
    if not cids:
        print("WARNING: no company identifiers; BLE company names will be limited")

    mal, mam, mas = dedupe(mal), dedupe(mam or []), dedupe(mas or [])
    cids, uuids = dedupe(cids), dedupe(uuids)

    blob = Blob()
    def table(entries):
        return [(k, blob.add(n)) for k, n in entries]
    t_l, t_m, t_s = table(mal), table(mam), table(mas)
    t_c, t_u = table(cids), table(uuids)

    def arr(ctype, name, vals, per=8):
        lines = [f"static const {ctype} {name}[] = {{"]
        for i in range(0, len(vals), per):
            lines.append("  " + ",".join(vals[i:i+per]) + ",")
        lines.append("};")
        return "\n".join(lines)

    with open(OUT, "w", encoding="ascii") as f:
        f.write("// GENERATED by tools/gen_vendor_tables.py - do not edit.\n")
        f.write("// Sources: IEEE RA registries (MA-L/MA-M/MA-S), Bluetooth SIG Assigned Numbers.\n")
        f.write("#pragma once\n#include <stdint.h>\n#define VENDOR_DB_FULL 1\n\n")
        f.write(f"#define OUI24_COUNT {len(t_l)}\n#define OUI28_COUNT {len(t_m)}\n#define OUI36_COUNT {len(t_s)}\n")
        f.write(f"#define CID_COUNT {len(t_c)}\n#define UUID16_COUNT {len(t_u)}\n\n")
        f.write(arr("uint32_t", "OUI24_KEY", [f"0x{k:06X}" for k, _ in t_l]) + "\n")
        f.write(arr("uint32_t", "OUI24_NAME", [str(o) for _, o in t_l]) + "\n")
        f.write(arr("uint32_t", "OUI28_KEY", [f"0x{k:07X}" for k, _ in t_m] or ["0"]) + "\n")
        f.write(arr("uint32_t", "OUI28_NAME", [str(o) for _, o in t_m] or ["0"]) + "\n")
        f.write(arr("uint64_t", "OUI36_KEY", [f"0x{k:09X}ULL" for k, _ in t_s] or ["0"]) + "\n")
        f.write(arr("uint32_t", "OUI36_NAME", [str(o) for _, o in t_s] or ["0"]) + "\n")
        f.write(arr("uint16_t", "CID_KEY", [f"0x{k:04X}" for k, _ in t_c] or ["0"]) + "\n")
        f.write(arr("uint32_t", "CID_NAME", [str(o) for _, o in t_c] or ["0"]) + "\n")
        f.write(arr("uint16_t", "UUID16_KEY", [f"0x{k:04X}" for k, _ in t_u] or ["0"]) + "\n")
        f.write(arr("uint32_t", "UUID16_NAME", [str(o) for _, o in t_u] or ["0"]) + "\n")
        f.write("static const char VENDOR_NAMES[] =\n")
        for s in blob.parts:
            f.write(f'  "{s}\\0"\n')
        f.write("  ;\n")

    approx = blob.size + 8*len(t_l) + 8*len(t_m) + 12*len(t_s) + 6*len(t_c) + 6*len(t_u)
    print(f"\nWrote {os.path.normpath(OUT)}")
    print(f"  MA-L {len(t_l)}  MA-M {len(t_m)}  MA-S {len(t_s)}  "
          f"company IDs {len(t_c)}  16-bit UUIDs {len(t_u)}")
    print(f"  approx flash use: {approx//1024} KB")

if __name__ == "__main__":
    main()
