---
name: milestone-review
description: Check where each lockstep-watched project stands against its MILESTONES.md — which open items recent commits appear to have finished, what's overdue, and where tests are failing or stale on the Mac or Linux machine. Use when the user asks to review milestones, check project progress, or see where their projects are.
model: sonnet
---

# Milestone review

The user keeps a `MILESTONES.md` in each project that lockstep watches. Your job
is a manual check-in: compare what each plan says against what has actually
happened, and report. You do not change anything without a yes.

## 1. Gather

Run `lockstep review` (or `lockstep review <name>` if the user named one project).
It prints Markdown for each watched repo:

- git state on this machine
- the last `lockstep test` result here and on the other machines, marked when it
  ran against older code or uncommitted changes
- the current milestone, its due date and its open items
- the commits since `MILESTONES.md` last changed

Its repo paths come from `~/.config/lockstep/config.json`. If `lockstep` isn't on
PATH, try `~/.local/bin/lockstep`.

## 2. Look closer where it matters

For each repo with open items and commits since the plan changed, decide which
open items those commits plausibly finished. Commit subjects are often enough;
when they aren't, look at `git -C <path> show --stat <commit>` or the code itself.
Don't read every commit in full — spend effort where a match is ambiguous.

Also note:
- milestones that are overdue, or due soon with many open items
- tests failing, never run, or stale, especially when one machine passes and the
  other fails or hasn't run (a platform problem)
- dependencies: repos declared with `depends_on` in the lockstep config (e.g.
  StageHand and UmpireDAW on uw-core), plus any the README or build files make
  obvious. An open item in a library can block an app's milestone.
- repos with no `MILESTONES.md`, or no test command for a platform

## 3. Report

Keep it scannable. Lead with what needs the user's attention, then one short
block per project:

```
### StruckSynth — M3: Preset browser, 4/7, due Nov 15 (in 31 days)
Probably done, not ticked: "Browser UI" (a1b2c3d Add preset browser view)
Open: Search, Favorites
Tests: mac ✓ current · linux ✗ failed 2d ago
Next: run the Linux tests; looks on track otherwise
```

Projects with nothing new can share a single line at the end.

## 4. Offer, don't do

Ask before you:
- tick items in a `MILESTONES.md` (list exactly which ones)
- draft a `MILESTONES.md` for a repo that has none (base it on its README,
  TODOs and recent history, and say it's a draft for the user to edit)
- run `lockstep test <name>` (it can take a while)

Never commit or push. The user commits through their own flow, where lockstep's
hooks run.
