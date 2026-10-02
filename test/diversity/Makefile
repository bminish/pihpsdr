#
# Standalone checks for the diversity auto-phasing engine.
#
#   make -C test/diversity run
#
# test_modes_live drives the real engine, with the real RECEIVER and VFO
# types, and asserts that every reference mode actually produces a weight.
# It exists because "this mode silently never starts" is the failure this
# code has produced more than once - most recently the carrier reference,
# whose bin range was computed before the transform its carrier tracker
# needed, so it could never become valid and sat on "searching" for ever.
#
TOP     = ../..
#
# No audio-backend define is needed: receiver.h only conditions on
# PORTAUDIO/ALSA/PULSEAUDIO for its buffer types, none of which this test
# touches. Passing -DPIPEWIRE here made the test unbuildable on an ALSA or
# PortAudio machine, which rather defeats the point of a regression test.
#
CFLAGS  = -O2 -Wall -I$(TOP)/src -I$(TOP)/wdsp \
          `pkg-config --cflags gtk+-3.0 fftw3f`
LIBS    = `pkg-config --libs gtk+-3.0 fftw3f` -lm
SRC     = $(TOP)/src/diversity_auto.c $(TOP)/src/rade_correlator.c

all: test_modes_live test_window test_rade test_digital test_props test_modal test_cw test_rates bench_cpu bench_nf

test_modes_live: test_modes_live.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(SRC) $(LIBS)

test_window: test_window.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(SRC) $(LIBS)

test_rade: test_rade.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(SRC) $(LIBS)

test_digital: test_digital.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(SRC) $(LIBS)

test_props: test_props.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(SRC) $(LIBS)

test_modal: test_modal.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(SRC) $(LIBS)

test_cw: test_cw.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(SRC) $(LIBS)

#
# Builds its test signals with a double-precision inverse FFT, so it also
# needs fftw3 as well as the engine's fftw3f.
#
test_rates: test_rates.c $(SRC)
	$(CC) $(CFLAGS) `pkg-config --cflags fftw3` -o $@ $< $(SRC) $(LIBS) `pkg-config --libs fftw3`

bench_cpu: bench_cpu.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(SRC) $(LIBS)

#
# The noise floor's selection against the qsort it replaced. Uses the
# engine's own div_nf_select() and div_nf_band_mean(), cut out of the
# source, so it cannot drift from what the radio runs.
#
nf_select.inc: $(TOP)/src/diversity_auto.c
	sed -n -e '/^static void div_nf_select(/,/^}/p' -e '/^static double div_nf_band_mean(/,/^}/p' $< > $@
	@grep -q div_nf_band_mean $@ || { echo "div_nf_band_mean not found in $<"; rm -f $@; exit 1; }

bench_nf: bench_nf.c nf_select.inc
	$(CC) -O2 -Wall -o $@ $< -lm

#
# One file to copy to a Raspberry Pi (or anywhere) and build with only a
# C compiler: pi_bench.c with the engine's selection code spliced in.
# See the head of pi_bench.c.
#
pi_bench_standalone.c: pi_bench.c nf_select.inc
	sed -e '/^#include "nf_select.inc"/{r nf_select.inc' -e 'd}' pi_bench.c > $@

#
# The Pi 5 bundle: the engine's sources, bench_cpu, pi_bench and the run
# script, in one tarball to copy to the Pi. The sources are found by the
# compiler's own dependency list, so the bundle follows the engine.
#
#   make -C test/diversity pi5-bench.tar.gz
#   scp test/diversity/pi5-bench.tar.gz pi@<your-pi>:
#
pi5-bench.tar.gz: bench_cpu.c pi_bench_standalone.c pi5/run_pi5.sh $(SRC)
	rm -rf pi5-bench && mkdir -p pi5-bench/src
	cp bench_cpu.c pi_bench_standalone.c pi5/run_pi5.sh pi5-bench/
	$(CC) $(CFLAGS) -MM bench_cpu.c $(SRC) | tr ' \\' '\n\n' | grep '^$(TOP)/src/' | sort -u \
	  | xargs -I{} cp {} pi5-bench/src/
	git -C $(TOP) describe --always --dirty > pi5-bench/VERSION
	tar czf $@ pi5-bench
	rm -rf pi5-bench

run: test_modes_live test_window test_rade test_digital test_props test_modal test_cw test_rates
	./test_props
	@echo
	./test_modal
	@echo
	./test_modes_live
	@echo
	./test_window
	@echo
	./test_rade
	@echo
	./test_digital
	@echo
	./test_cw
	@echo
	./test_rates

bench: bench_cpu bench_nf
	./bench_cpu
	./bench_nf

clean:
	rm -f test_modes_live test_window test_rade test_digital test_props test_modal test_cw test_rates bench_cpu bench_nf nf_select.inc pi_bench_standalone.c pi5-bench.tar.gz
