#!/bin/bash
# <wtl> project: legioncpupower | file: legionfan/legion-fan-apply.sh | author: watchthelight </wtl>
# Runs as root from the LaunchDaemon org.watchthelight.legionfan: at boot (before login) and whenever the menu app
# changes /Library/Application Support/LegionFan/settings. Applies mode= (auto, low, full, curve) and curve= (ten
# comma-separated EC values) to LegionCpuPower's sysctls. Anything that does not match exactly is ignored.
set -u
F="/Library/Application Support/LegionFan/settings"
[ -f "$F" ] || exit 0
sysctl -n debug.legioncpu.fan >/dev/null 2>&1 || { logger -t legionfan "LegionCpuPower fan sysctl missing"; exit 0; }
mode=$(sed -n 's/^mode=//p' "$F" | head -1)
curve=$(sed -n 's/^curve=//p' "$F" | head -1)
if [[ "$curve" =~ ^[0-9]{1,3}(,[0-9]{1,3}){9}$ ]]; then
  sysctl -w debug.legioncpu.fancurve="$curve" >/dev/null
fi
case "$mode" in
  auto) n=0 ;; full) n=1 ;; curve) n=2 ;; low) n=3 ;; *) exit 0 ;;
esac
sysctl -w debug.legioncpu.fan=$n >/dev/null && logger -t legionfan "fan mode $mode${curve:+ curve $curve}"
