#!/bin/bash
# <wtl> project: legioncpupower | file: probe/build-mac.sh | author: watchthelight </wtl>
# Native build on the Legion's macOS with the Command Line Tools SDK (no WSL needed). Output: out/LegionCpuProbe.kext
set -euo pipefail
cd "$(dirname "$0")"
SDK=$(xcrun --sdk macosx --show-sdk-path)
K=$SDK/System/Library/Frameworks/Kernel.framework/Headers
OUT=out/LegionCpuProbe.kext/Contents
mkdir -p "$OUT/MacOS" out/obj
FLAGS="-arch x86_64 -mmacosx-version-min=10.15 -mkernel -fapple-kext -nostdinc -fno-builtin -fno-common -fno-stack-protector
 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -isystem $(clang -print-resource-dir)/include -I$K -O2 -Wall -Wno-unused-parameter"
clang++ $FLAGS -std=gnu++17 -nostdinc++ -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wno-inconsistent-missing-override \
  -c LegionCpuProbe.cpp -o out/obj/LegionCpuProbe.o
clang $FLAGS -std=gnu11 -DKEXT_ID=org.watchthelight.LegionCpuProbe -DKEXT_VERSION='"1.1.0"' \
  -c kmod_info.c -o out/obj/kmod_info.o
clang++ -arch x86_64 -mmacosx-version-min=10.15 -nostdlib -Xlinker -kext -Xlinker -export_dynamic \
  out/obj/LegionCpuProbe.o out/obj/kmod_info.o -L"$SDK/usr/lib" -lkmodc++ -lkmod -lcc_kext -o "$OUT/MacOS/LegionCpuProbe"
cp Info.plist "$OUT/Info.plist"
otool -hv "$OUT/MacOS/LegionCpuProbe" | grep -q KEXTBUNDLE && echo "filetype: MH_KEXT_BUNDLE"
nm "$OUT/MacOS/LegionCpuProbe" | grep -q ' _kmod_info' && echo "kmod_info: present"
echo "undefined: $(nm -u "$OUT/MacOS/LegionCpuProbe" | tr '\n' ' ')"
