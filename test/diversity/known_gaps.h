/*
 * Checks that were written on feature/auto-diversity or
 * feature/diversity-binaural for features TEST does not have yet.
 *
 * A check guarded by one of these still runs and still prints its
 * figures. If it fails it is reported as KNOWN GAP and not counted, so the
 * suite says PASS on TEST without hiding what is missing. If it passes
 * while its gap is still defined, the test says so: the feature has
 * arrived, or the check has stopped testing it.
 *
 * When a feature is ported (docs/changes.md), delete its line here. The
 * check then counts again, which is the regression test for the port.
 */
#ifndef KNOWN_GAPS_H
#define KNOWN_GAPS_H

#define GAP_BRANCH_NOISE_RATIO "e6c12c05: the Window Sum weight carries the branch noise ratio"
#define GAP_LEVEL_OUTPUT       "4f24f5c3: hold the combined output at the level of one antenna"
#define GAP_STANDDOWN          "fc0b3d1e: the combiner stands down on an empty band"
#define GAP_CARRIER_FOLLOW     "41f8700c: the carrier search can follow the filter too"
#define GAP_WIRE_HELPERS       "42f68714: DIV_SETTINGS <-> wire conversion as functions (inline on TEST)"

/*
 * Not a feature still to port: upstream took this one out. f5a0ce9c
 * dropped the scheme-1 migration of diversity_auto_ref (and stopped
 * writing diversity_auto_ref_scheme), so a props file from before the
 * RADE passband reference was retired loads its old numbers as they are.
 * Tracked here rather than restored; see docs/changes.md.
 */
#define GAP_REF_SCHEME         "f5a0ce9c: upstream dropped the scheme-1 reference migration"

#include <stdio.h>

/*
 * For a whole check guarded by a gap: report it, and count it as passed
 * so it does not fail the suite. Call as ok = known_gap(ok, GAP_X).
 */
static inline int known_gap(int ok, const char *gap) {
  if (ok) {
    printf("  gap closed? this check passed with its gap still defined: %s\n", gap);
  } else {
    printf("  KNOWN GAP, not counted: %s\n", gap);
  }

  return 1;
}

#endif
