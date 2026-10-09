# lockstep-git

A commit-time guard for people who work on the same repos from more than one
machine. If your other machine has uncommitted or unpushed work in a project,
committing or pushing that project here is **blocked** until you deal with it:

```
⛔ mac has 2 uncommitted on my-project (6m ago). Pull or resolve before committing here.
```

It is not a file syncer. Each machine runs a small daemon (`lockstepd`) that
publishes its own git state (per-repo dirty count, branch, ahead/behind; no
filenames) to a **private GitHub repo you own**, encrypted with a key shared
between your machines. `pre-commit` and `pre-push` hooks ask the local daemon
"am I clear?"; it answers from a cache, so the hook never waits on the network.
Because each machine *publishes* rather than being polled, you still get the
warning when the other machine is asleep or offline.

macOS and Linux. No Windows.

## How it fits together

- **`lockstepd`**: one per machine, run by launchd (macOS) or systemd `--user`
  (Linux). Every `tick_seconds` (default 30) it publishes this machine's state and
  fetches the others'.
- **Rendezvous repo**: an empty private GitHub repo. Each machine owns one file in it,
  `<machine>.blob` (encrypted with libsodium XChaCha20-Poly1305).
- **Shared key**: one symmetric key, generated once and copied to each machine by
  hand. It is never committed anywhere.
- **Hooks**: installed into each watched repo. They block on the other machine's
  pending work in *this* repo, and warn (with a `[y/N]` prompt at a terminal) about
  pending work in its dependencies.
- **`lockstep-tray`** (optional, Qt): a menubar/tray icon showing both sides.

Repos are matched across machines by **directory basename**, so `~/src/foo` on one
machine and `~/dev/foo` on another are the same project. Keep the leaf names
identical, and avoid watching two different repos that share a basename.

Design rationale and history: [DESIGN.md](DESIGN.md). Platform details:
[MACOS.md](MACOS.md), [LINUX_BRINGUP.md](LINUX_BRINGUP.md).

## Setting it up

You'll do steps 1 and 4 once overall, and the rest on every machine.

### 1. Create your rendezvous repo (once)

On GitHub, create a new **private**, **empty** repository. Any name works;
`lockstep-rendezvous` is the convention. Initializing it with a README is harmless
(only `*.blob` files are read).

> **Warning:** use a dedicated repo and a dedicated clone. The daemon runs
> `git reset --hard` against origin in that clone on every tick. Pointing it at a
> checkout you work in will throw away your uncommitted changes.

### 2. Install dependencies and build (each machine)

You need a C++20 compiler, CMake ≥ 3.24, git, pkg-config and libsodium. CMake also
needs network access at configure time to fetch nlohmann/json.

```bash
# macOS
xcode-select --install
brew install cmake ninja pkg-config libsodium

# Debian/Ubuntu
sudo apt install build-essential cmake git pkg-config libsodium-dev

# Fedora
sudo dnf install gcc-c++ cmake git pkgconf-pkg-config libsodium-devel
```

```bash
git clone https://github.com/erickampman/lockstep-git.git
cd lockstep-git
cmake -S . -B build -G Ninja      # -G Ninja is optional on Linux
cmake --build build
```

For the optional tray, install Qt 6 and point CMake at it:
`-DCMAKE_PREFIX_PATH="$(brew --prefix qt)"` on macOS, or
`-DCMAKE_PREFIX_PATH="$(qmake6 -query QT_INSTALL_PREFIX)"` on Linux
(`qt6-base-dev` / `qt6-qtbase-devel`). Without Qt, the core still builds.

### 3. Clone the rendezvous repo with working non-interactive auth (each machine)

```bash
git clone git@github.com:<you>/lockstep-rendezvous.git ~/lockstep-rendezvous
```

The daemon runs under launchd/systemd, not your shell, and has git prompts turned
off, so pushing must work **without any prompt**:

- **HTTPS clone:** use a credential helper: `gh auth setup-git` after
  `gh auth login`, or the macOS keychain helper.
- **SSH clone:** the key must be usable without your interactive `ssh-agent`.
  systemd user services usually don't inherit `SSH_AUTH_SOCK`. A key without a
  passphrase dedicated to this repo (a deploy key with write access) is the simplest.

Check it from a plain shell. This must succeed without asking for anything:

```bash
GIT_TERMINAL_PROMPT=0 git -C ~/lockstep-rendezvous ls-remote origin
```

### 4. Generate the shared key (once, on one machine only)

