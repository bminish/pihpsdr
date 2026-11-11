# Branches

What each branch in this fork (`origin`, `bminish/pihpsdr`) is for, and
where old work goes. Written 2026-10-30, updated 2026-11-11. Upstream is `dl1ycf/pihpsdr`;
nothing is ever pushed there.

## Live branches

| Branch | What it is | Safe for others? |
|---|---|---|
| `master` | Follows upstream `master`. | Yes |
| `TEST` | `upstream/TEST` plus our local layer only: the capture recorder (`make DIVCAP=1`), the test harness and tools, and the docs. Every diversity change we proposed that upstream took is in it. | Yes. The latest tested diversity state. |
| ~~`feature/diversity`~~ | Retired 2026-11-11. It held the per-change LC history of the diversity series (31 commits ahead of `TEST`, 4 behind; its tree had nothing `TEST` lacks). Kept as the tag `backup/feature-diversity-20261111` (not yet bundled into `~/sdr/archive/`). `TEST` is the stable diversity base. | — |
| `feature/diversity-binaural` | `TEST` plus the binaural work: the ear split (a left and a right ear from the two antennas), the shared AGC link, Balance ATT, the Link AGC menu row, and the RADE V2 captures. | **No, highly experimental.** Expect it to be rebased and its history rewritten. |
| `feature/rade-native` | `TEST` plus the native RADE work (RADE V1 and V2 run inside piHPSDR through `librade`, the Reporter, the log viewer, the air tests) and the RADE V2 diversity reference (LC-071 to LC-075, renumbered from LC-064 to LC-068 on 2026-11-11; with its V2 captures T-027 to T-030), LC-070/LC-076 (RADE leaves on a mode request, entering RADE sets the diversity reference), the CW series and `RADE_NATIVE` on by default. Rebased onto `TEST` on 2026-10-30 (128 commits); `librade` is built separately (see `README.md`). Old tip: tag `backup/rade-native-pre-rebase-20261030`. | No, experimental |
| `devtest/everything` | **The active "everything" branch** (named `devtest/sidetone` until 2026-11-11). `TEST` plus binaural diversity plus native RADE (V1 and V2, LC-070 to LC-076) plus the CW work: the synthesised side tone (LC-064), the keyer fixes (LC-065, LC-066), the CW filter drift fix, `tools/cwlat` and their documents (`docs/cw.md` is the index). Includes the former `devtest/binaural-rade` and `feature/cw-synth-sidetone` (deleted 2026-11-11; tag `backup/cw-synth-sidetone-20261111`). `RADE_NATIVE` is off by default here: see `README.md` for the build. Entry point: `README.md`. | No, experimental |
| `feature/synth-sidetone` | The side tone as a series of patches on `master` (`tools/synth-sidetone/synth-sidetone-patches.sh`). Not ready for upstream. Refreshed 2026-11-11 (comments corrected; README points at `fix/keyer-lost-first-tap`). | Yes, for review |
| `fix/keyer-lost-first-tap` | One commit on `master`: `iambic.c` no longer loses a short tap that arrives while the keyer waits for TX. Independent of the side tone. | Yes |
| `fix/diversity-flat-weighting-TEST` | PR #149 to upstream. Leave it alone while the PR is open. | PR branch |
| `fix/p2-unused-adc-bpf-bypass-master` | PR #140 to upstream. Leave it alone while the PR is open. | PR branch |

`TEST` tracks `upstream/TEST` locally, so `git pull` on it pulls from
upstream, not from `origin`.

## Number clashes to settle before these branches meet

The two experimental branches were developed in parallel from the same
`TEST`, and each kept numbering from where `TEST` stopped. Two sets of
numbers now mean different things:

