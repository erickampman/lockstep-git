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

Everything through **slice 5** is done and verified on the Mac, all on `master`, and
Linux has run through the daemon/CLI/hooks side at least once:
- CMake scaffold; `lockstepd` daemon + `lockstep` CLI + `pre-commit`/`pre-push` hooks
  over a Unix socket.
- Config-driven watching of multiple repos; real git state via shell-out.
- Rendezvous crypto (libsodium AEAD), per-machine state blob, blocking verdict.
- `GitHubRendezvous` — publish/fetch the encrypted blobs through the private repo.
- **Background tick loop** (daemon syncs on a timer; `verdict`/`status` read a cache —
  no network on the commit path).
- **Dependency-aware watching** (`depends_on` + submodule pin drift → warnings, not
  blocks) and `lockstep status` showing both machines.
- **Slice 5:** `lockstep add`/`remove`, `lockstep install`/`uninstall`, and an optional
  Qt menubar tray. See "Slice 5 delta" below for what's new to pull on Linux.

It works end to end on both machines. If you're updating an existing Linux checkout
rather than starting cold, jump to "Updating an existing Linux checkout" below.

## Dependencies (Linux)

The core build needs only these. **Qt is NOT needed for the core** (it's only for the
optional tray — see "Slice 5 delta"), and **libgit2 is NOT needed** (we shell out to
the `git` binary).

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

4. **Install** (links into `~/.local/bin`, hooks into every watched repo, systemd
   user service started and enabled at login):
   ```bash
   ./build/bin/lockstep install       # re-run after adding repos to the config
   lockstep status                    # sanity check: lists watched repos
   ```
   After a rebuild: `systemctl --user restart lockstep`. Logs:
   `journalctl --user -u lockstep`.

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

The daemon syncs on a **timer** (`tick_seconds`, default 30), so cross-machine state
can be up to one tick stale — surfaced by the "as of Xm ago" text in messages.
`verdict` reads the cached state (no network on the commit path), and a transient
fetch error keeps the last-known state rather than dropping the guard. Event-driven
(FSEvents/fsmonitor) watching to shrink the window is a future refinement.

## Linux autostart

`lockstep install` writes `~/.config/systemd/user/lockstep.service` and enables it.
The daemon is a plain foreground process (logs to stdout/stderr, clean SIGTERM), so
systemd just supervises it — no platform-specific code in the daemon.

## Slice 5 delta — repo management + optional tray

New since Linux last synced. To catch up an existing checkout, see "Updating an
existing Linux checkout" below; what's new:

**`lockstep add` / `remove` — manage watched repos without hand-editing JSON:**
```bash
lockstep add  ~/dev/some-project     # appends to config.json, installs its hooks
lockstep remove some-project         # by repo name, or by path; removes its hooks
```
Config is rewritten preserving key order and `{"path","depends_on"}` entries. The
daemon reloads on its next tick — no restart. (Dependency-aware watching: write a repo
as `{"path": "~/dev/app", "depends_on": ["uw-core"]}` and the other machine's pending
work in `uw-core` becomes a *warning* when you commit in `app`, not a block. Git
submodules that point at another watched repo are detected automatically.)

**Optional Qt menubar tray (`lockstep-tray`)** — a thin GUI client of the daemon: a
status-tinted icon (green/amber/red) plus Add/Remove-repo menu items. It's **off
unless Qt is found at configure time**; the core always builds without it.
```bash
sudo apt install qt6-base-dev        # Debian/Ubuntu (Fedora: qt6-qtbase-devel)
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(qmake6 -query QT_INSTALL_PREFIX)"
cmake --build build                  # now also builds build/bin/lockstep-tray
```
Linux tray caveats (macOS has the cleaner story here):
- `lockstep install` does **not** autostart the tray on Linux — it only symlinks the
  binary and prints a note. Add `lockstep-tray` to your desktop environment's autostart
  yourself (it needs the graphical session).
- The tray uses a **StatusNotifierItem**: KDE/most DEs host it natively, but **GNOME
  needs an AppIndicator extension** or the icon won't appear.

## Updating an existing Linux checkout

If Linux already ran through an earlier slice and you just need to catch up:
```bash
cd ~/dev/lockstep-git        # your clone
git pull
cmake --build build          # CMake auto-reconfigures (the new src/tray subdir is
                             # Qt-guarded, so it's skipped unless Qt is installed)
lockstep install             # refresh binaries/hooks; restarts the systemd --user daemon
lockstep status              # confirm both machines
```
Hooks changed across slices, so re-running `install` (idempotent) keeps every watched
repo's `pre-commit`/`pre-push` current. Config is per-machine — manage Linux's watched
repos with `lockstep add`/`remove`; nothing about the repo list syncs from the Mac.
