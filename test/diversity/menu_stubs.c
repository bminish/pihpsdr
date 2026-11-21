//
// Stubs for the diversity menu functions the engine calls. The harness links
// diversity_auto.c without diversity_menu.c (which needs the whole GTK menu
// and radio state), so anything the engine calls into the menu ends here.
//
// diversity_menu_nudge(): called from diversity_auto_rade_waveform_changed()
// when RADE is entered or its waveform changes. No test drives that path, so
// a no-op is enough to link; a test that does should replace this.
//
#include "diversity_menu.h"

void diversity_menu_nudge(int ref, double tau) { (void)ref; (void)tau; }
