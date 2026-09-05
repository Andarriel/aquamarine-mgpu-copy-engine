#!/usr/bin/env python3
# Fake X11 display :99 that accepts (abstract + filesystem socket) and never answers,
# mimicking Hyprland's lazily-started Xwayland socket during compositor init.
import socket, os, time, sys, select
n = 99
path = f"/tmp/.X11-unix/X{n}"
try:
    os.unlink(path)
except FileNotFoundError:
    pass
socks = []
a = socket.socket(socket.AF_UNIX); a.bind(b"\0" + path.encode()); a.listen(4); socks.append(a)
f = socket.socket(socket.AF_UNIX); f.bind(path); f.listen(4); socks.append(f)
conns = []
end = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 30)
while time.time() < end:
    r, _, _ = select.select(socks, [], [], 1)
    for s in r:
        c, _ = s.accept(); conns.append(c)
        print("X client connected via", "abstract" if s is a else "filesystem", "at", time.strftime("%T"), flush=True)
os.unlink(path)
print("server done", flush=True)
