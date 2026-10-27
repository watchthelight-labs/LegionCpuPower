#!/bin/bash
# <wtl> project: legioncpupower | file: legionfan/legion-fan-apply.sh | author: watchthelight </wtl>
# Runs as root from the LaunchDaemon org.watchthelight.legionfan: at boot (before login) and whenever the menu app
# changes /Library/Application Support/LegionFan/settings. Applies mode= (auto, low, full, curve), curve= (ten
# comma-separated EC values), profile= (performance, balanced, saver: EPP 0 / 32 / 128, saver also turns sched off),
# sched= and guard= (0 or 1) to LegionCpuPower's sysctls. Anything that does not match exactly is ignored.
set -u
F="/Library/Application Support/LegionFan/settings"
[ -f "$F" ] || exit 0
sysctl -n debug.legioncpu.fan >/dev/null 2>&1 || { logger -t legionfan "LegionCpuPower fan sysctl missing"; exit 0; }
mode=$(sed -n 's/^mode=//p' "$F" | head -1)
curve=$(sed -n 's/^curve=//p' "$F" | head -1)
if [[ "$curve" =~ ^[0-9]{1,3}(,[0-9]{1,3}){9}$ ]]; then
  sysctl -w debug.legioncpu.fancurve="$curve" >/dev/null
fi
profile=$(sed -n 's/^profile=//p' "$F" | head -1)
sched=$(sed -n 's/^sched=//p' "$F" | head -1)
guard=$(sed -n 's/^guard=//p' "$F" | head -1)
case "$profile" in
  performance) sysctl -w debug.legioncpu.epp=0 >/dev/null ;;
  balanced) sysctl -w debug.legioncpu.epp=32 >/dev/null ;;
  saver) sysctl -w debug.legioncpu.epp=128 >/dev/null; sched=0 ;;
esac
case "$sched" in 0|1) sysctl -w debug.legioncpu.sched=$sched >/dev/null 2>&1 ;; esac
case "$guard" in 0|1) sysctl -w debug.legioncpu.guard=$guard >/dev/null 2>&1 ;; esac
case "$mode" in
  auto) n=0 ;; full) n=1 ;; curve) n=2 ;; low) n=3 ;; *) exit 0 ;;
esac
sysctl -w debug.legioncpu.fan=$n >/dev/null && logger -t legionfan "fan mode $mode${curve:+ curve $curve}"
