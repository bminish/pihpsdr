#
# The binaural paired AGC against the built WDSP (wdsp/libwdsp.a).
#
#   make -C test/binaural run
#
TOP    = ../..
CFLAGS = -O2 -Wall -I$(TOP)/wdsp -I$(TOP)/libspecbleach/include
LIBS   = $(TOP)/wdsp/libwdsp.a $(TOP)/libspecbleach/libspecbleach.a \
         `pkg-config --libs fftw3 fftw3f` -lpthread -lm

all: test_agcpair

test_agcpair: test_agcpair.c $(TOP)/wdsp/libwdsp.a
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

run: test_agcpair
	./test_agcpair

clean:
	rm -f test_agcpair

.PHONY: all run clean