| ID | On `TEST` | `feature/diversity-binaural` | `feature/rade-native` |
|---|---|---|---|
| T-026 | the empty-band stand-down | the per-ear WAV of 2026-10-05 (T-024 to T-027 are RADE V2 on 7.0879 MHz) | `TEST`'s, unchanged |
| T-027 | | per-ear WAV; 160 m captures run T-028 to T-035 | RADE V2 on 40 m (T-027), then T-028 to T-030 |
| T-028 to T-030 | | 160 m captures | 60 m, 80 m and the start weight |
| LT-037 | `test_modal` checks of LC-061 | | RADE V2 tooling, renumbered **LT-038** here |

`feature/diversity-binaural` already carries two different T-026 entries
(its own and `TEST`'s), because it was rebased onto a `TEST` that took T-026
after the branch had used it. Nothing is lost, but a reference to "T-026"
is ambiguous there. Suggested fix, to be done at the first merge of the two
branches: keep `TEST`'s numbers, renumber the binaural captures T-024 to
T-035 and the RADE ones T-027 to T-030 into one sequence after the highest
number on `TEST`, and record the old-to-new map in `test-findings.md`. LC
numbers do not clash since 2026-11-11: LC-041 to LC-045 are binaural, LC-064 to
LC-066 are the CW series, LC-071 to LC-075 are RADE V2 (renumbered from
LC-064 to LC-068), LC-070 and LC-076 RADE/diversity.

## Archive: bundles, not branches

Finished, superseded or abandoned work, and the `backup/` tags, are kept in
bundles in `~/sdr/archive/`, not on `origin` and not in this clone. A branch
that is done leaves `origin` the same way (bundle, verify, delete).

| Bundle | Holds |
|---|---|
| `pihpsdr-history-backups-20261109.bundle` | 16 `history/*` branches (`diversity/*` and `cw/*`: binaural, auto-diversity and bins, noise and RADE studies, the earlier CW side-tone and keyer branches), `archive/binaural-rade` (was `devtest/binaural-rade`, all of it is in `devtest/everything`) and 27 `backup/*` tags (the old tips from every rebase before 2026-11-09) |
| `pihpsdr-history-pipewire-upstream-20261030.bundle` | the 15 old pipewire, upstream-PR and TCI-doc branches (June to September 2026) |

To look at something, or to bring a branch back:

```
git bundle list-heads ~/sdr/archive/pihpsdr-history-backups-20261109.bundle
git fetch ~/sdr/archive/pihpsdr-history-backups-20261109.bundle \
    'refs/heads/history/diversity/binaural:refs/heads/history/diversity/binaural'
git switch --detach <tag>   # after fetching 'refs/tags/backup/*:refs/tags/backup/*'
```

Tags kept on `origin`: the releases (`v2.3` to `v3.1`, `3.0`, `Test-3.1`),
`pre-upstream-merge-*` and `noise-floor-eval-20261001`.

## Backups before a rebase

Before any rebase or force-push, tag the old tip as
`backup/<branch>-pre-rebase-<date>` and push it. When the rebase has
proved itself, bundle the tags into `~/sdr/archive/` and delete them, so
the tag list stays short.

## Rules

- A new experiment starts as `feature/<name>` or `devtest/<name>`. Nothing called `test` is created in this repository (no `test/` branches or tags: they
  collide with `TEST` on case-insensitive file systems, macOS and Windows). `devtest/` is for throwaway development code: it is not offered to anyone, may be
  rebased or rewritten at will, and is not a base for anything that is to last (that is a `feature/` or `fix/` branch). When a branch is finished or superseded it is
  bundled into `~/sdr/archive/`.
- Rebase onto `TEST` after `TEST` is re-synced with upstream. Tag the old
  tip first. A force-push to `origin` needs the owner's OK, and goes to
  `origin` only.
- Captures, props files, databases and patches are never committed or
  pushed (`wav/` and `captures/` are in `.gitignore`). The record of a
  capture is its entry in [test-findings.md](test-findings.md).
- The branch workflow in detail: [changes/git-workflow.md](archive/diversity/changes/git-workflow.md).
  The history of each re-sync: [changes/history.md](changes/history.md).
