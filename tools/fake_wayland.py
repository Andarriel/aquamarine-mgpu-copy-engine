#!/usr/bin/env python3
# A Wayland socket that accepts connections and never answers, to reproduce
# "driver connects to WAYLAND_DISPLAY from inside the compositor and deadlocks".
import socket, os, time, sys
p = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/run/user/1000"), "fake-wl-0")
try:
    os.unlink(p)
except FileNotFoundError:
    pass
s = socket.socket(socket.AF_UNIX)
s.bind(p)
s.listen(4)
s.settimeout(1)
conns = []
end = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 30)
while time.time() < end:
    try:
        c, _ = s.accept()
        conns.append(c)
        print("client connected at", time.strftime("%T"), flush=True)
    except socket.timeout:
        pass
os.unlink(p)
print("server done", flush=True)
