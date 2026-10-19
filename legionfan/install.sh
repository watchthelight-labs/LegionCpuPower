#!/bin/bash
# <wtl> project: legioncpupower | file: legionfan/install.sh | author: watchthelight </wtl>
# Installs LegionFan (run with sudo): the apply script and its LaunchDaemon, the settings folder (writable by admins,
# so the menu app needs no root), the app in /Applications and the per-user LaunchAgent that starts it at login.
# The first settings file is mode=full. Uninstall: install.sh --remove
set -euo pipefail
cd "$(dirname "$0")"
D="/Library/Application Support/LegionFan"
if [ "${1:-}" = "--remove" ]; then
  launchctl bootout system/org.watchthelight.legionfan 2>/dev/null || true
  rm -f /Library/LaunchDaemons/org.watchthelight.legionfan.plist /Library/LaunchAgents/org.watchthelight.legionfan.menu.plist \
        /usr/local/libexec/legion-fan-apply
  rm -rf /Applications/LegionFan.app "$D"
  sysctl -w debug.legioncpu.fan=0 >/dev/null 2>&1 || true
  echo "removed"; exit 0
fi
[ -d out/LegionFan.app ] || { echo "run build.sh first"; exit 1; }
install -d -m 755 /usr/local/libexec
install -m 755 -o root -g wheel legion-fan-apply.sh /usr/local/libexec/legion-fan-apply
install -d -m 775 -o root -g admin "$D"
[ -f "$D/settings" ] || { printf 'mode=full\n' > "$D/settings"; chown root:admin "$D/settings"; chmod 664 "$D/settings"; }
rm -rf /Applications/.LegionFan.new && cp -R out/LegionFan.app /Applications/.LegionFan.new
rm -rf /Applications/LegionFan.app && mv /Applications/.LegionFan.new /Applications/LegionFan.app
install -m 644 -o root -g wheel org.watchthelight.legionfan.plist /Library/LaunchDaemons/
install -m 644 -o root -g wheel org.watchthelight.legionfan.menu.plist /Library/LaunchAgents/
launchctl bootout system/org.watchthelight.legionfan 2>/dev/null || true
launchctl bootstrap system /Library/LaunchDaemons/org.watchthelight.legionfan.plist
echo "installed; the menu bar icon starts at the next login (or: open /Applications/LegionFan.app)"
