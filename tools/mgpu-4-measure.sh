#!/bin/bash
# Run this INSIDE the Hyprland session (in a terminal there), while music/visualiser is running:
#   bash ~/mgpu-4-measure.sh
# Samples Hyprland CPU for ~30 s and writes results to ~/Experiments/mgpu/diag/measure-<time>/
P=$(pgrep -x Hyprland | head -1)
D=$HOME/Experiments/mgpu/diag/measure-$(date +%H%M%S)
mkdir -p "$D"
T=$HOME/Experiments/mgpu/tools
{
  echo "lib: $(md5sum /usr/lib/libaquamarine.so.0.15.0)"
  echo "Hyprland pid $P, uptime $(ps -o etimes= -p $P)s"
  echo "clients: $(hyprctl clients -j | python3 -c 'import json,sys; print([c["class"] for c in json.load(sys.stdin)])')"
  echo "--- top (5 samples, 2s apart):"
  top -b -n 5 -d 2 -p "$P" | grep Hyprland
  echo "--- utime/stime over 10 s:"
  awk '{print "before utime", $14, "stime", $15}' /proc/$P/stat
  sleep 10
  awk '{print "after  utime", $14, "stime", $15}' /proc/$P/stat
  echo "--- irq rates over 5 s:"
  grep -E "i915|nvidia" /proc/interrupts | awk '{print $1, $(NF)}' | tr '\n' ' '; echo
  A=$(grep -E "i915|nvidia" /proc/interrupts | awk '{s=0; for(i=2;i<=NF-4;i++) s+=$i; print $NF, s}')
  sleep 5
  B=$(grep -E "i915|nvidia" /proc/interrupts | awk '{s=0; for(i=2;i<=NF-4;i++) s+=$i; print $NF, s}')
  echo "before: $A"; echo "after:  $B"
  echo "--- 16MB anon mappings: $(awk '{split($1,a,"-"); s=strtonum("0x"a[2])-strtonum("0x"a[1]); if (s>15000000 && s<17000000 && $6=="") c++} END{print c+0}' /proc/$P/maps)"
} > "$D/status.log" 2>&1
[ -x "$T/sampler" ] || gcc -O2 -o "$T/sampler" "$T/sampler.c"
"$T/sampler" "$P" 8 0 > "$D/samples.txt" 2>>"$D/status.log"
python3 "$T/symbolize.py" "$P" "$D/samples.txt" self > "$D/profile.txt" 2>&1
echo "done, results in $D"
head -12 "$D/profile.txt"
