#!/bin/bash
# Starts Hyprland from this TTY with copy-engine tracing and a watchdog.
# RUN WITHOUT SUDO, as andarriel, from a TTY you are logged in on:   bash ~/mgpu-2-test.sh
#
# - If Hyprland comes up: use it normally; log out from Hyprland when done (logs are collected then).
# - If it hangs: after 45 s the watchdog kills it and collects the logs. Nothing else to do.
# Logs land in ~/Experiments/mgpu/diag/<time>/
if [ "$(id -u)" = 0 ]; then echo "do NOT run me with sudo"; exit 1; fi
TS=$(date +%H%M%S)
D=$HOME/Experiments/mgpu/diag/$TS
mkdir -p "$D"
{
  echo "installed lib: $(md5sum /usr/lib/libaquamarine.so.0.15.0)"
  echo "tty: $(tty)  XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR  session: $XDG_SESSION_ID"
  echo "start: $(date +%T.%N)"
} > "$D/status.log"

export AQ_MGPU_VK_DEBUG=1
# uwsm-style env that Hyprland normally gets
export XDG_CURRENT_DESKTOP=Hyprland XDG_SESSION_TYPE=wayland XDG_SESSION_DESKTOP=Hyprland

RUNDIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
Hyprland > "$D/hypr-stdout.log" 2>&1 &
PID=$!
echo "Hyprland pid $PID" >> "$D/status.log"

UP=0
for i in $(seq 1 45); do
  sleep 1
  if ! kill -0 "$PID" 2>/dev/null; then echo "Hyprland exited after ${i}s" >> "$D/status.log"; break; fi
  if ls "$RUNDIR"/hypr/*/.socket.sock >/dev/null 2>&1; then
    # socket exists; is it answering?
    SIG=$(ls -t "$RUNDIR"/hypr/ | head -1)
    if HYPRLAND_INSTANCE_SIGNATURE=$SIG timeout 2 hyprctl version >/dev/null 2>&1; then
      echo "UP after ${i}s (instance $SIG)" >> "$D/status.log"; UP=1; break
    fi
  fi
done

if [ "$UP" = 1 ]; then
  # measure CPU a bit later, in the background, then wait for Hyprland to end
  (
    sleep 25
    echo "--- top samples at +25s (idle-ish):" >> "$D/status.log"
    top -b -n 3 -d 2 -p "$PID" | grep -E "Hyprland|%CPU" >> "$D/status.log"
    awk '{split($1,a,"-"); s=strtonum("0x"a[2])-strtonum("0x"a[1]); if (s>15000000 && s<17000000 && $6=="") print "16MB anon mapping:", $1}' /proc/$PID/maps >> "$D/status.log"
    cat /proc/$PID/stat | awk '{print "utime ticks", $14, "stime ticks", $15}' >> "$D/status.log"
  ) &
  wait "$PID"
  echo "Hyprland ended at $(date +%T)" >> "$D/status.log"
else
  if kill -0 "$PID" 2>/dev/null; then
    echo "HUNG: no answer after 45s; state: $(awk '{print $3}' /proc/$PID/stat 2>/dev/null); wchan: $(cat /proc/$PID/wchan 2>/dev/null)" >> "$D/status.log"
    echo "--- threads:" >> "$D/status.log"
    for t in /proc/$PID/task/*; do echo "$(basename $t) $(cat $t/comm) state=$(awk '{print $3}' $t/stat) wchan=$(cat $t/wchan 2>/dev/null)" >> "$D/status.log"; done
    cat /proc/$PID/stat | awk '{print "utime ticks", $14, "stime ticks", $15}' >> "$D/status.log"
    # SIGABRT first: Hyprland's crash handler writes a backtrace of the stuck thread to ~/.cache/hyprland
    kill -ABRT "$PID"
    for j in $(seq 1 15); do sleep 1; kill -0 "$PID" 2>/dev/null || break; done
    if kill -0 "$PID" 2>/dev/null; then kill -9 "$PID"; sleep 1; echo "SIGABRT ignored, killed -9" >> "$D/status.log"; else echo "exited after SIGABRT after ${j}s" >> "$D/status.log"; fi
    for c in $(ls -t ~/.cache/hyprland/hyprlandCrashReport*.txt 2>/dev/null | head -2); do
      [ "$(stat -c %Y "$c")" -ge "$(( $(date +%s) - 120 ))" ] && cp "$c" "$D/"
    done
  fi
fi

# collect Hyprland's own log files (they live in the runtime dir)
for f in "$RUNDIR"/hypr/*/hyprland.log; do [ -f "$f" ] && cp "$f" "$D/hyprland-$(basename $(dirname $f)).log"; done
journalctl -k --since "-3min" --no-pager > "$D/kernel.log" 2>&1
sync
echo
echo "=================== done, logs in $D"
cat "$D/status.log"
