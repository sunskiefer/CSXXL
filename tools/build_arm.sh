#!/usr/bin/env bash
# CSXXL: build the ARMv7 (Force / MPC OS) plugin. Needs: pip install ziglang; MPC_VST = sunskiefer/mpc-vst-plugins checkout (wrapper with the Q-Link UpdateDisplay fix).
set -euo pipefail
cd "$(dirname "$0")/.."
MPC_VST="${MPC_VST:-/home/claude/sunskiefer/mpc-vst-plugins}"; ZIG="${ZIG:-python3 -m ziglang}"
TGT="-target arm-linux-gnueabihf.2.31 -mcpu=generic+v7a+vfp3d16-d32-neon+thumb2 -ffp-contract=off"
FL="-O2 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -DNDEBUG -std=gnu11 -DHAS_SONG_POS=1 -DMODULE_SUBDIR=\"pad\""
mkdir -p vst/build/arm; OBJS=""
for f in src/engine.c src/xyz_pad.c "$MPC_VST/wrapper/vst2_wrap.c"; do
  o="vst/build/arm/$(basename "$f").o"; $ZIG cc $TGT $FL -Wall -Wno-unused-parameter -Ivst/build -Isrc -I"$MPC_VST/wrapper" -c "$f" -o "$o"; OBJS="$OBJS $o"
done
printf '{\n  global: VSTPluginMain;\n  local: *;\n};\n' > vst/build/arm/exports.map
$ZIG cc $TGT -shared -fPIC -Wl,--no-undefined -Wl,--version-script=vst/build/arm/exports.map -Wl,--gc-sections -Wl,-s $OBJS -lm -ldl -lpthread -o vst/build/csxxl.so
echo "highest glibc symbol: $(readelf -V vst/build/csxxl.so | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
