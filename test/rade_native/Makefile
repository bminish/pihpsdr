# Harness for the native RADE work. Needs librade: tools/rade/build_rade.sh
ROOT  := ../..
RSRC  := $(ROOT)/third_party/rade_c-src/src
RBLD  := $(ROOT)/third_party/rade_c-build/src
OPUS  := $(ROOT)/third_party/rade_c-build/build_opus-prefix/src/build_opus
CFLAGS := -O2 -Wall -std=gnu11 -I$(RSRC) -I$(OPUS) -I$(OPUS)/include -I$(OPUS)/celt -I$(OPUS)/dnn
LDFLAGS := -L$(RBLD) -lrade -lm -Wl,-rpath,$(abspath $(RBLD))

all: rade_tx_iq rade_rx_iq wdsp_probe test_geometry

rade_tx_iq: rade_tx_iq.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

WDSP := $(ROOT)/wdsp
rade_rx_iq: rade_rx_iq.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

wdsp_probe: wdsp_probe.c $(WDSP)/libwdsp.a
	$(CC) -O2 -Wall -pthread -I$(WDSP) -o $@ $< $(WDSP)/libwdsp.a \
	  $(ROOT)/libspecbleach/libspecbleach.a \
	  `pkg-config --libs fftw3` `pkg-config --libs fftw3f` -lm

test_geometry: test_geometry.c $(ROOT)/src/rade_native.c $(ROOT)/src/rade_native.h
	$(CC) $(CFLAGS) -I$(ROOT)/src -o $@ test_geometry.c $(ROOT)/src/rade_native.c $(LDFLAGS)

check: test_geometry
	./test_geometry

clean:
	rm -f rade_tx_iq rade_rx_iq wdsp_probe test_geometry
