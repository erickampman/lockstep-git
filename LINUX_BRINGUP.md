# lockstep-git — Linux bring-up / handoff

> Read this first if you're a fresh Claude (or Eric) picking up on the **Linux**
> machine. It's the cold-start guide for getting lockstep running on Linux after
> it was built and verified on the Mac. Full design rationale + the slice-by-slice
> progress log live in [DESIGN.md](DESIGN.md) — read that too, but this file is the
> operational path.

## What this project is (one paragraph)

lockstep-git is a **commit-time guard**, not a file syncer. A daemon (`lockstepd`)
on each machine publishes its own git state (per-repo dirty count, ahead/behind) to
a shared **private GitHub repo** (`lockstep-rendezvous`), encrypted with a symmetric
key the two machines share out of band. A `pre-commit` hook asks the local daemon
"am I clear?" and **blocks** if the *other* machine has uncommitted/unpushed work in
the same project. The point is to stop the Mac and Linux boxes from drifting out of
sync. See DESIGN.md for the why.

## Current state (as of the Mac work, 2026-10-08)

Four slices are done and verified on the Mac, all on `master`:
- CMake scaffold; `lockstepd` daemon + `lockstep` CLI + `pre-commit` hook, talking
  over a Unix socket.
- Config-driven watching of multiple repos; real git state via shell-out.
- Rendezvous crypto (libsodium AEAD), per-machine state blob, blocking verdict.
- `GitHubRendezvous` — publish/fetch the encrypted blobs through the private repo.

It works end to end. The **next planned slice is a background tick loop** (see
"Known caveat" below). Nothing Linux-specific has been built or tested yet — that's
this document's job.

## Dependencies (Linux)

The core build needs only these. **Qt is NOT needed** (the tray target isn't built
yet), and **libgit2 is NOT needed** (we shell out to the `git` binary).

- A C++20 compiler (g++ ≥ 11 or clang ≥ 14)
- CMake ≥ 3.24
- `git`
- `pkg-config`
- **libsodium** + dev headers (pkg-config package name: `libsodium`)
- Network access at *configure* time: nlohmann/json is pulled via CMake
  `FetchContent` (a shallow git clone). Nothing else fetches at build time.

Debian/Ubuntu:
```bash
sudo apt install build-essential cmake git pkg-config libsodium-dev
```
Fedora:
```bash
sudo dnf install gcc-c++ cmake git pkgconf-pkg-config libsodium-devel
```
Ninja is optional on Linux (the Mac uses it only to dodge a spaced-Xcode-path bug).
Plain Makefiles are fine here.

## Build

```bash
git clone https://github.com/erickampman/lockstep-git.git
cd lockstep-git
cmake -S . -B build          # add -G Ninja if you prefer
cmake --build build
# binaries: build/bin/lockstepd and build/bin/lockstep
```

## Configure lockstep on this machine

1. **Clone the private rendezvous repo** somewhere (its own checkout, separate from
   this repo):
   ```bash
   git clone https://github.com/erickampman/lockstep-rendezvous.git ~/lockstep-rendezvous
   ```
   GitHub auth on Linux is its own login — set up `gh auth login` or a credential
   helper so `git push`/`pull` to that private repo work non-interactively. The
   daemon sets `GIT_TERMINAL_PROMPT=0`, so if auth isn't configured it fails fast
   (and the guard fails *open* — see caveats) rather than hanging.

2. **Create `~/.config/lockstep/config.json`** — note `"machine": "linux"` (it MUST
   differ from the Mac's `"mac"`, or the two machines would share one slot and never
   see each other):
   ```json
   {
     "machine": "linux",
     "repos": ["~/dev/lockstep-git", "~/dev/some-project"],
     "rendezvous_repo": "~/lockstep-rendezvous"
   }
   ```
   Repos are matched across machines by **path basename**, so the same project can
   live at `~/Dev-Tools/foo` on the Mac and `~/dev/foo` here — just keep the leaf
   directory name identical.

3. **Install the shared key** — THIS IS THE ONE THING NOT IN GIT (see below).
   Put the key Eric hand-carries from the Mac at `~/.config/lockstep/key`, mode 0600:
   ```bash
   chmod 600 ~/.config/lockstep/key
   ```
   Do NOT run `lockstep keygen` on Linux — that would make a *different* key and the
   two machines couldn't read each other's blobs. Reuse the Mac's key.

4. **Run the daemon and install the hook:**
   ```bash
   ./build/bin/lockstepd &            # or a systemd --user unit (see DESIGN.md)
   ./build/bin/lockstep status        # sanity check: lists watched repos
   cp hooks/pre-commit ~/dev/some-project/.git/hooks/pre-commit
   ```

## Verify it's talking to the Mac

With the Mac daemon also running and both pointed at the same rendezvous repo + key:
- `lockstep status` should list this machine's repos.
- If the Mac has uncommitted/unpushed work in a repo whose basename matches one of
  yours, committing in that repo here should **block** with a message like:
  `⛔ mac has 2 uncommitted on foo (3m ago). Pull or resolve before committing here.`
- A key mismatch doesn't block; it reports "could not decrypt N blob(s) — key
  mismatch?" That's the signal the key wasn't copied correctly.

## Info the Linux Claude needs that is NOT (and must not be) in git

- **The shared AEAD key.** It is copied out of band (AirDrop won't work mac→linux;
  use a USB stick, `scp`, a password manager, or paste). Never commit it, never put
  it in the rendezvous repo — the repo holds only the encrypted blobs. If Eric hasn't
  provided it yet, ask him for it; without it the guard can't read the Mac's state.
- **GitHub auth for this machine** is separate from the Mac's — the Linux box needs
  its own `gh auth login` / credential helper for the private rendezvous repo.

Everything else (the private repo URL `github.com/erickampman/lockstep-rendezvous`,
all code, all config shape) is in git.

## Known caveat to be aware of

No background tick loop yet, so `verdict` does a **live `git fetch`/`push` to the
rendezvous on every call** — a `pre-commit` hook therefore pays a network round-trip
to GitHub per commit (~0.5–2s). Correct but slow; offline it falls back to
stale-but-usable local state and still returns a verdict. The planned tick loop makes
the daemon sync in the background so `verdict` reads a cache. If per-commit latency is
annoying on Linux, that slice is the fix — see DESIGN.md "next steps".

## Linux autostart (deferred, sketch)

systemd `--user` unit at `~/.config/systemd/user/lockstep.service`, then
`systemctl --user enable --now lockstep`. The daemon is a plain foreground process
(logs to stdout/stderr, clean SIGTERM), so systemd just supervises it — no
platform-specific code. Not built yet; the `lockstep install` subcommand will write
this unit eventually.
