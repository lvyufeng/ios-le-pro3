#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""zl1-log.py - turn a probe screen report into a checkable record.

The probe's only output is text on the phone's screen, so the report comes
back either keyed in by hand or OCR'd from a photo. Both are noisy. This
tool normalises what it gets, checks the values it understands against the
expectations in hardware/zl1-facts.md, and writes a reviewable report so the
next person (or the next agent) can see exactly what was and was not
confirmed on hardware.

Usage:
    tools/zl1-log.py photo.txt            # parse a pasted/OCR'd capture
    tools/zl1-log.py photo.txt --save     # also write hardware/observations/<date>.md

The parser is deliberately forgiving: it looks for "key: value" lines,
ignores everything else, and reports keys it did not expect instead of
failing. A malformed line is a reason to re-read the photo, not to lose the
whole capture.
"""
import argparse
import datetime as dt
import re
import sys
from pathlib import Path

LINE_RE = re.compile(r"^\s*([A-Za-z0-9_.\- ]+?)\s*:\s*(0x[0-9a-fA-F]+|\d+)\s*$")
ANY_RE = re.compile(r"^\s*([A-Za-z0-9_.\- ]+?)\s*:\s*(.+?)\s*$")

# Expected values, with the source of the expectation. "must" entries are
# things the probe's own design depends on; "note" entries are interesting
# but not fatal.
EXPECT = {
    "current_el":  {"want": 0x4, "must": True,
                    "why": "probe assumes EL1 (currentel encoding 4 = EL1)"},
    "build_tag":   {"want": 0x3131017a or None, "must": False,
                    "why": "identifies the image; any value is fine"},
    "cntfrq":      {"want": 19200000, "must": False,
                    "why": "19.2 MHz expected; 0 means the bootloader did not set it"},
}


def parse(text: str) -> dict:
    vals, textvals, unknown = {}, {}, []
    for line in text.splitlines():
        m = LINE_RE.match(line)
        if m:
            key = m.group(1).strip()
            raw = m.group(2)
            vals[key] = int(raw, 0)
            continue
        m = ANY_RE.match(line)
        if m:
            key = m.group(1).strip()
            textvals[key] = m.group(2).strip()
            continue
        if line.strip():
            unknown.append(line.rstrip())
    return {"vals": vals, "text": textvals, "other": unknown}


def check(parsed: dict) -> list:
    findings = []
    for key, exp in EXPECT.items():
        if key not in parsed["vals"]:
            if exp.get("must"):
                findings.append(("MISSING", key, "required by the probe design"))
            continue
        got = parsed["vals"][key]
        if exp["want"] is not None and got != exp["want"]:
            sev = "FAIL" if exp.get("must") else "WARN"
            findings.append((sev, key, f"{got:#x} != expected {exp['want']:#x}"
                                       f" ({exp['why']})"))
        else:
            findings.append(("ok", key, f"{got:#x}"))

    # Interpretation of the values that decide the next milestone.
    v = parsed["vals"]
    if "LSE_atomics" in parsed["text"]:
        lse = parsed["text"]["LSE_atomics"]
        if "NO" in lse.upper():
            findings.append(("NOTE", "LSE_atomics",
                             "XNU needs the ARMv8.0 LSE patch (see S8 port 0005/0035)"))
        else:
            findings.append(("NOTE", "LSE_atomics",
                             "native LSE: the sweet/shim code path may work unpatched"))
    if "grain16K" in parsed["text"]:
        g = parsed["text"]["grain16K"]
        findings.append(("NOTE", "grain16K",
                         "4 KiB only, as expected for MSM8996"
                         if "NO" in g.upper() else "16 KiB available - unexpected"))

    if "hcs" in v:
        hcs = v["hcs"]
        ready = (hcs & 0x3) == 0x3
        findings.append(("ok" if ready else "WARN", "hcs",
                         "DP+UTRLRDY set: a polled UFS driver can start"
                         if ready else "host not ready; UFS init would be needed"))

    if "mmu4k_rc" in v:
        rc = v["mmu4k_rc"]
        signed = rc - (1 << 64) if rc >= (1 << 63) else rc
        findings.append(("ok" if signed == 0 else "WARN", "mmu4k_rc",
                         "4 KiB identity map works on this SoC" if signed == 0
                         else f"4 KiB map rejected (rc={signed})"))

    if "fault_count" in v and v["fault_count"]:
        findings.append(("NOTE", "fault_count",
                         f"{v['fault_count']} reads faulted; far="
                         f"{parsed['vals'].get('fault_last_far', 0):#x} "
                         "- check those address-map guesses"))
    return findings


def render(parsed: dict, findings: list, source: Path) -> str:
    out = [f"# probe capture from {source.name}",
           f"generated {dt.datetime.now().isoformat(timespec='seconds')}", "",
           "## findings", ""]
    for sev, key, msg in findings:
        out.append(f"- `{sev:7}` {key}: {msg}")
    out += ["", "## raw values", ""]
    for k in sorted(parsed["vals"]):
        out.append(f"- {k}: {parsed['vals'][k]:#x}")
    if parsed["text"]:
        out += ["", "## text fields", ""]
        for k in sorted(parsed["text"]):
            out.append(f"- {k}: {parsed['text'][k]}")
    if parsed["other"]:
        out += ["", "## unrecognised lines", ""]
        out += [f"- {line}" for line in parsed["other"]]
    return "\n".join(out) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", type=Path)
    ap.add_argument("--save", action="store_true",
                    help="write hardware/observations/<date>-<name>.md")
    args = ap.parse_args()

    parsed = parse(args.capture.read_text(errors="replace"))
    findings = check(parsed)
    report = render(parsed, findings, args.capture)
    print(report)

    if args.save:
        root = Path(__file__).resolve().parent.parent
        dest_dir = root / "hardware" / "observations"
        dest_dir.mkdir(parents=True, exist_ok=True)
        stamp = dt.date.today().isoformat()
        dest = dest_dir / f"{stamp}-{args.capture.stem}.md"
        dest.write_text(report)
        print(f"wrote {dest}", file=sys.stderr)

    if any(sev == "FAIL" for sev, _, _ in findings):
        sys.exit(1)


if __name__ == "__main__":
    main()