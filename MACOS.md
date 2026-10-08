# lockstep-git — macOS guide

> The Mac counterpart to [LINUX_BRINGUP.md](LINUX_BRINGUP.md): how to build, install,
> and operate lockstep on macOS, with the launchd daemon lifecycle spelled out. Full
> design rationale and the slice-by-slice log are in [DESIGN.md](DESIGN.md). Setting
> lockstep up from scratch (rendezvous repo, key, config)? Start with the
> [README](README.md).

## Dependencies (macOS)

Same core as Linux — and the same two non-dependencies: **Qt is not needed** for the
core (only for the optional menubar app), and **libgit2 is not needed** (we shell out
to `git`).

- Xcode Command Line Tools (clang, C++20): `xcode-select --install`
- CMake ≥ 3.24 and **Ninja**: `brew install cmake ninja`
- **libsodium**: `brew install libsodium` (found via pkg-config)
- `git`, `pkg-config` (pkg-config: `brew install pkg-config`)
- Network at *configure* time for nlohmann/json (CMake FetchContent)

### Build — use the Ninja generator

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

**Why Ninja, not the default Makefiles:** the active Xcode on this Mac lives at
`/Applications/Xcode 26.3.app` — the space in that path breaks CMake's Unix Makefiles
generator at compiler-detection time. Ninja sidesteps it (and is faster). This is a
Mac-only wrinkle; Linux is fine with plain Makefiles.

For the menubar app, install Qt (`brew install qt`) and configure with
`-DCMAKE_PREFIX_PATH="$(brew --prefix qt)"` — see "The menubar app" below.

## Install / uninstall

`lockstep install` (run it from the build output so it can find `lockstepd` beside
itself) does the following and is safe to re-run:

```bash
./build/bin/lockstep install
```

1. **Binaries** — symlinks `~/.local/bin/lockstep` and `lockstepd` to this build.
2. **Hooks** — writes `pre-commit` and `pre-push` into every watched repo's hooks dir
   (honoring `core.hooksPath`/worktrees). It only overwrites hooks that are lockstep's
   own; a foreign hook is left untouched.
3. **Service** — writes the LaunchAgent plist and (re)starts the daemon.
4. **Tray** (only if `lockstep-tray` was built — see below) — symlinks it, writes a
   second LaunchAgent (`com.ericlkampman.lockstep.tray`), and starts the menubar app.

`lockstep uninstall` reverses all of it (stops+removes both agents, removes lockstep's
hooks, removes the symlinks) and **leaves your config and key in place**.

After install you'll likely see **`note: ~/.local/bin isn't on your PATH`**. The hooks
don't care (they fall back to the `~/.local/bin` path explicitly), but to type
`lockstep` directly:

```bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zshrc && source ~/.zshrc
```

## The daemon (launchd) — the important part

lockstep runs as a **LaunchAgent**, not a LaunchDaemon: it runs *as you* at login,
with your keychain (GitHub credentials) and GUI session — which a root LaunchDaemon
wouldn't have. Defined at:

```
~/Library/LaunchAgents/com.ericlkampman.lockstep.plist
```

Key plist settings: `RunAtLoad` (start at login) · `KeepAlive` (restart if it exits) ·
`ThrottleInterval 10` (don't crash-loop faster than every 10s) · an explicit `PATH`
with Homebrew (`/opt/homebrew/bin`) so the daemon finds the same `git` and credential
helper your shell does — launchd otherwise starts agents with a bare PATH.

**Logs:** `~/Library/Logs/lockstep.log` (both stdout and stderr).

### Checking whether it's running — mind the `ps` trap

A LaunchAgent is started by launchd in its own session with **no controlling
terminal**, so a bare `ps` (which only lists processes on your current tty) will **not
show it** — even though it's running. Use one of these instead:

```bash
pgrep -fl lockstepd
launchctl print gui/$(id -u)/com.ericlkampman.lockstep   # state = running, pid, last exit code
lockstep ping                                            # "daemon is up"
```

### Lifecycle commands

```bash
# After rebuilding the binary — launchd keeps running the OLD one until you kick it:
launchctl kickstart -k gui/$(id -u)/com.ericlkampman.lockstep

# Stop / start by hand:
launchctl bootout   gui/$(id -u)/com.ericlkampman.lockstep
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.ericlkampman.lockstep.plist
```

**The rebuild rule:** `cmake --build build` updates the binary on disk, but the running
daemon is a separate process launchd started — it keeps serving the old code until you
`kickstart -k` it (or re-run `lockstep install`, which does that for you). The `install`
output prints this reminder too.

## Config and key

- Config: `~/.config/lockstep/config.json` (XDG-aware; `$LOCKSTEP_CONFIG` overrides).
  Repos may be plain paths or `{"path": …, "depends_on": ["uw-core"]}`. Optional
  `tick_seconds` (default 30) sets the background sync interval.
- Shared key: `~/.config/lockstep/key` (0600), the same bytes as the Linux box
  (copied out of band — never committed, never in the rendezvous repo).
- Rendezvous clone: a private repo cloned locally, pointed at by `rendezvous_repo`.

A config edit is picked up on the daemon's next tick — **no restart needed** for config
changes. Only a *binary* rebuild needs the `kickstart`.

## macOS-specific gotcha: Full Disk Access / TCC

FSEvents watching under TCC-protected folders (Desktop, Documents, Downloads, iCloud
Drive) is silently blocked until you grant the daemon Full Disk Access. The current
daemon uses a timer, not FSEvents, so this doesn't bite yet — but keep watched repos
under unprotected paths (e.g. `~/Dev-Tools`, `~/Dev-Common`, `~/Dev-MIDI`) and it stays
a non-issue. Relevant if/when event-driven watching lands.

## The menubar app (lockstep-tray)

Optional Qt UI. It's **not built by default** — enable it by configuring with Qt:

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build
./build/bin/lockstep install    # now also installs + starts the tray agent
```

- A "¿?" menubar mark, tinted per side. The **¿ is this machine**: yellow when a
  watched repo is behind its remote (pull before working) or unreadable. The **? is
  the other machines**, worst wins: red when one has pending work in a repo this
  machine watches; yellow when one is busy elsewhere or behind, or on a stale sync or
  key mismatch. Click it for both machines' per-repo state, **Add repo…** (folder
  picker) and **Remove repo**.
- **Menubar-only, no Dock icon** — it sets the macOS activation policy to "accessory"
  at startup (no `.app` bundle needed).
- Autostarts at login via its LaunchAgent, which uses `KeepAlive` only on crash — so
  choosing **Quit** from its menu stops it until next login (or `launchctl kickstart`).
- Logs: `~/Library/Logs/lockstep-tray.log`. After a rebuild, refresh it with
  `launchctl kickstart -k gui/$(id -u)/com.ericlkampman.lockstep.tray`.

## Everyday checks

```bash
lockstep status    # this machine + the other machine (from the last background sync)
lockstep verdict   # exit 0 clear / 1 blocked / 3 clear-but-warnings (--warn-exit)
lockstep ping      # daemon liveness
```
