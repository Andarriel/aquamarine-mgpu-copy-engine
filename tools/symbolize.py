#!/usr/bin/env python3
"""Symbolize sampler output using /proc/pid/maps and nm -D / nm on the mapped ELF files."""
import bisect, collections, subprocess, sys, re, os

pid = int(sys.argv[1])
samples_file = sys.argv[2]
mode = sys.argv[3] if len(sys.argv) > 3 else "self"  # self | callers

maps = []
with open(f"/proc/{pid}/maps") as f:
    for line in f:
        parts = line.split()
        if len(parts) < 6 or 'x' not in parts[1]:
            continue
        lo, hi = (int(x, 16) for x in parts[0].split('-'))
        off = int(parts[2], 16)
        path = parts[5]
        maps.append((lo, hi, off, path))
maps.sort()
starts = [m[0] for m in maps]

symcache = {}
def load_syms(path):
    if path in symcache:
        return symcache[path]
    syms = []
    if os.path.exists(path):
        for flag in ("-D", ""):
            try:
                out = subprocess.run(["nm", "--defined-only", "-C"] + ([flag] if flag else []) + [path],
                                     capture_output=True, text=True).stdout
            except Exception:
                out = ""
            for line in out.splitlines():
                p = line.split(maxsplit=2)
                if len(p) == 3 and p[1].lower() in ("t", "w", "i"):
                    syms.append((int(p[0], 16), p[2]))
            if syms:
                break
    # also need load bias: for shared objs, the file offset of the first exec segment maps to vaddr;
    # use readelf to find program header of first LOAD with X flag.
    bias = 0
    try:
        out = subprocess.run(["readelf", "-lW", path], capture_output=True, text=True).stdout
        for line in out.splitlines():
            if line.strip().startswith("LOAD") and " E" in line[:80] or (line.strip().startswith("LOAD") and re.search(r"R.E", line)):
                p = line.split()
                # LOAD offset vaddr paddr filesz memsz flags align
                fileoff = int(p[1], 16); vaddr = int(p[2], 16)
                bias = vaddr - fileoff
                break
    except Exception:
        pass
    syms.sort()
    symcache[path] = (syms, [s[0] for s in syms], bias)
    return symcache[path]

def resolve(addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0 or addr >= maps[i][1]:
        return "?", "[unknown]"
    lo, hi, off, path = maps[i]
    fileaddr = addr - lo + off  # file offset
    syms, addrs, bias = load_syms(path)
    va = fileaddr + bias
    j = bisect.bisect_right(addrs, va) - 1
    name = syms[j][1] if j >= 0 and va - syms[j][0] < 0x200000 else f"+0x{va:x}"
    return os.path.basename(path), name

counts = collections.Counter()
libcounts = collections.Counter()
total = 0
with open(samples_file) as f:
    for line in f:
        if not line.startswith("S "):
            continue
        parts = line.split()
        tid = parts[1]
        ip = int(parts[2], 16)
        chain = [int(x, 16) for x in parts[3:]]
        total += 1
        if mode == "self":
            lib, name = resolve(ip)
            counts[(lib, name)] += 1
            libcounts[lib] += 1
        else:
            # inclusive: count every distinct frame in the chain once
            seen = set()
            for a in chain:
                if a >= (1 << 63):  # context markers
                    continue
                lib, name = resolve(a)
                key = (lib, name)
                if key in seen:
                    continue
                seen.add(key)
                counts[key] += 1

print(f"total samples: {total}")
if mode == "self":
    print("\n== by library ==")
    for lib, c in libcounts.most_common(15):
        print(f"{100*c/total:6.1f}%  {lib}")
print(f"\n== by function ({mode}) ==")
for (lib, name), c in counts.most_common(45):
    print(f"{100*c/total:6.1f}%  {lib:28s} {name[:110]}")
