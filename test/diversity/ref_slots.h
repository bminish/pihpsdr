/*
 * The per-reference slots, moved in and out of the live settings the way
 * the menu does it.
 *
 * Upstream's f5a0ce9c moved this out of the engine and into
 * diversity_menu.c, as store_ref_values() and restore_ref_values(). The
 * menu is GTK dialog code and cannot be linked into these tools, so this
 * is a copy of the data half of those two functions. Keep it in step with
 * them: CW has its own slot, and RADE V1 has no window and a threshold
 * pinned at zero (LC-016).
 */
#ifndef REF_SLOTS_H
#define REF_SLOTS_H

#include "diversity_auto.h"

static inline void tool_ref_store(int ref) {
  switch (ref) {
  case DIV_REF_CARRIER:
    div_carrier_centre = div_auto_centre;
    div_carrier_width  = div_auto_width;
    div_carrier_cohmin = div_auto_coherence_min;
    break;

  case DIV_REF_BAND:
    div_band_centre = div_auto_centre;
    div_band_width  = div_auto_width;
    div_band_cohmin = div_auto_coherence_min;
    break;

  case DIV_REF_DIGITAL_IQ:
    div_digital_centre = div_auto_centre;
    div_digital_width  = div_auto_width;
    div_digital_cohmin = div_auto_coherence_min;
    break;

  case DIV_REF_CW:
    div_cw_centre = div_auto_centre;
    div_cw_width  = div_auto_width;
    div_cw_cohmin = div_auto_coherence_min;
    break;

  default:
    break;
  }
}

static inline void tool_ref_recall(int ref) {
  switch (ref) {
  case DIV_REF_CARRIER:
    div_auto_centre        = div_carrier_centre;
    div_auto_width         = div_carrier_width;
    div_auto_coherence_min = div_carrier_cohmin;
    break;

  case DIV_REF_BAND:
    div_auto_centre        = div_band_centre;
    div_auto_width         = div_band_width;
    div_auto_coherence_min = div_band_cohmin;
    break;

  case DIV_REF_DIGITAL_IQ:
    div_auto_centre        = div_digital_centre;
    div_auto_width         = div_digital_width;
    div_auto_coherence_min = div_digital_cohmin;
    break;

  case DIV_REF_CW:
    div_auto_centre        = div_cw_centre;
    div_auto_width         = div_cw_width;
    div_auto_coherence_min = div_cw_cohmin;
    break;

  case DIV_REF_RADE_V1:
    div_auto_coherence_min = 0.0;
    break;

  default:
    break;
  }
}

#endif
