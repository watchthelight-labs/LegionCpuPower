#!/bin/bash
# <wtl> project: legioncpupower | file: tools/build-mac.sh | author: watchthelight </wtl>
# Native build on macOS with the Command Line Tools SDK (no Xcode project). Output: out/LegionCpuPower.kext
# Lilu headers and Library/plugin_start.cpp come from the Lilu 1.7.2 source in $LILU_SRC (default ./Lilu, cloned
# from acidanthera/Lilu at tag 1.7.2 on first run); Lilu's symbols are resolved at load time against the Lilu.kext
# OpenCore injects first. Link check: LILU_KEXT=/path/to/Lilu.kext tools/build-mac.sh
# Nothing is loaded or installed. The build is reproducible with the same Apple clang.
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' Info.plist)
LILU_SRC=${LILU_SRC:-$PWD/Lilu}
if [ ! -f "$LILU_SRC/Lilu/Headers/plugin_start.hpp" ]; then
  echo "cloning acidanthera/Lilu 1.7.2 into $LILU_SRC"
  git clone -q --depth 1 --branch 1.7.2 https://github.com/acidanthera/Lilu "$LILU_SRC"
fi
SDK=$(xcrun --sdk macosx --show-sdk-path)
K=$SDK/System/Library/Frameworks/Kernel.framework/Headers
OUT=out/LegionCpuPower.kext/Contents
rm -rf out/LegionCpuPower.kext
mkdir -p "$OUT/MacOS" out/obj
FLAGS="-arch x86_64 -mmacosx-version-min=10.15 -mkernel -nostdinc -fno-builtin -fno-common -fno-stack-protector
 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -isystem $(clang -print-resource-dir)/include -I$K -O2 -Wall -Wno-unused-parameter"
CXXFLAGS="$FLAGS -fapple-kext -std=gnu++17 -nostdinc++ -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wno-inconsistent-missing-override
 -I$LILU_SRC/Lilu -DPRODUCT_NAME=LegionCpuPower -DMODULE_VERSION=$VERSION -fvisibility=hidden -D__ACIDANTHERA_MAC_SDK=1 -Wno-sign-conversion -Wno-shadow"
clang++ $CXXFLAGS -c src/LegionCpuPower.cpp -o out/obj/LegionCpuPower.o
clang++ $CXXFLAGS -Wno-unused-function -c "$LILU_SRC/Lilu/Library/plugin_start.cpp" -o out/obj/plugin_start.o
clang $FLAGS -std=gnu11 -DKEXT_ID=org.watchthelight.LegionCpuPower -DKEXT_VERSION="\"$VERSION\"" \
  -c src/kmod_info.c -o out/obj/kmod_info.o
clang++ -arch x86_64 -mmacosx-version-min=10.15 -nostdlib -Xlinker -kext -Xlinker -export_dynamic \
  out/obj/LegionCpuPower.o out/obj/plugin_start.o out/obj/kmod_info.o -L"$SDK/usr/lib" -lkmodc++ -lkmod -lcc_kext \
  -o "$OUT/MacOS/LegionCpuPower"
cp Info.plist "$OUT/Info.plist"
BIN="$OUT/MacOS/LegionCpuPower"
otool -hv "$BIN" | grep -q KEXTBUNDLE && echo "filetype: MH_KEXT_BUNDLE"
nm "$BIN" | grep -q ' _kmod_info' && echo "kmod_info: present"
echo "undefined: $(nm -u "$BIN" | tr '\n' ' ')"
if nm -u "$BIN" | grep -qE '^_{2,3}(cxa_|Unwind|gxx_)'; then echo "FAIL: C++ runtime symbols the kernel does not export"; exit 1; fi
# Link check against a Lilu.kext: every Lilu symbol the plugin imports must be exported by it
if [ -n "${LILU_KEXT:-}" ]; then
  LBIN="$LILU_KEXT/Contents/MacOS/Lilu"
  miss=0
  for s in $(nm -u "$BIN" | grep -E 'Lilu|KernelPatcher|_lilu'); do
    nm -gU "$LBIN" | awk '{print $3}' | grep -qx "$s" || { echo "MISSING in Lilu: $s"; miss=1; }
  done
  [ "$miss" = 0 ] && echo "lilu symbols: all $(nm -u "$BIN" | grep -cE 'Lilu|KernelPatcher|_lilu') exported by Lilu $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$LILU_KEXT/Contents/Info.plist")"
  [ "$miss" = 0 ]
fi
echo "built org.watchthelight.LegionCpuPower $VERSION: $BIN ($(stat -f %z "$BIN") bytes)"
echo "sha256: $(shasum -a 256 "$BIN" | cut -c1-64)"
