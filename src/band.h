/* Copyright (C)
* 2015 - John Melton, G0ORX/N6LYT
* 2025 - Christoph van Wüllen, DL1YCF
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/

#ifndef _BAND_H_
#define _BAND_H_

#include <gtk/gtk.h>
#include "bandstack.h"

enum _band_enum {
  band136 = 0,
  band472,
  band160,
  band80,
  band60,
  band40,
  band30,
  band20,
  band17,
  band15,
  band12,
  band10,
  band6,
  band70,
  band144,
  band220,
  band430,
  band902,
  band1240,
  band2300,
  band3400,
  bandAIR,
  bandWWV,
  bandGen,
  BANDS
};

#define XVTRS 10

/* --------------------------------------------------------------------------*/
/**
* @brief Band definition
*/
struct _BAND {
  char title[16];                 // band title
  BANDSTACK *bandstack;           // pointer to band stack
  //
  // Data that can be changed via menus etc.
  //
  unsigned char OCrx;             // OC bit pattern for RX
  unsigned char OCtx;             // OC bit pattern for TX
  int gaincalib;                  // band dependent RX gain offset
  int RxAntenna;                  // (ALEX) RX antenna
  int TxAntenna;                  // (ALEX) TX antenna
  double pa_calibration;          // PA calibration value for this band
  long long frequencyMin;         // lower band edge
  long long frequencyMax;         // upper band edge
  long long frequencyLO;          // frequency offset
  long long errorLO;              // band dependent LO frequency correction
  int disablePA;                  // if 1, PA is disabled for this band
  //
  // Data that is automatically stored here, and applied whenever
  // a band change is done
  //
  double RFgain;                  // position of the RF gain slider
  double AGCgain;                 // position of the AGC gain slider
  int preamp;                     // This is to support C25 attenuation storage
  int dither;                     // This is to support C25 attenuation storage
  int attenuation;                // position of the ATT slider
  //
  // The two ADC step attenuators as DIVERSITY last had them on this band,
  // with the pair split (see div_indep_att). Kept per band because what
  // they correct is per band: the second antenna is hotter than the first
  // by an amount that is a property of the two antennas on that band, so
  // an antenna that needs 14 dB on 160 m needs it again the next time the
  // operator goes there. div_att_valid is 0 until diversity has actually
  // been run with the pair split here, so that a band never visited in
  // diversity does not start by asserting 0/0 over the operator's ATT.
  //
  int div_att[2];
  int div_att_valid;
  int alexAttenuation;            // if ALEX: attenuator (0/1/2/3 for 0/10/20/30 dB)
  int panlow;                     // panadapter settings
  int panhigh;
  int panstep;
};

//
// Note that several entries are compile-time constants for non-XVTR bands,
// that is, there is no GUI to change then, and they are not read from the
// props file:
//
// title, frequencyMin, frequencyMax, frequencyLO, errorLO, disablePA, gain
//

typedef struct _BAND BAND;

struct _CHANNEL {
  long long frequency;
  long long width;
};

typedef struct _CHANNEL CHANNEL;

#define UK_CHANNEL_ENTRIES 11
#define OTHER_CHANNEL_ENTRIES 5
#define WRC15_CHANNEL_ENTRIES 1

extern int channel_entries;
extern CHANNEL *band_channels_60m;

//extern CHANNEL band_channels_60m_UK[UK_CHANNEL_ENTRIES];
//extern CHANNEL band_channels_60m_OTHER[OTHER_CHANNEL_ENTRIES];
//extern CHANNEL band_channels_60m_WRC15[WRC15_CHANNEL_ENTRIES];

extern BAND *band_get_band(int b);
extern int get_band_from_frequency(long long f);

extern BANDSTACK *bandstack_get_bandstack(int band);

extern void radio_change_region(int region);

extern void band_save_state(void);
extern void band_restore_state(void);

char* getFrequencyInfo(long long frequency, int filter_low, int filter_high);
int TransmitAllowed(void);

extern void band_minus(int id);
extern void band_plus(int id);
#endif
