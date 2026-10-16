# Native RADE: keeping it as patches against upstream

The RADE work lives on `feature/rade-native`, which grew out of the TEST branch (commit `0c871087`). Upstream took the
TEST series by rewriting it (same content, other hashes), so `feature/rade-native` cannot be merged with or rebased onto
`upstream/master` as a branch: the history under it is not upstream's. What carries over is the *content*, and the work
is arranged so that only a few lines of it have to meet upstream's code.

## Three layers

| Layer | What | Size now | Can it conflict with upstream? |
|---|---|---|---|
| 1 hooks | lines added to files upstream owns, each behind `#ifdef RADE_NATIVE` | 8 files, 172 lines added, 1 changed (Makefile) | yes: the only layer that can |
| 2 core | `src/rade_*.[ch]`, `rade_native.mk`: files that exist only here | about 7,100 lines | no |
| 3 extras | `tools/` (Reporter, log viewer, librade build), `test/`, `docs/rade-native*.md` | about 14,600 lines | no; none of it is built into piHPSDR |

Rules that keep layer 1 small (check them when adding anything):

- **No RADE logic in an upstream file.** A hook is a guard or a call into `src/rade_native*.c` and nothing else. Anything with
  more than a few lines goes into layer 2 and is called from the hook (the properties save/restore in `radio.c` is two calls
  for that reason).
- **Nothing in the Makefile except three lines: `include rade_native.mk`, `$(RADE_NATIVE_LIBS)` in `LIBS`, `$(RADE_NATIVE_OBJS)` in `OBJS`.** The Makefile hunks used to
  sat next to the TEST branch's DIVCAP block, which upstream does not have, and conflicted; the three lines now have the same neighbours in both; the options, objects,
  dependencies and the default are now in `rade_native.mk` (which also keeps the Makefile's default goal).
- **Nothing in `.gitignore`.** The run logs and captures write their own `.gitignore` into the directories they create
  (`rade_ignore_dir()`); `third_party/`, `tools/` and `test/` carry theirs.
- **Everything is off unless `make RADE_NATIVE=ON`**, so upstream's own build is not affected by the patches.

## The hooks (layer 1)

| File | Lines | What it does |
|---|---|---|
| `src/receiver.c` | 33 | `rx_full_buffer()`: the WDSP output is replaced by decoded speech; `rx_set_filter()`: the passband follows the waveform; `rx_set_af_binaural()`: I and Q kept apart |
| `src/transmitter.c` | 68 | `tx_full_buffer()`: the mic block is replaced by the modem signal, TX capture; `tx_set_filter()`; the speech processors (CFC, leveller, equaliser, phase rotator, downward expander, mic gain) held off while RADE transmit is on |
| `src/radio.c` | 34 | `radio_set_mox()`: the end of an over is held until the end-of-over frame has gone out; properties save/restore |
| `src/new_menu.c`, `.h` | 25 | RADE menu button and entry point |
| `src/actions.c`, `.h` | 9 | `MENU_RADE` action (assignable to a toolbar button) |
| `Makefile` | 3 | `include rade_native.mk`, `$(RADE_NATIVE_LIBS)`, `$(RADE_NATIVE_OBJS)` |

What the hooks rely on in upstream (a change to any of these is what breaks the build or the behaviour):
`RECEIVER` (`audio_output_buffer`, `output_samples`, `volume`, `filter_low/high`), `TRANSMITTER` (`mic_input_buffer`,
`iq_output_buffer`, `buffer_size`, `tune`, `twotone`, `txnoise`, `filter_low/high`), `radio_is_transmitting()`,
`radio_set_mox()`, `rx_full_buffer()`, `tx_full_buffer()`, the `tx_set_*` functions, the `GetProp`/`SetProp` macros, the
`ACTION` enum and `ActionTable`, WDSP's panel call `SetRXAPanelBinaural`.

## Tooling

`tools/rade/rade-patches.sh`:

- `export DIR` writes the set: `0001-rade-native-hooks.patch`, `0002-rade-native-core.patch`,
  `0003-rade-native-tools-tests-docs.patch` (the work against the commit it started from).
- `check [REF]` (default `upstream/master`, fetched first) makes a scratch worktree at REF, applies the three patches
  (`git apply --3way` for the hooks), and compiles the RADE objects and the six files with hooks with `RADE_NATIVE=ON`
  (no linking). It says which layer fails, and removes the worktree when all is well.

It works from committed content (`HEAD`): commit first.

## Bringing the work to a new upstream

1. `tools/rade/rade-patches.sh check` (after each upstream update, or before a release): if it says OK nothing more is needed.
2. If the hooks conflict: the log names the files; the conflicts are in the scratch worktree it leaves behind. Resolve there,
   build and test, then redo the same change on `feature/rade-native` (one hook per file, so it is a few lines each).
3. If only the build fails: an upstream API changed (the list above); fix the call in layer 2 where possible, in the hook if not.
4. For a branch that sits on upstream: `git switch -c rade-on-upstream upstream/master`, `git apply --3way 0001...`,
   `git apply 0002... 0003...`, commit as three commits.

## Current state

Checked against `upstream/master` `dbcaa64d` ("Auto Diversity" from the TEST branch) on 2026-10-16: see the entry in
`rade-native-progress.md` for the result of the last run of `check`.

## Not done, and why

- The `tx_set_*` guards in `transmitter.c` (five blocks of about six lines, one per speech processor) could be folded into a
  single call at the end of `tx_set_filter()`/profile loading; they are separate because each of those functions is also
  called on its own (profile loads, end of TUNE), which is exactly the bug they were added for (CFC re-enabled after TUNE).
  Left as they are until upstream changes that area.
- Layer 3 is probably not wanted upstream as it is (the Reporter and the log viewer are Python programs with their own
  dependencies). The export keeps it separate so that layers 1 and 2 can be offered alone.
