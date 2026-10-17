##############################################################################
#
# Native RADE V1/V2 (feature/rade-native). Included by the Makefile after
# PROGRAM, OPTIONS and LIBS are defined and before OBJS is. The Makefile has
# three lines for it: the include, $(RADE_NATIVE_LIBS) in LIBS and
# $(RADE_NATIVE_OBJS) in OBJS. Needs librade, which tools/rade/build_rade.sh fetches and builds
# into third_party/. Off by default: make RADE_NATIVE=ON (or put it in
# make.config.pihpsdr). See docs/archive/rade/plan.md.
#
##############################################################################

RADE_NATIVE ?= OFF

# The rules below must not become the default goal: until the Makefile's own first rule is seen, that is the program.
# (PROGRAM is set further down the Makefile, after this include, so the name is written out;
# tools/rade/rade-patches.sh check tests that the default goal is still the program.)
ifeq ($(.DEFAULT_GOAL),)
.DEFAULT_GOAL := pihpsdr
endif

ifeq ($(RADE_NATIVE),ON)

RADE_SRC=./third_party/rade_c-src/src
RADE_BLD=./third_party/rade_c-build
RADE_OPUS=$(RADE_BLD)/build_opus-prefix/src/build_opus
RADE_NATIVE_INCLUDE=-I$(RADE_SRC) -I$(RADE_OPUS) -I$(RADE_OPUS)/include \
	-I$(RADE_OPUS)/celt -I$(RADE_OPUS)/dnn

# Fail early and clearly when librade has not been fetched and built.
ifneq ($(filter-out clean distclean,$(or $(MAKECMDGOALS),all)),)
ifneq ($(words $(wildcard $(RADE_SRC)/rade_api.h $(RADE_BLD)/src/librade.so)),2)
$(error RADE_NATIVE=ON needs librade: run tools/rade/build_rade.sh first)
endif
endif

OPTIONS += -D RADE_NATIVE
# librade carries its own opus. The Makefile puts $(RADE_NATIVE_LIBS) before the WDSP libraries, as it always has
# (the order matters: both carry an nnet).
RADE_NATIVE_LIBS=-L$(RADE_BLD)/src -lrade -Wl,-rpath,$(abspath $(RADE_BLD)/src) \
	$(RADE_OPUS)/.libs/libopus.a

RADE_NATIVE_OBJS = src/rade_native.o src/rade_native_menu.o src/rade_eoo_text.o src/rade_v2_text.o \
	src/rade_launch.o src/rade_native_log.o src/rade_native_test.o

src/rade_native.o: src/rade_native.h src/rade_eoo_text.h src/rade_v2_text.h src/rade_native_log.h
src/rade_native_log.o: src/rade_native_log.h
src/rade_native_test.o: src/rade_native_test.h
src/rade_eoo_text.o: src/rade_eoo_text.h src/rade_eoo_text_hra.h
src/rade_v2_text.o: src/rade_v2_text.h src/rade_eoo_text_hra.h
src/rade_native_menu.o: src/rade_native.h src/rade_native_menu.h src/rade_native_test.h src/rade_launch.h
src/rade_launch.o: src/rade_launch.h
src/receiver.o src/transmitter.o src/radio.o: src/rade_native.h
# librade's headers include a "nnet.h" of their own (opus), which clashes with
# wdsp/nnet.h. Only this object sees librade, with its own include path.
src/rade_native.o: INCLUDES=$(RADE_NATIVE_INCLUDE)

endif
