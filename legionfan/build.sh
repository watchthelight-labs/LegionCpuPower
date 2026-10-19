#!/bin/bash
# <wtl> project: legioncpupower | file: legionfan/build.sh | author: watchthelight </wtl>
# Builds out/LegionFan.app with the Command Line Tools (no Xcode project).
set -euo pipefail
cd "$(dirname "$0")"
A=out/LegionFan.app/Contents
rm -rf out && mkdir -p "$A/MacOS"
swiftc -O -parse-as-library -target x86_64-apple-macos14 LegionFan.swift -o "$A/MacOS/LegionFan"
cat > "$A/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>org.watchthelight.legionfan</string>
<key>CFBundleName</key><string>LegionFan</string>
<key>CFBundleExecutable</key><string>LegionFan</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.1.0</string>
<key>CFBundleVersion</key><string>0.1.0</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
<key>LSUIElement</key><true/>
</dict></plist>
PL
codesign -s - --force "out/LegionFan.app" 2>/dev/null
echo "built out/LegionFan.app"
