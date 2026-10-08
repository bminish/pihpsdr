#!/bin/bash
#
# Diversity engine CPU benchmark for a Raspberry Pi 5 (or any machine).
#
# Unpack, then run from the unpacked directory:
#
#   tar xzf pi5-bench.tar.gz && cd pi5-bench && ./run_pi5.sh
#
# Takes about 25 minutes. Close piHPSDR first: anything else using the CPU
# moves the figures. Leaves results-<host>-<time>.tar.gz here; copy that
# back.
#
# What it does: times the engine's kernels, transforms and decimators
# (pi_bench), times every reference and objective at every rate, the
# engine's start / stop / restart, and the receive thread's combine
# (bench_cpu), runs the whole functional suite on this machine, and then
# compares the figures with the 2026-10-02 Pi 5 run in baseline/ and says
# whether anything regressed, dropped blocks, or became a bottleneck
# (compare.txt; the last line is the verdict).
#
# Needs make, a C compiler, pkg-config, and the FFTW, GTK 3 and Opus
# development packages (the ones piHPSDR itself builds with; the suite's
# test_props includes client_server.h, which wants opus/opus.h). python3 is optional: it
# runs the comparison.
#
#   sudo apt install build-essential make pkg-config libfftw3-dev libgtk-3-dev libopus-dev python3
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
command -v make >/dev/null || missing="$missing make"
pkg-config --exists fftw3f 2>/dev/null || missing="$missing libfftw3-dev"
pkg-config --exists gtk+-3.0 2>/dev/null || missing="$missing libgtk-3-dev"
pkg-config --exists opus 2>/dev/null || missing="$missing libopus-dev"

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

say "1/6 pi_bench: kernels, FFT planners, decimators, the combine (about 5 minutes)"
./pi_bench > "$OUT/pi_bench.txt" 2>&1
say "    exit $? (1 means a selection or decimator check failed)"
state "after pi_bench"

say "2/6 bench_cpu, paced as the radio, with start / stop / restart (about 8 minutes)"
./bench_cpu > "$OUT/bench_cpu-paced.txt" 2>&1
state "after bench_cpu paced"

say "3/6 bench_cpu, hot (about 3 minutes)"
./bench_cpu --hot > "$OUT/bench_cpu-hot.txt" 2>&1
state "after bench_cpu hot"

if command -v perf >/dev/null; then
  say "4/6 perf profile of bench_cpu --hot --quick (about 1 minute)"
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
  say "4/6 perf not installed: skipped (optional)"
fi
state "after perf"

say "5/6 the functional suite, on this machine (about 3 minutes)"
if make -C test/diversity run > "$OUT/suite.txt" 2>&1; then
  say "    PASS (exit 0). Known gaps are reported in suite.txt and are not counted."
  suite=0
else
  say "    FAILED (exit $?): see $OUT/suite.txt"
  suite=1
fi
grep -c ' FAIL' "$OUT/suite.txt" | sed 's/^/    lines with FAIL (known gaps among them): /' | tee -a "$LOG"
state "after suite"

say "6/6 compare with the 2026-10-02 Pi 5 run"
if command -v python3 >/dev/null; then
  python3 compare_pi5.py baseline "$OUT" > "$OUT/compare.txt" 2>&1
  say "    $(tail -1 "$OUT/compare.txt")"
  say "    (details: $OUT/compare.txt)"
else
  say "    python3 not installed: skipped. Copy the results back and compare there."
fi
state "after"

tar czf "$OUT.tar.gz" "$OUT"
say ""
say "Done. Copy back: $(pwd)/$OUT.tar.gz"
