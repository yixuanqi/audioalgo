# Decision 0001: editor visual system and G0 dependency locks

Date: 2026-10-16
Status: accepted for the first editor slice

## Editor direction

AudioAlgo is an audio signal workbench. The interface uses a quiet, dark slate control surface so signal paths and the waveform carry the visual emphasis. The memorable accent is a sea-glass signal color, with a warm amber reserved for selection or active playback.

| Token | Value | Use |
|---|---|---|
| Canvas | `#101B23` | graph and waveform background |
| Panel | `#1F2D36` | library, inspector, transport bar |
| Text | `#E6ECE7` | primary labels and values |
| Signal | `#9AD3C8` | audio paths and selected waveform |
| Active | `#EDAF6E` | selection, playback and focus |
| Muted | `#7796A5` | secondary labels and inactive ports |

Use the system UI font for readable controls and a system monospace face only for sample rates, times, and numeric measurements. Keep labels in sentence case. Reserve a persistent top transport for project, run, save, play, and pause; the working area has a compact node library on the left, a large aligned canvas in the middle, and a parameter inspector on the right; waveform and analysis occupy a shallow bottom tray. Place node names and parameter values left aligned. Audio connections carry small port labels and use a consistent signal stroke.

The plan was checked against generic dashboard patterns: no equal rounded cards, gradient surfaces, decorative numbering, or universal neon accent. The visual hierarchy comes from signal flow and the waveform, which are specific to editing audio graphs.

## Dependency lock approach

The initial JUCE and Catch2 versions come from the locally installed vcpkg registry snapshot `be563dfee81e88f9f6cf68839dff5b671b5cfb37`. Their source archives are pinned by SHA-512 in `dependencies/versions.json`, so builds cannot silently follow a moving branch. SpeexDSP 1.2.1 is also pinned for the later algorithm gate. JUCE distribution licensing is unresolved; local engineering may proceed, while redistribution remains unverified.

## G0 Red/Green behavior

The G0 probe is registered only in a temporary build configuration and raises a Python assertion. `scripts/verify_gate.py --failure-probe-only` runs that CTest entry and records whether the non-zero test result propagated. The temporary test file/build entry is removed immediately after the evidence is captured.
