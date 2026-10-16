# Unlikelyware projects: overview for release planning

A snapshot (2026-10-16) of the ten repos lockstep-git watches, written as
background for a general discussion about releasing them. Status comes from each
repo's README and `MILESTONES.md`; visibility and license from GitHub.

## At a glance

| Project | What it is | Kind | Platforms | GitHub | License | Where it stands |
|---|---|---|---|---|---|---|
| **lockstep-git** | Commit-time guard across machines | Dev tool (daemon, CLI, hooks, Qt tray) | macOS, Linux | public | MIT | M1 and M2 done |
| **uw-core** | Shared MIDI/audio/plugin-host library | C++ library (submodule) | macOS, Linux | private | MIT | M1 done; M2 missing Linux UMP endpoints |
| **StageHand** | Live-performance CLAP host | App | Linux; macOS port nearly done | private | MIT | M1 done; M2 left: signing/notarization, icon, Finder open |
| **Umpire** (UmpireDAW) | MIDI 2.0–native DAW | App | Linux first | private | MIT | Planning; only M1 infrastructure and a UI spike |
| **KeyboardEDO** | Microtonal (EDO) MIDI keyboard controller | App | macOS, Linux | private | MIT | M1–M4 done; M5 (identity/discovery) partly done |
| **PlainOldSynth** | Classic subtractive synth | Plugin: CLAP, VST3, AUv2 | macOS, Linux | private | MIT (was proprietary) | M1–M3 done; README says "pre-release" |
| **ModicumFM** | 6-operator FM synth with friendly macros | Plugin: CLAP | macOS, Linux | private | MIT | M1, M3 done; M2 one item left |
| **StruckSynth** | Modal-synthesis struck-resonator synth | Plugin: CLAP | macOS, Linux | private | MIT | M1, M2 done |
| **StruckKit** | Physically modelled drum kit | Plugin: CLAP | macOS, Linux | private | MIT | M1 done except physical kick |
| **IFSynth** | Fractal (IFS) synth | Plugin: CLAP | macOS, Linux | private | MIT | M1 done; M2 two items left |

## How they fit together

```
uw-core (shared: UMP/MIDI 2.0, clock, CLAP host, JACK/CoreAudio, ALSA/CoreMIDI)
 ├── StageHand        (live host)        ─┐
 ├── Umpire           (DAW, planned)      ├─ apps
 ├── KeyboardEDO      (controller)       ─┘
 └── uw::ump layer → the synth plugins (StruckKit today; others planned)

PolygraphModular voice engine + DPF/NanoVG UI kit (vendored)
 └── IFSynth → StruckSynth → StruckKit;  IFSynth → TwinFM → ModicumFM;  PlainOldSynth

DPF fork (erickampman/DPF + pugl): Umpire uses it; StageHand and all plugins are slated to move onto it.

lockstep-git: standalone tool used to develop all of the above on two machines.
```

Common threads: everything is C++/CMake, MIDI 2.0 (UMP) first with MIDI 1.0
fallback, microtonal pitch (Pitch 7.9 / 7.25) supported end to end, and Linux and
macOS treated as equals. The plugins share one architecture: a raw CLAP entry
point (DPF used only for the GUI) so they can receive MIDI 2.0, plus MPE.

## The projects

### lockstep-git
Each machine runs a daemon that publishes its repos' git state (dirty count,
branch, ahead/behind) encrypted to a private GitHub "rendezvous" repo; pre-commit
and pre-push hooks block a commit when the other machine has pending work in the
same repo, and warn about pending work in dependencies. Also syncs test results
across machines and tracks `MILESTONES.md` progress in a Qt tray. Already public
and MIT. A general-audience tool, unlike the rest; the only one not tied to
music.

### uw-core
The shared foundation for the MIDI apps (and, for its UMP layer, the plugins):
UMP message handling and MIDI 1.0↔2.0 translation, clock and lock-free queues, a
plugin seam with a CLAP host implementation, audio backends (JACK, CoreAudio) and
MIDI backends (ALSA sequencer UMP, CoreMIDI) including UMP endpoints and function
blocks. Consumed as a pinned git submodule. Releasing any app means it either
goes public too or gets vendored.

