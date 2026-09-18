# lockstep-git — Design & Decision Handoff

> Handoff doc for continuing this side project on macOS. Captures every decision
> reached in the exploratory session so a fresh Claude (or me) can pick up cold.

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
  two UI codebases isn't worth maintaining.
- **Libraries:** libgit2 (or shell out to `git`) for repo ops; libcurl for the
  GitHub rendezvous; nlohmann/json (header-only) for state blobs.
- **Build system: CMake as the single source of truth** (builds on both platforms;
  first-class Qt support). Editor is a free choice on top:
  - **VS Code + CMake Tools + clangd** day-to-day — identical on both machines.
  - `cmake -G Xcode` when you want Xcode's lldb/Instruments — generated, never
    committed. `.gitignore` the build dir and any `*.xcodeproj`.
  - Do NOT make an `.xcodeproj` the canonical build (Mac-only, won't build on Linux).

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

## OPEN DECISION — the A/B fork (shapes the daemon's core loop)

The one unresolved decision. The hard case is **uncommitted changes on the other
machine that were never pushed anywhere** — GitHub can't see them.

- **(A) Metadata-only.** Daemon publishes *facts about* dirty state (file count,
  names, mtime, ahead/behind, timestamp). Cheap, safe, private. But you can only
  *warn* — the actual diff never leaves the offline machine.
- **(B) Publish-WIP.** Daemon auto-commits/pushes uncommitted work to a hidden
  `wip/<machine>` branch, so the other machine can actually *see and pull* it. True
  "best of both worlds," at the cost of pushing messy WIP to GitHub.

**Decide this before writing the daemon's tick loop.** (Possible middle path: start
with A, add B as an opt-in per-repo setting.)

## Suggested next steps on the Mac

1. Decide the A/B fork.
2. `git init lockstep-git`; commit this file as `DESIGN.md`.
3. Scaffold: CMake project with `lockstepd` (daemon), `lockstep` (CLI), and a Qt tray
   target; `.gitignore` (build dir, `*.xcodeproj`); the `pre-commit`/`pre-push` hook
   client; a GitHub-rendezvous stub; the LaunchAgent plist + systemd user unit; an
   `install` subcommand.
