# Harness for the native RADE work. Needs librade: tools/rade/build_rade.sh
ROOT  := ../..
RSRC  := $(ROOT)/third_party/rade_c-src/src
RBLD  := $(ROOT)/third_party/rade_c-build/src
OPUS  := $(ROOT)/third_party/rade_c-build/build_opus-prefix/src/build_opus
CFLAGS := -O2 -Wall -std=gnu11 -I$(RSRC) -I$(OPUS) -I$(OPUS)/include -I$(OPUS)/celt -I$(OPUS)/dnn
LDFLAGS := -L$(RBLD) -lrade -lm -Wl,-rpath,$(abspath $(RBLD))

all: rade_tx_iq

rade_tx_iq: rade_tx_iq.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f rade_tx_iq
