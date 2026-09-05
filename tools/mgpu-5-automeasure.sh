#!/bin/bash
# Waits until tty4 (the Hyprland VT) is active, then samples Hyprland for 25 s. Run from tty3 in
# the background; the user just switches to tty4 for a minute.
P=$(pgrep -x Hyprland | head -1)
D=$HOME/Experiments/mgpu/diag/auto-$(date +%H%M%S)
mkdir -p "$D"
T=$HOME/Experiments/mgpu/tools
for i in $(seq 1 300); do [ "$(cat /sys/class/tty/tty0/active)" = tty4 ] && break; sleep 1; done
[ "$(cat /sys/class/tty/tty0/active)" = tty4 ] || { echo "tty4 never became active" > "$D/status.log"; exit 1; }
sleep 6
{
  echo "active vt: $(cat /sys/class/tty/tty0/active), Hyprland pid $P"
  echo "--- irq + utime over 10 s:"
  A=$(grep -E "i915|nvidia" /proc/interrupts | awk '{s=0; for(i=2;i<=NF-4;i++) s+=$i; printf "%s=%d ", $NF, s}')
  U0=$(awk '{print $14}' /proc/$P/stat); S0=$(awk '{print $15}' /proc/$P/stat)
  sleep 10
  B=$(grep -E "i915|nvidia" /proc/interrupts | awk '{s=0; for(i=2;i<=NF-4;i++) s+=$i; printf "%s=%d ", $NF, s}')
  U1=$(awk '{print $14}' /proc/$P/stat); S1=$(awk '{print $15}' /proc/$P/stat)
  echo "irq before: $A"; echo "irq after:  $B"
  echo "Hyprland user cpu: $(( (U1-U0) ))% kernel: $(( (S1-S0) ))%  (ticks per 10 s = percent)"
  echo "--- top:"; top -b -n 3 -d 2 -p "$P" | grep Hyprland
  echo "active vt now: $(cat /sys/class/tty/tty0/active)"
} > "$D/status.log" 2>&1
"$T/sampler" "$P" 8 0 > "$D/samples.txt" 2>>"$D/status.log"
python3 "$T/symbolize.py" "$P" "$D/samples.txt" self > "$D/profile.txt" 2>&1
echo "active vt at end: $(cat /sys/class/tty/tty0/active)" >> "$D/status.log"
