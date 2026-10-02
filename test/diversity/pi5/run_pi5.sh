#!/bin/bash
#
# Diversity engine CPU benchmark for a Raspberry Pi 5 (or any machine).
#
# Unpack, then run from the unpacked directory:
#
#   tar xzf pi5-bench.tar.gz && cd pi5-bench && ./run_pi5.sh
#
# Takes about 15 minutes. Close piHPSDR first: anything else using the CPU
# moves the figures. Leaves results-<host>-<time>.tar.gz here; copy that
# back.
#
# Needs a C compiler, pkg-config, and the FFTW single-precision and GTK 3
# development packages (the ones piHPSDR itself builds with):
#
#   sudo apt install build-essential pkg-config libfftw3-dev libgtk-3-dev
#
# perf is optional. With it, a profile says which functions the time goes
# to:
#
#   sudo apt install linux-perf
#
# Nothing here changes the system: the CPU governor and clocks are
# recorded, not set, so the figures are for the Pi as it runs the radio.
#

set -u
cd "$(dirname "$0")"
OUT="results-$(hostname)-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
LOG="$OUT/run.log"

say() { echo "$*" | tee -a "$LOG"; }

# ---- Dependencies --------------------------------------------------------

missing=""
command -v cc >/dev/null || missing="$missing build-essential"
command -v pkg-config >/dev/null || missing="$missing pkg-config"
pkg-config --exists fftw3f 2>/dev/null || missing="$missing libfftw3-dev"
pkg-config --exists gtk+-3.0 2>/dev/null || missing="$missing libgtk-3-dev"

if [ -n "$missing" ]; then
  echo "Missing:$missing"
  echo "Install with:  sudo apt install$missing"
  exit 2
fi

if pgrep -x pihpsdr >/dev/null; then
  echo "piHPSDR is running. Close it first: it would load the CPU under the benchmark."
  exit 2
fi

# ---- The machine, as it stands ---------------------------------------------

state() {   # $1: label
  {
    echo "== $1, $(date '+%F %T')"
    echo "loadavg: $(cat /proc/loadavg)"
    for c in /sys/devices/system/cpu/cpu[0-9]*/cpufreq; do
      [ -d "$c" ] || continue
      echo "$(basename "$(dirname "$c")"): governor $(cat "$c/scaling_governor" 2>/dev/null)" \
           "cur $(cat "$c/scaling_cur_freq" 2>/dev/null) kHz" \
           "min $(cat "$c/scaling_min_freq" 2>/dev/null) max $(cat "$c/scaling_max_freq" 2>/dev/null)"
    done
    if command -v vcgencmd >/dev/null; then
      echo "vcgencmd: $(vcgencmd measure_temp) $(vcgencmd get_throttled) $(vcgencmd measure_clock arm)"
    elif [ -r /sys/class/thermal/thermal_zone0/temp ]; then
      echo "temp: $(( $(cat /sys/class/thermal/thermal_zone0/temp) / 1000 )) C"
    fi
    echo
  } >> "$OUT/system.txt"
}

{
  echo "bundle: $(cat VERSION 2>/dev/null)"
  if [ -r /proc/device-tree/model ]; then
    echo "model: $(tr -d '\0' < /proc/device-tree/model)"
  else
    echo "model: $(grep -m1 'model name' /proc/cpuinfo)"
  fi
  echo "kernel: $(uname -a)"
  grep PRETTY_NAME /etc/os-release 2>/dev/null
  echo "compiler: $(cc --version | head -1)"
  echo "fftw3f: $(pkg-config --modversion fftw3f)   gtk+-3.0: $(pkg-config --modversion gtk+-3.0)"
  echo
  lscpu 2>/dev/null
  echo
  free -m
  echo
  echo "busiest processes:"
  ps -eo pcpu,pmem,comm --sort=-pcpu | head -12
  echo
} > "$OUT/system.txt"
state "before"

# ---- Build -----------------------------------------------------------------

#
# -O3 as the radio's Makefile builds; -g only adds symbols for perf and
# does not change the code.
#
CFLAGS="-O3 -g -Wall -Wno-unused-parameter"
say "building..."
cc $CFLAGS -DWITH_FFTW -o pi_bench pi_bench_standalone.c -lfftw3f -lm 2>> "$LOG" \
  || { say "pi_bench did not build; see $LOG"; exit 1; }
cc $CFLAGS -Isrc $(pkg-config --cflags gtk+-3.0 fftw3f) -o bench_cpu bench_cpu.c \
  src/diversity_auto.c src/rade_correlator.c $(pkg-config --libs gtk+-3.0 fftw3f) -lm 2>> "$LOG" \
  || { say "bench_cpu did not build; see $LOG"; exit 1; }

# ---- Run -------------------------------------------------------------------

say "1/4 pi_bench: kernels, FFT planners, decimators (about 5 minutes)"
./pi_bench > "$OUT/pi_bench.txt" 2>&1
say "    exit $? (1 means a selection or decimator check failed)"
state "after pi_bench"

say "2/4 bench_cpu, paced as the radio (about 5 minutes)"
./bench_cpu > "$OUT/bench_cpu-paced.txt" 2>&1
state "after bench_cpu paced"

say "3/4 bench_cpu, hot (about 2 minutes)"
./bench_cpu --hot > "$OUT/bench_cpu-hot.txt" 2>&1
state "after bench_cpu hot"

if command -v perf >/dev/null; then
  say "4/4 perf profile of bench_cpu --hot --quick (about 1 minute)"
  if perf record -F 999 -o "$OUT/perf.data" ./bench_cpu --hot --quick > /dev/null 2>> "$OUT/perf.log"; then
    #
    # The analysis thread alone (named div_auto), then everything: the
    # harness generates its signals inside the profile, so the whole
    # report also carries random() and sincos() that the radio never
    # runs. diversity_auto_sample() is the feeder, as on the receive
    # thread.
    #
    perf report -i "$OUT/perf.data" --stdio --no-children --comms div_auto \
      --sort dso,symbol --percent-limit 0.3 > "$OUT/perf-worker.txt" 2>> "$OUT/perf.log"
    perf report -i "$OUT/perf.data" --stdio --no-children --sort comm,dso,symbol \
      --percent-limit 0.3 > "$OUT/perf-all.txt" 2>> "$OUT/perf.log"
    rm -f "$OUT/perf.data"
  else
    say "    perf record failed (see perf.log; perf_event_paranoid is $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null))"
  fi
else
  say "4/4 perf not installed: skipped (optional)"
fi
state "after"

tar czf "$OUT.tar.gz" "$OUT"
say ""
say "Done. Copy back: $(pwd)/$OUT.tar.gz"
