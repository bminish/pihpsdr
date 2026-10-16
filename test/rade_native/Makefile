# Harness for the native RADE work. Needs librade: tools/rade/build_rade.sh
ROOT  := ../..
RSRC  := $(ROOT)/third_party/rade_c-src/src
RBLD  := $(ROOT)/third_party/rade_c-build/src
OPUS  := $(ROOT)/third_party/rade_c-build/build_opus-prefix/src/build_opus
CFLAGS := -O2 -Wall -std=gnu11 -I$(RSRC) -I$(OPUS) -I$(OPUS)/include -I$(OPUS)/celt -I$(OPUS)/dnn
LDFLAGS := -L$(RBLD) -lrade -lm -Wl,-rpath,$(abspath $(RBLD))

all: rade_tx_iq rade_rx_iq rade_decode_iq wdsp_probe test_geometry test_log test_rx_engine test_tx_engine

rade_tx_iq: rade_tx_iq.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

WDSP := $(ROOT)/wdsp
rade_rx_iq: rade_rx_iq.c $(ROOT)/src/rade_eoo_text.c
	$(CC) $(CFLAGS) -I$(ROOT)/src -o $@ rade_rx_iq.c $(ROOT)/src/rade_eoo_text.c $(LDFLAGS)

rade_decode_iq: rade_decode_iq.c $(ROOT)/src/rade_eoo_text.c
	$(CC) $(CFLAGS) -I$(ROOT)/src -o $@ rade_decode_iq.c $(ROOT)/src/rade_eoo_text.c $(LDFLAGS) \
	  $(OPUS)/.libs/libopus.a

wdsp_probe: wdsp_probe.c $(WDSP)/libwdsp.a
	$(CC) -O2 -Wall -pthread -I$(WDSP) -o $@ $< $(WDSP)/libwdsp.a \
	  $(ROOT)/libspecbleach/libspecbleach.a \
	  `pkg-config --libs fftw3` `pkg-config --libs fftw3f` -lm

test_geometry: test_geometry.c $(ROOT)/src/rade_native.c $(ROOT)/src/rade_native_log.c $(ROOT)/src/rade_eoo_text.c $(ROOT)/src/rade_native.h
	$(CC) $(CFLAGS) -pthread -I$(ROOT)/src -o $@ test_geometry.c $(ROOT)/src/rade_native.c $(ROOT)/src/rade_native_log.c $(ROOT)/src/rade_eoo_text.c \
	  $(LDFLAGS) $(OPUS)/.libs/libopus.a

test_rx_engine: test_rx_engine.c $(ROOT)/src/rade_native.c $(ROOT)/src/rade_native_log.c $(ROOT)/src/rade_eoo_text.c $(ROOT)/src/rade_native.h
	$(CC) $(CFLAGS) -pthread -I$(ROOT)/src -o $@ test_rx_engine.c $(ROOT)/src/rade_native.c $(ROOT)/src/rade_native_log.c $(ROOT)/src/rade_eoo_text.c \
	  $(LDFLAGS) $(OPUS)/.libs/libopus.a

test_tx_engine: test_tx_engine.c $(ROOT)/src/rade_native.c $(ROOT)/src/rade_native_log.c $(ROOT)/src/rade_eoo_text.c $(ROOT)/src/rade_native.h
	$(CC) $(CFLAGS) -pthread -I$(ROOT)/src -o $@ test_tx_engine.c $(ROOT)/src/rade_native.c $(ROOT)/src/rade_native_log.c $(ROOT)/src/rade_eoo_text.c \
	  $(LDFLAGS) $(OPUS)/.libs/libopus.a

# interoperability of src/rade_eoo_text.c with freedv-gui's own V1 text code,
# built unmodified from a freedv-gui checkout (FREEDV_SRC = its src/ directory)
FREEDV_SRC ?= $(HOME)/sdr/freedv-gui/build_linux/_deps/freedv_backend-src/src
text_interop/interop: text_interop/interop.cpp $(ROOT)/src/rade_eoo_text.c
	g++ -O1 -std=c++17 -I$(ROOT)/src -I$(FREEDV_SRC) -I$(FREEDV_SRC)/pipeline -I$(RSRC) -I$(OPUS) \
	  -I$(OPUS)/include -I$(OPUS)/celt -I$(OPUS)/dnn -x c++ text_interop/interop.cpp \
	  $(FREEDV_SRC)/pipeline/rade_text.cpp $(FREEDV_SRC)/pipeline/ldpc_encode.cpp \
	  $(FREEDV_SRC)/pipeline/ldpc_decode.cpp -x c $(ROOT)/src/rade_eoo_text.c -o $@

text_interop: text_interop/interop
	./text_interop/interop

test_tx_level: test_tx_level.c $(ROOT)/src/rade_native.c $(ROOT)/src/rade_native_log.c $(ROOT)/src/rade_eoo_text.c $(ROOT)/src/rade_native.h
	$(CC) $(CFLAGS) -pthread -I$(ROOT)/src -o $@ test_tx_level.c $(ROOT)/src/rade_native.c $(ROOT)/src/rade_native_log.c $(ROOT)/src/rade_eoo_text.c \
	  $(LDFLAGS) $(OPUS)/.libs/libopus.a

test_launch: test_launch.c $(ROOT)/src/rade_launch.c $(ROOT)/src/rade_launch.h
	$(CC) $(CFLAGS) -I$(ROOT)/src -o $@ test_launch.c $(ROOT)/src/rade_launch.c `pkg-config --cflags --libs glib-2.0`

modes_table: modes_table.c $(ROOT)/src/rade_native.c $(ROOT)/src/rade_native_log.c $(ROOT)/src/rade_eoo_text.c $(ROOT)/src/rade_native.h
	$(CC) $(CFLAGS) -pthread -I$(ROOT)/src -o $@ modes_table.c $(ROOT)/src/rade_native.c $(ROOT)/src/rade_native_log.c $(ROOT)/src/rade_eoo_text.c \
	  $(LDFLAGS) $(OPUS)/.libs/libopus.a

test_ctl: test_ctl.c $(ROOT)/src/rade_native_test.c $(ROOT)/src/rade_native_test.h
	$(CC) $(CFLAGS) -pthread -I$(ROOT)/src -o $@ test_ctl.c $(ROOT)/src/rade_native_test.c `pkg-config --cflags --libs glib-2.0` -lm

test_log: test_log.c $(ROOT)/src/rade_native_log.c $(ROOT)/src/rade_native_log.h
	$(CC) $(CFLAGS) -pthread -I$(ROOT)/src -o $@ test_log.c $(ROOT)/src/rade_native_log.c -lm

check: test_geometry test_log test_ctl
	./test_geometry
	./test_log
	python3 test_ctl.py

clean:
	rm -f modes_table test_launch test_tx_level rade_decode_iq text_interop/interop test_tx_engine test_rx_engine rade_tx_iq rade_rx_iq wdsp_probe test_geometry test_log test_ctl
