---
description: Execute one traceable implementation step under the architecture gate
argument-hint: <what to build, or "next" to derive it>
allowed-tools: Read, Grep, Glob, Edit, Write, Bash(cc:*), Bash(git:*), Bash(uname:*), Bash(./scripts/verify.sh)
---

<task>
$ARGUMENTS
</task>

Follow these phases in order. Do not skip ahead; each phase's output is the next one's input.

<phase name="discovery">
Read before asserting. Establish ground truth from the repository, not from assumption:
- `git status --short` and `git log --oneline -10`
- Every file the task touches or depends on, read in full
- `./scripts/verify.sh` — record the current exit code as the baseline

Report contradictions between the task and what the repository actually contains. If the task
presumes a symbol, column, port, or file that does not exist, say so and stop.
</phase>

<phase name="plan">
Before writing code, output:
- FILES: exact paths to create or modify, each with a one-line reason
- TRACE: for each file, the stated requirement or committed decision it derives from. A file
  that traces to nothing is scope creep — cut it.
- SCOPE FENCE: adjacent things you are deliberately NOT building, and why they are not yet
  traceable
- RISKS: boundary conditions, concurrency, failure modes, and anything that could break an
  existing test
- VERIFICATION: the exact command sequence that will prove the step correct

If the task violates a layer rule or a committed decision in CLAUDE.md, halt here. State the
risk, propose the corrected design, and wait. Do not implement a compromise silently.
</phase>

<phase name="execute">
Write the code. Tests land in the same commit as the behavior they cover.

Hard constraints:
- No `// TODO`, no `/* logic here */`, no stub returns, no `any`, no `as` assertions used to
  silence the compiler, no `@ts-ignore` or `@ts-expect-error` without an adjacent comment
  explaining why the type system is genuinely wrong.
- No new runtime dependency without naming it and saying what it replaces.
- No edits outside the FILES list. If the plan was wrong, return to the plan phase and say so.
- Comment the non-obvious *why*. Do not narrate the *what*.
</phase>

<phase name="verify">
Run `./scripts/verify.sh`. Paste the real terminal output, including the exit code. Do not paraphrase
it and do not claim success you did not observe. If it fails, fix and rerun — a red gate is
not a finding to report, it is work that is not finished.
</phase>

<output_contract>
Close with exactly these four sections, no preamble and no summary of what you just did:

1. CHANGED — file paths with a one-line description each
2. TRACE — each change mapped to its originating requirement or decision
3. VERIFICATION — the `./scripts/verify.sh` result, verbatim
4. NEXT — the single most traceable follow-on step, and what currently blocks it
</output_contract>