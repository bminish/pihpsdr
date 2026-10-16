#!/usr/bin/env python3
"""A Reporter left running from before an update (it outlives piHPSDR) is replaced by the new one, not merely shown; one running
the same code is only asked to show itself."""
import fcntl, os, signal, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
scratch = tempfile.mkdtemp()
env = dict(os.environ, XDG_STATE_HOME=scratch + "/state", XDG_CONFIG_HOME=scratch + "/config")
os.makedirs(scratch + "/state/pihpsdr-rade")
fails = 0
def check(c, what):
    global fails
    print("  %-4s %s" % ("ok" if c else "FAIL", what)); fails += (not c)

holder = """
import fcntl, os, signal, sys, time
f = open(os.environ['XDG_STATE_HOME'] + '/pihpsdr-rade/reporter.lock', 'a+')
fcntl.flock(f, fcntl.LOCK_EX)
f.seek(0); f.truncate(); f.write('%d %s' % (os.getpid(), sys.argv[1])); f.flush()
signal.signal(signal.SIGUSR1, lambda *a: open(os.environ['XDG_STATE_HOME'] + '/shown', 'w').write('shown'))
while True: time.sleep(0.1)
"""
probe = """
import sys
sys.path.insert(0, %r)
import rade_reporter as rr
f = rr.single_instance()
print('GOT' if f is not None else 'REFUSED', rr.CODE_ID)
""" % os.path.join(HERE, "..", "..", "tools", "rade-reporter")

def start_holder(code):
    p = subprocess.Popen([sys.executable, "-c", holder, code], env=env)
    time.sleep(0.6)
    return p

print("an older Reporter is running")
old = start_holder("0ldc0de0")
r = subprocess.run([sys.executable, "-c", probe], env=env, capture_output=True, text=True, timeout=30)
check(r.stdout.startswith("GOT"), "the new one takes over: %s" % r.stdout.strip())
time.sleep(0.3)
check(old.poll() is not None, "the old one was stopped")
print("one with the same code is running")
code = r.stdout.split()[1]
same = start_holder(code)
r = subprocess.run([sys.executable, "-c", probe], env=env, capture_output=True, text=True, timeout=30)
time.sleep(0.4)
check(r.stdout.startswith("REFUSED") and same.poll() is None and os.path.exists(scratch + "/state/shown"), "it is asked to show itself and left running")
same.kill()
print("FAILED %d" % fails if fails else "all passed")
sys.exit(1 if fails else 0)
