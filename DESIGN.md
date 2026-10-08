# lockstep-git — Design & Decision Handoff

> Handoff doc for continuing this side project. Captures every decision reached in
> the exploratory + build sessions so a fresh Claude (or me) can pick up cold.
>
> **Operating it?** [MACOS.md](MACOS.md) (build, install, launchd daemon lifecycle)
> and [LINUX_BRINGUP.md](LINUX_BRINGUP.md) (Linux cold-start) are the operational
> guides. This doc is the full rationale and the slice-by-slice progress log.

## Problem

Eric develops the same project across a **Mac and a Linux machine** and loses time
when the two get out of sync. Goal: a mostly-background tool that tracks both repos
and **warns/blocks at commit (or push) time** when the *other* machine has
uncommitted or unpushed work — so you never start committing on machine A while
machine B has pending changes you forgot about.

## Prior-art survey (conclusion: the niche is genuinely uncovered)

The existing tools split cleanly and nobody fuses the two halves:

- **Status scanners / dashboards** — [multi-git-status/mgitstatus](https://github.com/fboender/multi-git-status),
  [unpushed](https://pypi.org/project/unpushed/1.0.1/),
  [git-monitor](https://github.com/Funkenjaeger/git-monitor) (closest: a self-hosted
  Python dashboard that SSHes into each machine to collect uncommitted/unpushed
  state — but **visibility only**, no commit-time enforcement).
- **Command interception (shims)** — [destructive_command_guard](https://github.com/Dicklesworthstone/destructive_command_guard)
  and git-prism intercept `git` to block *dangerous* commands for AI agents. Proves
  the funnel mechanism, but nothing about cross-machine sync.
- **Multi-remote sync** — [MultiGit](https://dev.to/eshanized/multigit-one-repository-infinite-destinations-2adj)
  (Rust daemon) pushes to many remotes; doesn't *guard*.

Nobody has put "always-running daemon that knows the other machine's state" behind
"warn/block at commit time." That's the gap lockstep-git fills.

## Key architectural insight — publish, don't poll

The naive design ("periodically check the other machine") fails exactly when needed:
**when you're on Linux, the Mac is usually asleep.** Direct machine-to-machine
polling is dead on arrival.

Instead: **each machine publishes its own state to GitHub; the funnel reads the
published state.** GitHub is the always-on rendezvous both machines already
authenticate to — no extra server, no Tailscale, no SSH-into-a-sleeping-laptop.
Each daemon pushes a small heartbeat (dirty file count, ahead/behind, timestamp) to
a dedicated orphan branch or a gist. When the Mac is offline you still get
*"Mac had 3 uncommitted files as of 2h ago"* — precisely the warning you want.

This turns the hard part (cross-machine awareness) into a solved problem (push/pull
a tiny JSON blob).

## Architecture (three components, one binary + hooks)

1. **Daemon (`lockstepd`) — the brain, one per machine**
   - Watches repos (FSEvents on mac / fsmonitor or cheap poll on linux) and
     periodically `git fetch`es GitHub.
   - Publishes *this* machine's state to the rendezvous; caches the *other*
     machine's last-published state.
   - Exposes a local **Unix socket** for fast verdicts.
   - Written as a **plain foreground process**: logs to stdout/stderr, handles
     `SIGTERM` cleanly, does NOT self-daemonize. Let each OS's supervisor manage it.

2. **The funnel = git hooks, NOT a PATH shim**
   - `pre-commit` / `pre-push` hooks are the interception point: they fire whether
     you commit from the CLI, an IDE, or an agent; git-native; stay out of the way of
     unrelated `git status`/`log`.
   - Each hook is a ~5-line thin client that asks the daemon over the socket "am I
     clear?" and exits non-zero (with a message) to block.
   - A full `~/bin/git` shim was considered and rejected: more power but fragile and
     it has to reason about which git calls even matter. Hooks give warn/block for free.
   - Example block message: *"⛔ Mac has 2 unpushed commits on `dgl-ui` (as of 6m
     ago). Pull or resolve before committing here."*

3. **CLI + UI — both thin clients of the daemon socket**
   - CLI: `lockstep status`, `lockstep why` (explain the last block), config.
   - UI: **Qt `QSystemTrayIcon`** menubar/tray — green/yellow/red per repo + a
     desktop notification when the other machine goes dirty.

## Technology decisions

- **Language: C++** for the core (daemon + CLI). Reuses Eric's existing
  cross-platform C++ build experience (PMU already builds + ships installers on
  mac+linux). Rejected Go/Rust (the "single static binary" argument is irrelevant —
  this is Eric's own two machines / a personal repo). C considered but C++ wins for
  JSON/HTTP ergonomics. Swift is **not** viable as the shared core (no real Linux
  GUI story) — only a possible optional Mac-native front-end later, bolted on over
  the C++ daemon. **Not needed** given Qt covers both platforms with one codebase.
- **UI: Qt (`QSystemTrayIcon`)** — one C++ UI codebase for both mac + linux. Chosen
  over the Swift(mac)+DGL(linux) split specifically because it's a *public* repo and
  two UI codebases isn't worth maintaining. (`QSystemTrayIcon` works natively on both
  the macOS menubar and Linux — no macOS tray caveat, unlike GTK's SNI path. Qt's
  LGPL open-source build is fine here: personal, open, never sold.) Because the UI is
  a thin client of the daemon socket, the toolkit choice is isolated to the tray
  target and can be revisited without touching the core.
- **Libraries:** libgit2 (or shell out to `git`) for repo ops; libcurl for the
  GitHub rendezvous; nlohmann/json (header-only) for state blobs.
- **Build system: CMake as the single source of truth** (builds on both platforms).
  Editor is a free choice on top:
  - **VS Code + CMake Tools + clangd** day-to-day — identical on both machines
    (`CMAKE_EXPORT_COMPILE_COMMANDS` is on, so clangd gets `compile_commands.json`).
  - `cmake -G Xcode` when you want Xcode's lldb/Instruments — generated, never
    committed. `.gitignore` the build dir and any `*.xcodeproj`.
  - Do NOT make an `.xcodeproj` the canonical build (Mac-only, won't build on Linux).
  - **Generator: Ninja, not Unix Makefiles.** The active Xcode on this Mac lives at
    `/Applications/Xcode 26.3.app` — the space in the path breaks the Makefiles
    generator's sub-make invocation at compiler-detection time. `cmake -G Ninja`
    sidesteps it (and is faster). Configure with `cmake -S . -B build -G Ninja`.

## macOS startup (the daemon-launch question)

- Use a **LaunchAgent**, NOT a LaunchDaemon. LaunchDaemon runs as root at boot with
  no user session (no keychain, no GUI). LaunchAgent runs *as you* at login, with
  keychain access (GitHub token) and GUI session (tray). Lives in
  `~/Library/LaunchAgents/com.ericlkampman.lockstep.plist`.
- plist keys: `Label`, `ProgramArguments`, `RunAtLoad=true`, `KeepAlive=true`,
  `ThrottleInterval=10` (avoid crash-loops), `StandardErrorPath`.
- Manage with the modern API (old `launchctl load` is deprecated):
  - `launchctl bootstrap gui/$(id -u) <plist>`
  - `launchctl kickstart -k gui/$(id -u)/com.ericlkampman.lockstep` (restart after rebuild)
  - `launchctl bootout gui/$(id -u)/com.ericlkampman.lockstep` (stop)
- **Socket activation** (the `Sockets` plist key) is a nice-to-have: launchd owns the
  listening socket and can launch the daemon on first connect (the hook connects
  anyway). But we also need periodic polling, so baseline is `RunAtLoad`+`KeepAlive`,
  with socket activation optional later.
- **TCC / Full Disk Access gotcha:** FSEvents watching under protected folders
  (Desktop, Documents, Downloads, iCloud Drive) is silently blocked until the user
  grants Full Disk Access. Default watched roots to unprotected paths
  (`~/dev`, `~/Developer`). Document this.

## Linux startup

- **systemd `--user` unit**: `~/.config/systemd/user/lockstep.service`,
  `systemctl --user enable --now lockstep`.
- Because the daemon is a plain foreground process, launchd (mac) and systemd-user
  (linux) both just supervise it — **zero platform-specific code in the daemon.**

## Naming (settled)

- **Repo: `lockstep-git`** (disambiguates from the game-netcode "lockstep" crowd that
  dominates GitHub search; "lockstep" is a term of art for deterministic multiplayer
  sync).
- **Binary: `lockstep`** · **Daemon: `lockstepd`** (repo name ≠ command name is fine;
  Homebrew-style tools do this constantly). Command reads well: `lockstep status`.
- Cleared: apt (no package), web (uselockstep.app = unrelated compliance SaaS;
  lockstep.com = stale, irrelevant), GitHub (all hits are game networking, none in
  VCS space). Repo is namespaced under the account anyway, so no global collision.
- Scope: **mac + linux only. No Windows. No App Store. GitHub repo only** (no
  Homebrew formula / domain planned — so an `install` subcommand that writes the
  plist/systemd unit is the distribution path, not `brew services`).

## The A/B fork — RESOLVED: A now, B opt-in later

The hard case is **uncommitted changes on the other machine that were never pushed
anywhere** — GitHub can't see them.

- **(A) Metadata-only.** Daemon publishes *facts about* dirty state (file count,
  names, mtime, ahead/behind, timestamp). Cheap, safe, private. But you can only
  *warn* — the actual diff never leaves the offline machine.
- **(B) Publish-WIP.** Daemon auto-commits/pushes uncommitted work to a hidden
  `wip/<machine>` branch, so the other machine can actually *see and pull* it. True
  "best of both worlds," at the cost of pushing messy WIP to GitHub.

**Decision: ship (A) first; add (B) later as an opt-in per-repo setting.** (A)
delivers the warning behavior — 90% of the value and the original ask — with far
less machinery: the tick loop just gathers stats and writes a JSON blob, no throwaway
commits/branches to manage. (B) is deferred until there's a repo where pulling the
actual WIP is worth the noise.

## Security model

Threat: the published metadata leaks **filenames of in-progress work**, activity
timestamps (a behavioral fingerprint), and local paths/usernames. Almost all of the
exposure comes from the rendezvous being *public*, not from the data being inherently
secret.

**Design (simple by construction — no MITM surface):**

1. **Private rendezvous, not the public code repo.** Publish metadata to a
   **private repo or a secret gist**, separate from `lockstep-git` itself. GitHub
   auth then gates it for free: not world-readable, TLS in transit, encrypted at rest
   on GitHub's servers. This removes most of the exposure at ~zero cost and is done
   regardless of the crypto below.
2. **Symmetric AEAD for at-rest + authenticity (belt-and-suspenders).**
   - **libsodium** `crypto_aead_xchacha20poly1305` (or `crypto_secretbox`), fresh
     nonce per message prepended to the ciphertext. ~30 lines.
   - **One shared symmetric key, copied out of band once** (AirDrop / password
     manager / paste) between the two machines. This is NOT a key-*exchange*
     protocol — the out-of-band copy *is* the authentication, so there is **no MITM
     surface**. Contrast the trap of asymmetric key exchange *through* the rendezvous:
     anyone who can write to it (or a compromised GitHub) could substitute a public
     key — textbook MITM. Avoided entirely.
   - AEAD's auth tag gives **confidentiality + integrity/authenticity in one move**:
     a third party who can write to the rendezvous can't forge or tamper without the
     key (tampered blob fails the tag), and a valid blob is implicitly "from one of my
     machines" (only key-holders can produce a valid tag).
   - **Key at rest:** store in a `~/.config/lockstep/key` file, `0600`, relying on
     **FileVault/LUKS** for at-rest protection. Storing the key in macOS Keychain +
     libsecret (Linux) is **deferred** — two platform integrations for marginal
     benefit when the disk is already encrypted.
3. **No asymmetric crypto.** Per-machine keypairs / key exchange are the wrong tool
   for two machines you own and trust — pure cost, and they reintroduce MITM risk.

Net threat model: **TLS for transit · private rendezvous for access control ·
symmetric AEAD (out-of-band-shared key, FileVault/LUKS at rest) for at-rest secrecy +
authenticity · no asymmetric · no MITM surface.**

Cost estimate: private rendezvous ≈ free; symmetric AEAD layer ≈ half a day;
keychain/libsecret integration ≈ +1 day (deferred); asymmetric ≈ don't.

## Progress

**Slice 1 — CMake infra + daemon/CLI/hook spine — DONE (2026-09-18).**
The vertical spine builds and runs end-to-end on the Mac:
- `CMakeLists.txt` (top-level) + per-target `src/{common,daemon,cli}/CMakeLists.txt`.
  nlohmann/json pulled via `FetchContent` (no brew dep for the core build).
- `src/common/` — shared static lib: socket-path resolution (`paths.{h,cpp}`,
  honors `$LOCKSTEP_SOCKET`/`$XDG_RUNTIME_DIR`, else `~/.lockstep/daemon.sock`) and a
  blocking newline-delimited-JSON Unix-socket transport (`ipc.{h,cpp}`).
- `src/daemon/` — `lockstepd`: plain foreground process, `sigaction`-based (no
  SA_RESTART) SIGTERM/SIGINT handling so the accept loop actually wakes and cleans up
  the socket; answers `ping`/`verdict`/`status` with a **hardcoded "clear"** verdict.
- `src/cli/` — `lockstep {ping,status,verdict}`, thin socket client; `verdict` exits
  0 clear / 1 blocked / 1 if daemon unreachable (fail-closed for now).
- `hooks/pre-commit` — thin client that relays the verdict; `LOCKSTEP_SKIP=1` escape
  hatch; no-ops if the CLI isn't installed (absent guard shouldn't block commits).
- `.gitignore` — build dirs, `*.xcodeproj`, `compile_commands.json`, `.DS_Store`.

Verified: daemon up → `verdict` clear/exit 0; SIGTERM → clean exit + socket removed;
daemon down → clear error + exit 1; hook relays exit code.

**Slice 2 — config-driven repo watching + real local git state — DONE (2026-09-18).**
- `src/common/config.{h,cpp}` — reads `config.json` (`{"repos": [...]}`), XDG-aware
  path (`$LOCKSTEP_CONFIG` > `$XDG_CONFIG_HOME/lockstep` > `~/.config/lockstep`),
  tilde-expands paths; missing file = watch nothing (not an error).
- `src/common/subprocess.{h,cpp}` — `fork`/`execvp` capture of stdout, **no shell**
  (paths from config never hit a shell), stderr to /dev/null.
- `src/common/git.{h,cpp}` — `inspect(path)` shells out to git for branch, dirty
  count (`status --porcelain`), and ahead/behind (`rev-list --count --left-right
  @{u}...HEAD`); handles detached HEAD, no upstream, and non-repo/missing paths via
  `RepoState::error`. `is_clean()` = no dirty + not ahead.
- `lockstepd` `status` now scans all configured repos and returns a structured
  `repos` array + a summary ("N watched, M with pending local work, K unreadable").
  `lockstep status` renders a per-repo table (✓/● + branch + state).
- `verdict` still clear-by-design: it concerns the *other* machine, which needs the
  rendezvous (slice 3). Local dirty state is the user's own and never blocks them.

Verified against synthetic repos: clean/up-to-date, dirty, ahead-N, no-upstream,
non-repo dir, and missing path all report correctly; empty config → friendly notice.

**Slice 3 (core) — rendezvous crypto/blob + real blocking verdict — DONE (2026-10-08).**
Built and tested fully offline via a filesystem rendezvous backend standing in for
the GitHub repo:
- `src/common/crypto.{h,cpp}` — libsodium XChaCha20-Poly1305 AEAD; random nonce
  prepended per message; shared key as 0600 hex file; generate/load/save. ~as the
  security model specified (symmetric, out-of-band key, no asymmetric, no MITM).
- `src/common/blob.{h,cpp}` — per-machine state blob (machine id, timestamp, per-repo
  briefs). **Metadata-only: counts + branch + ahead/behind, NOT filenames.** Repos
  matched across machines by path **basename** (`~/dev/foo` ↔ `~/src/foo`).
- `src/common/rendezvous.h` + `rendezvous_file.cpp` — `Rendezvous` interface (publish
  my slot / fetch others'); `FileRendezvous` = one `<machine>.blob` per machine under
  a dir, atomic write-then-rename. The GitHub backend will layer commit/push/pull over
  this same shape.
- `lockstepd` verdict now: publish self, fetch others, decrypt, and **block** if the
  other machine has uncommitted/unpushed work in the committing repo. Message matches
  the doc's example ("⛔ linux has 2 uncommitted on proj (0s ago). Pull or resolve…").
- **Policy — fail OPEN:** unconfigured rendezvous/key, or an unreadable/undecryptable
  blob, never blocks real work; it returns clear. A key mismatch is surfaced in the
  clear message ("could not decrypt N blob(s) — key mismatch?") rather than silently
  disabling the guard. Only a definite "other machine is dirty" signal blocks.
- `lockstep keygen` writes the shared key (0600) and tells you to copy it out of band.
  `lockstep verdict` auto-detects the committing repo (git toplevel basename) and sends
  it so the check is scoped to that project.
- Config gained `machine`, `rendezvous_dir`, `key_path`; env overrides
  `$LOCKSTEP_KEY`, `$LOCKSTEP_RENDEZVOUS_DIR`.

Verified two-machine scenarios (mac+linux sharing a dir+key): both clean → clear;
linux dirty → mac blocks with the right message; linux commits+pushes → mac clear
again; wrong key on the daemon → fail-open clear + warn.

**Slice 3 (GitHub backend) — `GitHubRendezvous` — DONE (2026-10-08).**
- `src/common/rendezvous_github.cpp` — a `Rendezvous` backed by a local clone of the
  private repo. `publish`: sync to origin, write `<machine>.blob`, commit, `push -u
  origin HEAD`, retrying on a non-fast-forward (concurrent push); bootstraps an empty
  repo (unborn `main`). `fetch_others`: fetch + hard-reset to origin, read the other
  machines' blobs. Each machine only writes its own file, so the sole conflict is a
  rejected push, which the retry resolves. Daemon sets `GIT_TERMINAL_PROMPT=0` so a
  missing credential fails fast instead of hanging.
- Backend selection: `rendezvous_repo` (git clone) wins over `rendezvous_dir` (file,
  tests). Config keys + env `LOCKSTEP_RENDEZVOUS_REPO` / `LOCKSTEP_RENDEZVOUS_DIR`.
- Verified offline against a local bare repo as origin with two clones (mac/linux):
  empty-repo bootstrap, clean→clear, dirty→block (right message), commit+push→clear.
  Confirmed the pushed blobs are ciphertext — no plaintext (filenames/branch/state)
  on the remote.

**Real-world wiring (what you do once):** private repo `lockstep-rendezvous` cloned
to `~/Dev-Common/lockstep-rendezvous` (mac). Set `"rendezvous_repo"` to that path and
run `lockstep keygen`, then copy `~/.config/lockstep/key` to the Linux box and clone
the same private repo there. See "Current config" below.

**Slice 4 — background tick loop — DONE (2026-10-08).**
`lockstepd`'s main loop is now a single-threaded `poll()` loop: it serves the socket
and, on a timer, runs a **tick** that publishes this machine's state and fetches +
decrypts the other machines'. The result is cached (`RendezvousCache`), and
`verdict`/`status` read that cache — **no network on the commit path**, so the hook is
a fast local round-trip. Single-threaded by design: no mutex, and no
fork()-in-a-thread hazard from shelling out to git.
- Interval: `tick_seconds` in config (env `LOCKSTEP_TICK_SECONDS`), default 30, floored
  at 5, capped at 3600. A tick also runs once at startup to warm the cache.
- Fail-open preserved: no rendezvous/key → cache `configured=false` → clear with a
  note. A transient fetch error **keeps the last-known state** (stale beats silently
  unprotected) and adds a "sync issue … (showing last known state)" note; a config
  problem clears it. Decrypt failures are warned once per machine per run (no per-tick
  log spam).
- New tradeoff (much smaller): cross-machine state can be up to one tick-interval
  stale, which the existing "as of Xm ago" text already surfaces.
- Verified (file backend, 2s tick): empty → clear; other machine goes dirty → verdict
  stays clear until the next tick, then flips to blocked on its own; SIGTERM still
  exits cleanly through the poll loop.

**Slice 5 — repo management + Qt tray — DONE (2026-10-08).**
- `status` now shows the cached other-machine state (this-machine / other-machines
  view) — done earlier this day.
- **`lockstep add <path>` / `remove <path|name>`** (`src/cli/install.cpp`): edit the
  watched-repo list in `config.json` (via `ordered_json`, so key order and
  `{"path","depends_on"}` entries survive) and install/remove that repo's hooks.
  Daemon reloads on its next tick — no restart.
- **`lockstep-tray`** (`src/tray/`, optional Qt target, guarded by `find_package(Qt6
  QUIET)` so the core still builds without Qt): polls `status` every 5s, shows a
  "¿?" mark tinted per side: the **¿ is this machine** (yellow = a watched repo is
  behind its remote — pull before working — or unreadable), the **? is every other
  machine, worst wins** (red = some machine has pending work in a repo this machine
  watches; yellow = busy elsewhere / stale sync / key mismatch). The glyphs are
  separated at runtime by connected shape, so new artwork needs no re-splitting
  (it falls back to tinting the whole mark if it stops splitting cleanly). Notifies on
  worsening transitions, lists both machines' per-repo state, and offers "Add repo…"
  (folder picker) + "Remove repo" that shell out to `add`/`remove`. Enable with
  `-DCMAKE_PREFIX_PATH="$(brew --prefix qt)"`.

**Slice 5 polish — DONE (2026-10-08):** tray **autostart** is wired into `lockstep
install` (a second macOS LaunchAgent `…​.tray`, `KeepAlive` on-crash-only so the menu's
Quit sticks; Linux prints a note since tray autostart is desktop-specific), and the
**Dock icon** is gone — the tray sets the macOS activation policy to "accessory" at
startup via the ObjC runtime (no `.app`/LSUIElement bundle needed). `uninstall` tears
both down.

**Deferred deliberately (not yet built):** FSEvents/fsmonitor push watching (tick is a
plain timer — event-driven would cut the staleness window); `lockstep why`; tray
autostart on Linux (desktop-environment-specific).

## Suggested next steps on the Mac

1. ~~**Repo watching + real verdict.**~~ DONE (slice 2).
2. ~~**Rendezvous crypto/blob + blocking verdict.**~~ DONE (slice 3 core, offline).
3. ~~**GitHub-private-repo backend.**~~ DONE (slice 3 GitHub backend).
4. ~~**Background tick loop.**~~ DONE (slice 4) — timer-based; FSEvents/fsmonitor
   event-driven watching is a later refinement to cut the staleness window.
5. ~~**`lockstep install`** + `pre-push` hook.~~ DONE on Linux (2026-10-08); the
   LaunchAgent half is written but **untested on the Mac**. Idempotent:
   - links `~/.local/bin/{lockstep,lockstepd}` → the build output (rebuild + restart
     the service picks up new code);
   - writes the hook into every watched repo as `pre-commit` **and** `pre-push` (one
     script, wording keyed off `$0`; compiled into the CLI from `hooks/pre-commit`).
     Honors `core.hooksPath`; never overwrites a hook that isn't lockstep's;
   - writes + enables `~/.config/systemd/user/lockstep.service` /
     `~/Library/LaunchAgents/com.ericlkampman.lockstep.plist` (PATH includes
     Homebrew) and waits for the daemon to answer. Refuses if a hand-started
     daemon is running (two daemons would fight over the socket).
   - `lockstep uninstall` reverses it all, keeping config + key.
   Pre-push runs the same verdict as pre-commit (blocks on the other machine's
   pending work on this repo, prompts on dependency warnings).
6. ~~Qt tray + `add`/`remove`.~~ DONE (slice 5). Remaining tray polish: autostart via
   `install`, and macOS `.app`/LSUIElement bundling so it's menubar-only.

## Dependency-aware watching (Eric, 2026-10-08) — BUILT

Projects depend on shared libs: **StageHand** and **Umpire** both need **uw-core**.
A repo's verdict also considers its dependencies, but dependencies **warn, never
block**: the hook asks "Commit anyway? [y/N]" (default No) when at a terminal, and
just prints the warning when there's no terminal (IDE, GUI client, script).

- **Dependencies** = config `depends_on` ∪ submodules whose `.gitmodules` URL
  basename names a watched repo (StageHand's `external/uw-core` is detected
  automatically). Declare by writing a repo entry as an object:
  `{"path": "~/dev/dev-midi/UmpireDAW", "depends_on": ["uw-core"]}`.
- **Cross-machine warning:** a dependency with uncommitted/unpushed work in the
  other machine's blob (matched by basename, as today). If the other machine
  doesn't watch the dependency at all, the verdict adds a non-prompting note —
  **the dependency must be watched on both machines** to be checked.
- **Pin-drift warning (local):** a submodule pin committed in the parent's HEAD
  that differs from the standalone clone's HEAD (behind, ahead, diverged, or a
  commit the clone doesn't have).
- **Wire/CLI:** the verdict reply gains `warnings` (each with a `message`) and
  `notes`. `lockstep verdict --warn-exit` exits 3 for "clear but warnings"; plain
  `verdict` still exits 0 then, so older copied hooks never start blocking.

Not covered: unpushed commits made *inside* a submodule checkout (e.g. in
`StageHand/external/uw-core`) — the dirty-submodule case shows up as StageHand
being dirty, but unpushed submodule commits rely on
`git push --recurse-submodules=check`.

## Current config

Mac config lives at `~/.config/lockstep/config.json`. Example:
```json
{
  "machine": "mac",
  "repos": ["~/Dev-Tools/lockstep-git", "~/Dev-Common/some-project"],
  "rendezvous_repo": "~/Dev-Common/lockstep-rendezvous"
}
```
Shared key: `~/.config/lockstep/key` (0600), made with `lockstep keygen`, copied out
of band to the Linux box's same path. The private rendezvous repo is cloned to
`~/Dev-Common/lockstep-rendezvous` on the mac (clone the same repo on linux).
