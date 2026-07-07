#ifndef _RADAE_HANDLER_H_
#define _RADAE_HANDLER_H_

#include "radio.h"

extern int rx_rade_snr;
extern int rx_rade_sync;
extern float rx_rade_foff;
extern char decoded_callsign[16];

extern int rade_iq_toggle; // I/Q Q/I manual toggle (0 = normal, 1 = inverted)

void radae_init(void);
void radae_cleanup(void);

void radae_rx_start(void);
void radae_rx_stop(void);
void radae_process_rx_iq(RECEIVER *rx, double *iq_in, int count);

void radae_tx_start(void);
void radae_tx_stop(void);
void radae_tx_start_eoo(void);
int radae_tx_is_eoo_pending(void);
int radae_tx_active(void);
void radae_process_tx_audio(TRANSMITTER *tx, double *audio_buffer, int count);

#endif
