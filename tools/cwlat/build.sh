#!/bin/sh
# Build cwlat (needs the ALSA and PipeWire development packages that piHPSDR itself needs).
cd "$(dirname "$0")" || exit 1
gcc -O2 -Wall -Wextra -Wno-unused-parameter -o cwlat cwlat.c \
    $(pkg-config --cflags --libs libpipewire-0.3) -lasound -lm -lpthread
