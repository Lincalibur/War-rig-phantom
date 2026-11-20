---
name: catch-up
description: Catch up on this CyberDeck project at the start of a session (or after a break). Gathers the current branch, recent commits across all branches, uncommitted/staged changes, the in-repo STATUS/handoff docs, any Claude memory, prebuilt-binary state, and open TODOs, then prints a short "where we are / what's next" brief. Use when the user says "catch up", "where did we leave off", "what's the status", "resume", at session start, or switching machines (Windows build PC <-> Kubuntu board laptop).
---

# Catch up on CyberDeck

Goal: in one pass, reconstruct where the project stands so we can continue without re-deriving
context. This repo spans two machines (a Windows build PC and a Kubuntu laptop that holds the
SPECTRE board); **only the git repo syncs between them**, so the in-repo `STATUS.md` files are the
source of truth — Claude's `~/.claude` memory does NOT travel.

Run the steps below, then produce the **Catch-up brief** at the end. Keep it tight; don't dump raw
output — synthesise it.

## 1. Git state
Run these (they work in Git Bash on Windows and bash on Linux):
```bash
git branch --show-current
git log --oneline -8 --decorate
git branch -a --sort=-committerdate | head -12            # recent branches, incl. remotes
git log --oneline -6 --all --source | head -20            # recent work on any branch
git status --short                                        # uncommitted / staged changes
git stash list                                            # anything stashed?
```
Note: current branch, how it relates to `main`, the last few commits, whether the working tree is
clean, and whether other branches have newer work than the one checked out.

## 2. In-repo handoff / status docs (the cross-machine truth)
Find and read the project's own status files — they outlive memory:
```bash
git ls-files | grep -iE "status|handoff|readme|plan" | grep -ivE "node_modules"
```
Always read **`spectre/v1/STATUS.md`** if it exists (the main handoff). Also skim any `README.md`
in a directory that has recent commits, and `docs/` plan files if relevant to what's in flight.

## 3. Claude memory (machine-local — may be absent)
If present, read the memory index and the most relevant entries:
- Index: `~/.claude/projects/<this-project-dir>/memory/MEMORY.md`
  (on Windows: `C:\Users\<user>\.claude\projects\...\memory\MEMORY.md`).
- Open the 2-3 memory files whose one-line hooks match what the recent commits/STATUS are about.
If the memory dir doesn't exist (e.g. fresh machine), say so and lean on the in-repo STATUS docs
instead — and offer to mirror memory across machines if the user wants.

## 4. Build / artifact state
Check what's built and whether it's current:
```bash
git ls-files | grep -iE "prebuilt|\.bin$|\.apk$|\.kicad_pcb$"
```
- Firmware: is there a current `spectre/v1/prebuilt/*_MERGED.bin`? (flash target for the board)
- App: APK path (built on Windows at `Desktop\CyberDeck.apk`).
- PCB: `hardware/spectre-pcb/` — schematic/PCB/pinout present? which branch?
Mention anything that looks stale vs. the latest source commit.

## 5. Open threads
Scan for explicit next-steps and unfinished work:
```bash
git ls-files | xargs grep -lniE "TODO|FIXME|NEXT STEP|open question|pending|untested" 2>/dev/null | head
```
Prefer the "NEXT STEPS" sections in `STATUS.md` and the "open" notes in memory.

## 6. Output — the Catch-up brief
Print a short, skimmable brief in this shape (fill from what you found, omit empty parts):

```
## Catch-up — <date>
**Branch:** <current> (<ahead/behind main>)  ·  **Tree:** <clean | N files changed>
**Last commits:** <2-3 one-liners>

**Where we are:** <1-3 sentences of current state, from STATUS.md + memory>
**In flight / uncommitted:** <what's half-done or unstaged, if any>
**Build state:** <firmware bin / APK / PCB currency>

**Next steps:**
1. <the top 1-4 concrete actions, from STATUS "NEXT STEPS" / memory "open">

**Watch out for:** <any blocker, gotcha, or cross-machine note (e.g. board is on Kubuntu)>
```

Then ask one question: **"Pick up on <the top next step>, or something else?"**

## Notes
- Don't modify anything during catch-up — this is read-only.
- If on a feature branch (e.g. `spectre-pcb`), check whether `main` moved since it branched.
- Convert any relative dates you write into absolute ones.
- If git and STATUS.md disagree, trust git for *what changed* and STATUS/memory for *why / intent*.