```bash
./build/bin/lockstep keygen      # writes ~/.config/lockstep/key, mode 0600
```

Copy that file to the **same path** on every other machine (scp, USB stick,
password manager), then `chmod 600 ~/.config/lockstep/key` there. Do **not** run
`keygen` on the other machines: different keys can't read each other's blobs.
Never commit the key, and never put it in the rendezvous repo.

### 5. Write the config (each machine)

`~/.config/lockstep/config.json` (or `$XDG_CONFIG_HOME/lockstep/config.json`, or the
path in `$LOCKSTEP_CONFIG`):

```json
{
  "machine": "laptop",
  "rendezvous_repo": "~/lockstep-rendezvous",
  "repos": []
}
```

- `machine`: this machine's name as the others will see it. It defaults to the
  hostname, but setting it explicitly is better. It **must differ** on every machine,
  or they'll overwrite each other's slot.
- `rendezvous_repo`: the clone from step 3.
- Optional: `key_path` (default: `key` next to `config.json`), `tick_seconds`
  (default 30, range 5–3600).

### 6. Install and add repos (each machine)

```bash
./build/bin/lockstep install          # links binaries into ~/.local/bin, starts the daemon
lockstep add ~/dev/my-project         # watch a repo and install its hooks
lockstep status                       # this machine and the others
```

`install` is idempotent: re-run it after rebuilding. `~/.local/bin` should be on
your `PATH` for interactive use (the hooks don't need it). Each machine has its own
list of watched repos, so add a project on **every** machine you want it guarded on.
Use `lockstep remove <name|path>` to stop watching one.

### 7. Check it works

With the daemons running on two machines, make an uncommitted change in a watched
repo on machine A. Within one tick, `lockstep status` on machine B shows it, and
`git commit` in that repo on B is blocked.

If nothing ever shows up, look at `lockstep status`:

- **"sync issue: …"**: the daemon can't fetch or push the rendezvous repo. This is
  almost always auth (step 3). Logs are in `~/Library/Logs/lockstep.log` (macOS) or
  `journalctl --user -u lockstep` (Linux).
- **"could not decrypt N blob(s): key mismatch?"**: the key files differ between
  machines (step 4).

## Day to day

```bash
lockstep status      # both machines' state, from the last sync
lockstep verdict     # exit 0 clear / 1 blocked (what the hooks run)
lockstep ping        # is the daemon up?
LOCKSTEP_SKIP=1 git commit ...   # bypass the guard once
```

**Visual diffs.** `lockstep diff [<name|path>] [--last]` opens this machine's
uncommitted changes against `HEAD` (or, with `--last`, the last commit) in
`git difftool --dir-diff`. With no argument it uses the repo you're in. It uses
git's `diff.tool`, so set one first, e.g. `git config --global diff.tool bc`.
Untracked files aren't shown, though it tells you how many there are.

**Dependencies.** If one project depends on another watched repo, declare it and the
other machine's pending work there becomes a warning, not a block:

```json
"repos": ["~/dev/core-lib", {"path": "~/dev/app", "depends_on": ["core-lib"]}]
```

Git submodules that point at a watched repo are detected automatically. The
dependency must be watched on both machines to be checked.

## Behavior you should know about

- **It fails open.** If the rendezvous or key isn't configured, or the other
  machine's blob can't be read, commits are allowed and `status` says why. Only a
  definite "the other machine has pending work" blocks. Check `status` after setup;
  don't assume silence means you're protected.
- **State can be one tick old.** Messages show "as of Xm ago". A machine that's
  asleep keeps showing its last published state, which is usually what you want.
- **Existing hooks are left alone.** If a repo already has a non-lockstep
  `pre-commit` or `pre-push` (husky, the pre-commit framework, your own script),
  `install`/`add` won't overwrite it, so that repo isn't guarded. To keep both, call
  `lockstep verdict` from your existing hook (see [hooks/pre-commit](hooks/pre-commit)
  for how lockstep's hook handles exit codes).
- **Logged-out Linux machines.** The systemd `--user` daemon runs only while you're
  logged in unless you run `loginctl enable-linger "$USER"`.
- **GNOME** needs an AppIndicator extension to show the tray icon.
- **More than two machines** works: each publishes its own blob, and the verdict and
  tray consider all of them.

## Uninstalling

```bash
lockstep uninstall
```

This stops the daemon and tray, and removes the services, the hooks lockstep
installed, and the `~/.local/bin` links. It keeps your config and key.

## License

[MIT](LICENSE)
