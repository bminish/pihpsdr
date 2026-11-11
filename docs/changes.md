# Local changes on this branch: the synthesised CW side tone

`feature/synth-sidetone` is `TEST` ([bminish/pihpsdr](https://github.com/bminish/pihpsdr), which tracks `dl1ycf/pihpsdr` `TEST`) plus the series below, built as a patch series for review (`tools/synth-sidetone/synth-sidetone-patches.sh`; the series is based on `origin/TEST`). Everything else in the tree, including the other documents, is `TEST`'s and is described at the end of this page. The write-up of the side tone itself is [synth-sidetone/README.md](synth-sidetone/README.md). Updated 2026-11-11.

## Register: what this branch changes

Numbers are the CW series' numbers in the register of the working branch `devtest/everything` (`docs/cw.md` there is the index); `TEST`'s own register stops at LC-063.

| ID | Kind | Summary | Files | Status |
|---|---|---|---|---|
| LC-064 | New | The local CW side tone is synthesised in the PipeWire output callback instead of being queued through the FIFO (`st_buffer`, padded with 6.7 to 8 ms of silence). Off by default: CW menu check box "Synth sidetone". Key-to-sink delay falls by about 8 ms at every quantum; element lengths unchanged; RF unaffected | `sidetone.c`, `sidetone.h`, `pipewire.c`, `cw_menu.c`, `radio.c`, `radio.h`, `transmitter.c` | Local, for review. Not ready for upstream (see the README's list) |
| LC-064 (part) | Fix | The first event after TX start is neither delayed nor lost: `sidetone_idle()` puts the renderer in the state of a long pause, and queued events are kept for 200 ms | `sidetone.c`, `sidetone.h`, `pipewire.c` | With LC-064 |
| LC-064 (optional) | Setting | PipeWire playback quantum 128 instead of upstream's 256: about 4 ms earlier on either path, no audible drop-outs on the test machine. **Local setting, its own commit, not for upstream** (upstream moved to 256 after drop-out reports with HDMI audio) | `pipewire.c` | Local only |
| LT-039 | Tooling | `tools/cwlat` (external side tone and RF latency, jitter and shape measurement), `test/sidetone` (unit and start-of-TX tests), `tools/synth-sidetone/synth-sidetone-patches.sh` (export and check the series) | `tools/`, `test/sidetone/` | Local. Not part of what is sent upstream |

Measurements, design record and the list of what is missing before upstream: [synth-sidetone/findings.md](synth-sidetone/findings.md), [synth-sidetone/design.md](synth-sidetone/design.md), [synth-sidetone/README.md](synth-sidetone/README.md).

## Not on this branch

- **LC-065**, a short paddle tap is no longer lost while the keyer waits for TX: the branch `fix/keyer-lost-first-tap` (one commit on `master`, independent of the side tone).
- **LC-066**, straight key: the start of a key hold is not lost in the RX-to-TX timing hole, the break-in timer plan, the later `cwlat` work and the Hermes Lite 2 runs: only on `devtest/everything`.
- RADE, binaural diversity and the rest of the working branch: `devtest/everything`. See `docs/branches.md` there.

## The rest of this tree is `TEST`'s

`TEST` carries our auto-diversity series on top of upstream; dl1ycf has taken it into `upstream/TEST` in three steps (`f05546e3`, `07ae1f56`, `30b327e2`: LC-001 to LC-063, 57 accepted, LC-003 and LC-004 deferred, LC-022 dropped). That register and its history are kept here unchanged and are not part of this branch's change:

| Document | What it holds |
|---|---|
| [changes/accepted.md](changes/accepted.md) | The 57 accepted changes, by topic, each linking to its write-up |
| [changes/upstream-reconcile.md](changes/upstream-reconcile.md) | The reconciles with upstream: what he took, how it was checked |
| [changes/open-items.md](changes/open-items.md) | Outstanding diversity work (as of `TEST`, 2026-10-30) |
| [changes/rules.md](changes/rules.md), [changes/git-workflow.md](changes/git-workflow.md), [changes/settled-decisions.md](changes/settled-decisions.md), [changes/ownership.md](changes/ownership.md) | House rules, workflow, settled decisions, who owns what |
| [changes/tooling.md](changes/tooling.md) | The local diversity test and capture tooling (LT-NNN) |
| [changes/history.md](changes/history.md) | Dated log of re-syncs, landings and decisions |
| [test-findings.md](test-findings.md), [diversity-measurements.md](diversity-measurements.md), [diversity-rade.md](diversity-rade.md), [menu-notes-dl1ycf.md](menu-notes-dl1ycf.md) | Captures, findings, the RADE design record and the notes for dl1ycf |

The write-ups by topic: [settings](changes/lc-settings.md), [menu](changes/lc-menu.md), [estimate and gate](changes/lc-gate.md), [RADE V1](changes/lc-rade.md), [CW reference](changes/lc-cw.md), [noise floor and Best](changes/lc-noise-floor.md), [engine](changes/lc-engine.md), [averaging](changes/lc-averaging.md). (`lc-cw.md` is the diversity engine's CW reference, not the side tone.)
