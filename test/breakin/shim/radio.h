#pragma once
// Host test shim: the settings and calls iambic.c uses.
enum { KEYER_STRAIGHT = 0, KEYER_MODE_A, KEYER_MODE_B };
extern volatile int mox;
extern int cw_keyer_mode, cw_breakin, cw_keyer_hang_time, cw_keyer_speed, cw_keyer_weight;
extern int cw_keys_reversed, cw_keyer_spacing, cw_keyer_internal, CAT_cw_is_active;
extern double cw_ramp_width;
void radio_set_mox(int state);
