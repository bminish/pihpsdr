# Branches

What each branch in this fork (`origin`, `bminish/pihpsdr`) is for, and
where old work goes. Written 2026-10-30. Upstream is `dl1ycf/pihpsdr`;
nothing is ever pushed there.

## Live branches

| Branch | What it is | Safe for others? |
|---|---|---|
| `master` | Follows upstream `master`. | Yes |
| `TEST` | `upstream/TEST` plus our local layer only: the capture recorder (`make DIVCAP=1`), the test harness and tools, and the docs. Every diversity change we proposed that upstream took is in it. | Yes. The latest tested diversity state. |
| `feature/diversity` | The same commit as `TEST`. The stable base to start diversity work from or to point others at. Moves only when `TEST` does. | Yes |
| `feature/diversity-binaural` | `TEST` plus the binaural work: the ear split (a left and a right ear from the two antennas), the shared AGC link, Balance ATT, the Link AGC menu row, and the RADE V2 captures. | **No, highly experimental.** Expect it to be rebased and its history rewritten. |
| `feature/rade-native` | Native RADE work, based on an older `TEST` (`d5154b91`, 37 commits behind). | No |
| `fix/diversity-flat-weighting-TEST` | PR #149 to upstream. Leave it alone while the PR is open. | PR branch |
| `fix/p2-unused-adc-bpf-bypass-master` | PR #140 to upstream. Leave it alone while the PR is open. | PR branch |

`TEST` tracks `upstream/TEST` locally, so `git pull` on it pulls from
upstream, not from `origin`.

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
`backup/<branch>-pre-rebase-<date>` (23 now). They are pushed to `origin`,
so they stay out of the branch list and are not lost. To look at one,
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
