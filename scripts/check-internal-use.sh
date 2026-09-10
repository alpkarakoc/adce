#!/usr/bin/env bash
# Every function in include/ must have a caller in shipping code, or say why not.
#
# WHAT IT CHECKS. For each function defined or declared in include/*.h, count
# call sites in include/*.h and src/*.c with comments and string literals
# stripped and the definition itself excluded. Zero is a failure unless the
# declaration carries
#
#     ADCE_PUBLIC_NO_INTERNAL_USER: <reason>
#
# in the comment block above it.
#
# WHY. adce_epoch_is_stale sat at zero shipping call sites and fourteen test
# call sites while CLAUDE.md and README.md both described it as the mechanism
# producing the fail-closed handling of a future timestamp. Every word of that
# description was true OF THE FUNCTION; what had changed was which path the code
# takes. Neither a reader nor a grep could detect it -- the name is present in
# shipping code six times, all in comments -- and it was found only by mutating
# the wrap and watching the case that named it stay green.
#
# THE TEST SUITE IS WHAT CONCEALS THE DEATH, and that is the mechanism this
# closes rather than the one instance. A function with fourteen call sites reads
# as maximally live from every angle except the one that matters. The suite
# keeps it compiling, keeps it covered, and keeps it looking exercised long
# after nothing ships through it. On this repository today adce_epoch_publish
# and adce_epoch_read each sit at exactly ONE shipping call site against
# nineteen and seventeen test calls: one refactor from the same state, with
# nothing in the gate that would turn red.
#
# WHY THIS IS MECHANISABLE WHERE THE PRINTED-UNIVERSAL RULE WAS NOT. That rule
# asked whether the quantity behind a sentence is asserted anywhere, which is a
# fact about the TEST SUITE reached from a fact about ENGLISH -- the predicate
# lived outside the text it had to judge, and no regex could cross the gap. This
# predicate is a fact about the code, evaluated against the code: does a call
# edge exist. It is decidable, it is exact, and its answer does not depend on how
# any sentence is phrased. The earlier negative result is about that specific
# predicate, not about gating in general.
#
# STEADY-STATE COST, which is the number that matters. It fires once per
# function, ever: annotate it or wire it up, and it is quiet until the answer
# changes. Contrast the boundary note, which fires on 71% of pull requests
# forever. A one-time burst is not the failure mode this project condemns; a
# recurring prompt answered by reflex is.
set -euo pipefail

cd "${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

python3 - "$@" <<'PY'
import re, sys, pathlib

MARK = "ADCE_PUBLIC_NO_INTERNAL_USER"

def strip(s):
    """Comments and string literals out, so a mention is never read as a call."""
    s = re.sub(r'"(\\.|[^"\\])*"', '""', s)
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', s)

root = pathlib.Path(".")
hdrs = sorted(root.glob("include/*.h"))
srcs = sorted(root.glob("src/*.c"))
if not hdrs:
    print("FAIL: no headers found in include/", file=sys.stderr)
    sys.exit(1)

DEF  = re.compile(r'^static\s+inline\s+[A-Za-z_][\w \t\*]*?\**\s*(\badce_[A-Za-z0-9_]+)\s*\(', re.M)
DECL = re.compile(r'^[A-Za-z_][\w \t\*]*?\**\s*(\badce_[A-Za-z0-9_]+)\s*\([^;{]*\);', re.M)

# name -> (header, 0-based line of the declaration in the ORIGINAL text)
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

bad, annotated, live = [], [], 0
for name, (h, line) in sorted(found.items()):
    if calls(hdrs, name) + calls(srcs, name) > 0:
        live += 1
        continue
    reason = annotation(h, line)
    if reason:
        annotated.append((name, h.name, reason))
    else:
        bad.append((name, h.name, line + 1))

print(f"include/ functions: {len(found)}  |  with a shipping caller: {live}"
      f"  |  annotated: {len(annotated)}  |  UNEXPLAINED: {len(bad)}")
if annotated:
    print(f"\n{MARK} (declared, not defects):")
    for n, f, r in annotated:
        print(f"  {n:26} {f:18} {r}")
if not bad:
    print("\nOK: every function in include/ either has a shipping caller or says why not.")
    sys.exit(0)

print(f"\nFAIL: {len(bad)} function(s) in include/ have ZERO call sites in", file=sys.stderr)
print("      include/ or src/, and carry no annotation:\n", file=sys.stderr)
for n, f, ln in bad:
    print(f"  {f}:{ln}  {n}", file=sys.stderr)
print(f"""
A test suite conceals this. A function can carry dozens of test call sites and
still have nothing shipping through it, which is exactly how adce_epoch_is_stale
kept two documentation paragraphs pointing at a mechanism the gate had stopped
using. Test calls are deliberately NOT counted here.

Two ways past this, and both are correct answers:

  1. Wire it up, or delete it. Deleting from a public header is an API decision:
     propose it and wait, per the working agreement.

  2. If it is public API with no internal user by design, say so above the
     declaration and give the reason:

         /* {MARK}: called by consumer ingress
          * sites, never from inside the library -- see the recipe in README.md. */

     The reason is required. A bare marker would make this a token to paste.""",
      file=sys.stderr)
sys.exit(1)
PY
