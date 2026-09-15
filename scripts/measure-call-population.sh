#!/usr/bin/env bash
# The live / test-only / dead population of include/, measured.
#
# THIS IS A MEASUREMENT AND NOT A GATE. Nothing invokes it: not
# scripts/verify.sh, not scripts/verify-linux-gcc.sh, not any job in
# .github/workflows/verify.yml, not anything under scripts/githooks/. It
# asserts nothing and it cannot fail a build. It exists so the numbers in
# CLAUDE.md's population entry can be RE-DERIVED by the next reader instead of
# inherited, which is this project's standing complaint about figures.
#
# WHY IT IS A SEPARATE FILE FROM check-internal-use.sh. That script is a LIVE
# GATE with a required-adjacent role and a pre-registered five-pull-request
# answer window running against it. Adding reporting to it -- even reporting
# that changes no verdict -- is a gate change, and a gate change lands in its
# own pull request with its own mutation proofs. This measurement must not
# perturb the instrument whose blind spot it is measuring.
#
# THE PARSER IS THE GATE'S OWN, COPIED VERBATIM rather than re-implemented.
# strip(), DEF, DECL and calls() below are byte-identical to
# scripts/check-internal-use.sh's. That is the whole point: a hand-written grep
# counts declarations and definitions as calls, which is exactly how the crude
# sample counts that motivated this measurement went wrong. The test-call count
# has to be derived the same way the shipping count already is, or the two
# columns are not comparable and the three-way split means nothing.
#
# If check-internal-use.sh's parser changes, this copy goes stale. That is a
# real cost of copying and it is accepted deliberately: the alternative is
# coupling a measurement to a gate, and a stale measurement is visible the
# moment someone re-runs it while a perturbed gate is not.
set -euo pipefail

cd "${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if ! command -v python3 >/dev/null 2>&1; then
    echo "FAIL: measure-call-population needs python3 and it is not on PATH." >&2
    echo "      This is a measurement, not a gate -- nothing is blocked by" >&2
    echo "      this failure, but no numbers were produced either." >&2
    exit 1
fi

python3 - "$@" <<'PY'
import re, sys, pathlib

MARK = "ADCE_PUBLIC_NO_INTERNAL_USER"

# ---- verbatim from scripts/check-internal-use.sh -------------------------
def strip(s):
    """Comments and string literals out, so a mention is never read as a call."""
    s = re.sub(r'"(\\.|[^"\\])*"', '""', s)
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', s)

root = pathlib.Path(".")
hdrs = sorted(root.glob("include/*.h"))
srcs = sorted(root.glob("src/*.c"))
tsts = sorted(root.glob("test/*.c"))

DEF  = re.compile(r'^static\s+inline\s+[A-Za-z_][\w \t\*]*?\**\s*(\badce_[A-Za-z0-9_]+)\s*\(', re.M)
DECL = re.compile(r'^[A-Za-z_][\w \t\*]*?\**\s*(\badce_[A-Za-z0-9_]+)\s*\([^;{]*\);', re.M)

found = {}
for h in hdrs:
    raw = h.read_text()
    code = strip(raw)
    lines = raw.split("\n")
    for rx in (DEF, DECL):
        for m in rx.finditer(code):
            name = m.group(1)
            if name in found:
                continue
            for i, l in enumerate(lines):
                if re.search(r'\b' + name + r'\s*\(', l):
                    found[name] = (h, i)
                    break

def calls(paths, name):
    n = 0
    for p in paths:
        c = strip(p.read_text())
        k  = len(re.findall(r'\b' + name + r'\s*\(', c))
        k -= len(re.findall(r'^[A-Za-z_][\w \t\*]*?\**\s*\b' + name + r'\s*\([^;]*$', c, re.M))
        k -= len(re.findall(r'^[A-Za-z_][\w \t\*]*?\**\s*\b' + name + r'\s*\([^;{]*\);', c, re.M))
        n += max(k, 0)
    return n

def annotation(h, line):
    """The MARK and its reason, if the comment block IMMEDIATELY above has one.

    Walks upward from the declaration over blank and comment lines only, and
    stops at the first line that is neither. So a marker attached to some other
    declaration further up cannot be borrowed by this one."""
    lines = h.read_text().split("\n")
    block, i = [], line - 1
    while i >= 0:
        t = lines[i].strip()
        if t == "" or t.startswith("*") or t.startswith("/*") or t.startswith("//") \
           or t.endswith("*/"):
            block.append(lines[i]); i -= 1
        else:
            break
    m = re.search(MARK + r'\s*:\s*(\S[^\n]*)', "\n".join(reversed(block)))
    return m.group(1).strip() if m else None
# ---- end verbatim --------------------------------------------------------

rows = []
for name, (h, line) in sorted(found.items()):
    ship = calls(hdrs, name) + calls(srcs, name)   # exactly the gate's predicate
    test = calls(tsts, name)                       # the same parser, test/ instead
    ann  = annotation(h, line)
    cls  = "live" if ship > 0 else ("test-only" if test > 0 else "dead")
    rows.append((name, h.name, ship, test, cls, ann))

n = len(rows)
live = [r for r in rows if r[4] == "live"]
tonly = [r for r in rows if r[4] == "test-only"]
dead = [r for r in rows if r[4] == "dead"]
ann = [r for r in rows if r[5] is not None]

print(f"include/ functions: {n}")
print(f"  live       (shipping > 0)              : {len(live)}")
print(f"  test-only  (shipping == 0, test > 0)   : {len(tonly)}")
print(f"  dead       (both zero)                 : {len(dead)}")
print(f"  annotated  ({MARK}) : {len(ann)}")
print()
print(f"{'function':28} {'header':18} {'ship':>5} {'test':>5}  {'class':10} annotated")
print("-" * 86)
for name, hf, ship, test, cls, a in sorted(rows, key=lambda r: (r[4], -r[3], r[0])):
    print(f"{name:28} {hf:18} {ship:>5} {test:>5}  {cls:10} {'yes' if a else 'no'}")

print()
print("THE 14 ANNOTATED FUNCTIONS, BY CLASS")
for c in ("live", "test-only", "dead"):
    sel = [r for r in ann if r[4] == c]
    print(f"  {c:10} {len(sel):>2}: " + (", ".join(r[0] for r in sel) if sel else "-"))

print()
print("THE TWO UNANNOTATED ZERO-SHIPPING FUNCTIONS (the gate's current red)")
for r in rows:
    if r[2] == 0 and r[5] is None:
        print(f"  {r[0]:28} ship={r[2]} test={r[3]}  -> {r[4]}")

print()
print("LIVE FUNCTIONS WITH EXACTLY ONE SHIPPING CALLER")
one = [r for r in live if r[2] == 1]
print(f"  {len(one)} of {len(live)} live functions")
for r in sorted(one, key=lambda r: -r[3]):
    print(f"  {r[0]:28} ship=1 test={r[3]:>3}")
PY
