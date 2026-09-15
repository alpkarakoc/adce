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

# THE INTERPRETER IS A HARD DEPENDENCY AND ITS ABSENCE IS A FAILURE, NOT A SKIP.
#
# Without this guard the script still fails -- `set -e` carries bash's exit 127
# out -- but the only thing printed is
#
#     ./scripts/check-internal-use.sh: line 48: python3: command not found
#
# which names a line of shell and not a gate. This repository already ranks an
# unattributable red as a defect in its own right: it is why the stale-posture
# failure prints `aged=N` beside torn and future instead of a bare assertion
# number. A check that goes red without saying what it was checking invites the
# reading that the check is broken, and the next step after that reading is to
# skip it.
#
# It must never become a SKIP. A gate that passes because an interpreter is
# missing is the class this project has ruled worse than no gate at all -- the
# green tick would be indistinguishable from the green tick of a check that ran.
# So the absent interpreter exits 1 with a message, and there is deliberately no
# environment variable that turns this into a pass.
if ! command -v python3 >/dev/null 2>&1; then
    cat >&2 <<'EOM'
FAIL: internal-use check did NOT RUN -- python3 is not on PATH.

This is a FAILURE, not a skip, and the distinction is the point. The check
asks whether every function in include/ has a shipping call site or says why
not. With no interpreter it asked nothing, so it has no answer to report, and
reporting a pass would make "the check ran and was satisfied" and "the check
never executed" look identical from the outside.

WHERE THIS IS EXPECTED TO RUN: the internal-use job in
.github/workflows/verify.yml, on ubuntu-24.04, which ships python3. No local
gate invokes this script -- neither scripts/verify.sh nor
scripts/verify-linux-gcc.sh nor anything under scripts/hooks/ -- so a
development machine without python3 does not hit this path unless the script
is run by hand.

TO FIX: install python3, or run the check on a host that has it. There is no
flag that makes this pass.
EOM
    exit 1
fi

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
    declaration further up cannot be borrowed by this one.

    Returns None when there is no marker, "" when there is a marker but no
    reason, and the reason otherwise. The three are distinct because they need
    different messages: a missing marker is an unanswered question, an empty one
    is the token-to-paste this check exists to refuse.

    THE REASON MUST BE ON THE MARKER'S OWN LINE. The predicate was
    `MARK \\s*:\\s*(\\S[^\\n]*)` over the joined block, and `\\s*` after the colon
    matches NEWLINES -- so the capture walked to the next non-space character
    anywhere below the marker and returned it as the reason. A bare marker sitting
    above the block's own terminator was accepted with `*/` as its reason; above
    an unrelated sentence, that sentence became the reason. The check verified
    that a colon was present, not that a reason existed, while its own guidance
    said a bare marker would make this a token to paste."""
    lines = h.read_text().split("\n")
    block, i = [], line - 1
    while i >= 0:
        t = lines[i].strip()
        if t == "" or t.startswith("*") or t.startswith("/*") or t.startswith("//") \
           or t.endswith("*/"):
            block.append(lines[i]); i -= 1
        else:
            break
    for text in reversed(block):
        m = re.search(MARK + r'[ \t]*:(.*)$', text)
        if m is None:
            continue
        reason = re.sub(r'\*/\s*$', '', m.group(1)).strip()
        # Comment punctuation is not a reason. This is what catches `*/`, `*`,
        # and a line that trails off into the block's own syntax.
        if re.fullmatch(r'[*/\s]*', reason):
            return ""
        return reason
    return None

bad, empty, annotated, live = [], [], [], 0
for name, (h, line) in sorted(found.items()):
    if calls(hdrs, name) + calls(srcs, name) > 0:
        live += 1
        continue
    reason = annotation(h, line)
    if reason:
        annotated.append((name, h.name, reason))
    elif reason == "":
        empty.append((name, h.name, line + 1))
    else:
        bad.append((name, h.name, line + 1))

print(f"include/ functions: {len(found)}  |  with a shipping caller: {live}"
      f"  |  annotated: {len(annotated)}  |  UNEXPLAINED: {len(bad)}"
      f"  |  MARKER WITHOUT REASON: {len(empty)}")
if annotated:
    print(f"\n{MARK} (declared, not defects):")
    for n, f, r in annotated:
        print(f"  {n:26} {f:18} {r}")
if empty:
    print(f"\nFAIL: {len(empty)} function(s) carry {MARK} with NO REASON:\n",
          file=sys.stderr)
    for n, f, ln in empty:
        print(f"  {f}:{ln}  {n}", file=sys.stderr)
    print(f"""
The marker is present and the reason is not. A bare marker is a token to paste:
it clears this check without answering the question the check asks, which is
WHY a published function has no caller inside the library.

The reason must be on the MARKER'S OWN LINE and must be more than comment
syntax -- `*/`, `*` and a blank remainder are all rejected. Write it as

    /* {MARK}: called by consumer ingress
     * sites, never from inside the library -- see the recipe in README.md. */

Continuation lines are fine; the first line is what must carry the reason.""",
          file=sys.stderr)

if not bad and not empty:
    print("\nOK: every function in include/ either has a shipping caller or says why not.")
    sys.exit(0)
if not bad:
    sys.exit(1)

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
