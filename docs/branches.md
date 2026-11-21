# Branches

What each branch in this fork (`origin`, `bminish/pihpsdr`) is for, and
where old work goes. Written 2026-10-30, updated 2026-11-21. Upstream is `dl1ycf/pihpsdr`;
nothing is ever pushed there.

## Live branches

| Branch | What it is | Safe for others? |
|---|---|---|
| `master` | Follows upstream `master`. | Yes |
| `TEST` | `upstream/TEST` plus our local layer only: the capture recorder (`make DIVCAP=1`), the test harness and tools, and the docs. Every diversity change we proposed that upstream took is in it. | Yes. The latest tested diversity state. |
| `feature/diversity` | The same commit as `TEST`. The stable base to start diversity work from or to point others at. Moves only when `TEST` does. | Yes |
| `feature/diversity-binaural` | `TEST` plus the binaural work: the ear split (a left and a right ear from the two antennas), the shared AGC link, Balance ATT, the Link AGC menu row, and the RADE V2 captures; since 2026-11-21 also the CW side tone and tap fix (LC-064, LC-065) and LC-077 to LC-081. Rebased onto `TEST` 2026-11-21. Series and state: `docs/binaural/README.md`. | **No, highly experimental.** Expect it to be rebased and its history rewritten. |
| `feature/rade-native` | `TEST` plus the native RADE work (RADE V1 and V2 run inside piHPSDR through `librade`, the Reporter, the log viewer, the air tests) and the RADE V2 diversity reference (LC-069 to LC-076; it was LC-064 to LC-068 before the 2026-11-11 renumbering), with its V2 captures T-027 to T-030. Rebased onto `TEST` on 2026-11-21 (144 commits); `librade` is built separately (see `README.md`). Old tips: tags `backup/rade-native-pre-rebase-20261121` and `-pre-amend-rebase-20261121`. | No, experimental |
| `feature/synth-sidetone` | `TEST` plus the synthesised CW side tone (LC-064) and its tools (LT-039), as a patch series for review, including the optional PipeWire quantum-128 commit. Rebased onto `TEST` 2026-11-21. | Review copy |
| `fix/keyer-lost-first-tap` | One commit (LC-065): a short paddle tap is no longer lost while the keyer waits for TX. On an older `TEST`; also carried by `feature/diversity-binaural`. | Patch branch |
| `feature/cw-synth-sidetone` | The CW working line the side tone and keyer fixes came from (LC-064 to LC-066). | No; the CW work continues on `devtest/everything` |
| `devtest/everything` | The working branch with everything together: RADE, binaural, the CW series and tools. Holds the master register (`docs/changes.md` there). Was `devtest/sidetone`, which is deleted. | No, working branch |
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
numbers are shared and do not clash: LC-041 to LC-043 are binaural (LC-044
and LC-045 are accepted `TEST` changes), LC-064 to LC-066 the CW series,
LC-069 to LC-076 RADE, LC-077 to LC-081 binaural (2026-11-21). The next
free number is LC-082.

## Archive: `history/`

Finished, superseded or abandoned work is kept as `history/<topic>/<name>`.
Nothing there is maintained and nothing is rebased. Today's contents,
all under `history/diversity/`:

- `binaural`, `binaural-2`, `binaural-agc-link`, `binaural-ui-test`: earlier binaural lines
- `auto-diversity`, `auto-bins`, `autobins-renum`: the auto-diversity and bin work (upstream took it)
- `noise-correlator`, `noise-floor-wip`, `phasing-analysis`, `radeV1-eq`, `radev1-level`, `radeV2-correlator`, `rade-abandoned`: measurement and RADE studies
- `pr-menu-ref-row`: a PR branch with no PR open

A branch moves into `history/` with `git branch -m <old> history/<topic>/<new>`,
then a push of the new name, then deleting the old name on `origin`.

## Backups are tags, not branches

Before any rebase or force-push, the old tip is kept as a tag,
`backup/<branch>-pre-rebase-<date>` (or `-pre-amend-rebase-`). Those from
2026-11 on are pushed to `origin` as well; the older ones are in this clone
only. Tags stay out of the branch list. To look at one,
`git switch --detach backup/TEST-pre-rebase-20261030`.

## Bundled and removed from `origin`

The old pipewire, upstream-PR and TCI-doc branches (15, from June to
September 2026) are in
`~/sdr/archive/pihpsdr-history-pipewire-upstream-20261030.bundle`
and no longer on `origin` or in this clone. To bring them back:

```
git fetch ~/sdr/archive/pihpsdr-history-pipewire-upstream-20261030.bundle \
    'refs/heads/*:refs/heads/*'
```

## Rules

- A new experiment starts as `feature/<name>` or `test/<name>`. When it is
  finished or superseded it moves to `history/`.
- Rebase onto `TEST` after `TEST` is re-synced with upstream. Tag the old
  tip first. A force-push to `origin` needs the owner's OK, and goes to
  `origin` only.
- Captures, props files, databases and patches are never committed or
  pushed (`wav/` and `captures/` are in `.gitignore`). The record of a
  capture is its entry in [test-findings.md](test-findings.md).
- The branch workflow in detail: [changes/git-workflow.md](changes/git-workflow.md).
  The history of each re-sync: [changes/history.md](changes/history.md).