### StageHand
A live-performance host for CLAP plugins: racks, setlists, a rig with effect
buses, a sequencer and a looper, with MIDI 1.0 and 2.0 both first-class. Mature
on Linux (JACK via PipeWire, ALSA sequencer, DGL rack window). The macOS port is
functionally complete (CoreAudio, CoreMIDI, plugin editors, app bundle); what
remains for a Mac release is signing, notarization, the plugin-loading
entitlement, an icon and Finder document opening. M3 (performance features:
panic, count-in, controller learn, MIDI out) is the next big chunk.

### Umpire (UmpireDAW)
A DAW built on UMP from the ground up, MIDI-only and Linux first, possibly audio
later. Still planning: the infrastructure (UI spike, uw-core, DPF fork) is done;
the DAW itself (piano roll, transport, clips, files) is M2, and the "first useful
version" is the end of M3. Furthest from release.

### KeyboardEDO
An on-screen MIDI keyboard for equal divisions of the octave (successor to
KeyEDO): piano, diamond, isomorphic tiled and octave-column layouts, multitouch,
custom layout editor, arpeggiator, `.kedo` documents with several layouts.
Sends MIDI 2.0 (Pitch 7.9 notes, per-note bend) or 1.0, on CoreMIDI and ALSA;
publishes itself as a UMP endpoint on macOS. Feature-rich and usable now;
remaining work is MIDI 2.0 identity on Linux, protocol negotiation and
timestamps. Uses SDL3 + Dear ImGui (not DPF).

### PlainOldSynth
A classic subtractive synth: stereo-detuned Tri/Saw/Pulse oscillator, 24 dB
low-pass, two ADSRs, two LFOs, three modulation sections, a flanger/chorus/delay
effect, full MIDI 2.0 and MPE. The only plugin already built as **CLAP, VST3 and
AUv2** (via clap-wrapper; auval passes), with Linux CI. Self-described
"pre-release"; the most release-ready plugin.

### ModicumFM
A 6-operator, DX7-style FM synth (32 algorithms) with a filter and chorus, whose
aim is to hide FM's complexity: role-based macros (Brightness, Bite, Grit, Body),
shared relative envelopes, responsiveness controls; the full operator panel is
the expert view. MIDI 2.0/MPE done with Linux CI. Left: per-note "realism"
randomness, a host check of the new Pitch panel, instrument templates, and
VST3/AUv2.

### StruckSynth
A modal-synthesis instrument: a shaped strike excites a bank of up to 64 tuned
resonant modes (bell, bar, tine, block), with material presets and expression
controls. MIDI 2.0/MPE done with Linux CI. CLAP only so far.

### StruckKit
A physically modelled drum kit forked from StruckSynth: snare with modelled
wires, two-headed toms, bronze-plate cymbals and hi-hats with choke, hand drums
and idiophones, a StruckKit or General MIDI key map, all in one instance. Left:
a physical model for the kick (it still uses an additive one). CLAP only, and
still on DPF's own CLAP wrapper (raw CLAP entry point is M2).

### IFSynth
A synth whose oscillator, envelopes and LFOs are all Iterated Function System
fractals, edited directly on custom fractal-editor controls, plus a second
oscillator, filter and modulation mixers. MIDI 2.0/MPE mostly done; left are
All Notes/Sound Off, pressure/timbre as modulation sources, and UI for two
host-only parameters. CLAP only.

## Things that cut across all releases

Points likely to matter in a release discussion, gathered from the above:

- **Decided (2026-10-16): everything is released open source under MIT**,
  funded by voluntary support (something like Ko-fi) rather than sales. LICENSE
  files are committed in every repo; PlainOldSynth and PolygraphModularUnified
  were switched from their proprietary pre-release terms. Still to do: make the
  private GitHub repos public.
- **Shared code goes public with the apps.** uw-core, the DPF fork and
  PolygraphModularUnified are dependencies, so they go public too.
- **Plugin formats.** Only PlainOldSynth has VST3/AUv2. The others are CLAP-only,
  which limits the hosts (Logic, Pro Tools, Ableton) they can reach without
  clap-wrapper.
- **macOS distribution.** Signing and notarization are an open item for
  StageHand and will apply to every app and plugin bundle.
- **Planned migration.** Every plugin and StageHand are slated to move onto the
  DPF fork; deciding whether that happens before or after a first release
  affects timing.
- **Release-readiness, roughly:** lockstep-git (already public) → PlainOldSynth,
  KeyboardEDO, StruckSynth → ModicumFM, StruckKit, IFSynth, StageHand (macOS
  packaging) → Umpire (not yet built).
