#!/usr/bin/env bash
#
# build.sh - Cross-compile PreoMG14Driver.exe from source.
#
# The build is fully reproducible: no network access, no prebuilt binaries, no
# code signing. It uses the Zig C compiler, which bundles a MinGW-w64
# toolchain and can target Windows directly from Linux.
#
#   ./build.sh              build the .exe
#   ./build.sh --clean      remove build artefacts first
#
set -euo pipefail

cd "$(dirname "$0")"

TARGET="x86_64-windows-gnu"
OUT="dist/PreoMG14Driver.exe"
OBJDIR="build/obj"

SOURCES=(util settings device hidinfo driverops buttons main)
LIBS=(-lhid -lsetupapi -lcfgmgr32 -ladvapi32 -lshell32 -lversion -lcomctl32 -luser32 -lgdi32)

if [[ "${1:-}" == "--clean" ]]; then
    rm -rf build dist
fi

# --- locate the compiler -------------------------------------------------
if command -v zig >/dev/null 2>&1; then
    ZIG=(zig)
elif python3 -c "import ziglang" >/dev/null 2>&1; then
    ZIG=(python3 -m ziglang)
elif [[ -x "$HOME/.local/share/preo-build/bin/python" ]] && \
     "$HOME/.local/share/preo-build/bin/python" -c "import ziglang" >/dev/null 2>&1; then
    ZIG=("$HOME/.local/share/preo-build/bin/python" -m ziglang)
else
    cat >&2 <<'EOF'
error: the Zig C compiler was not found.

This project cross-compiles to Windows with Zig, which bundles its own
MinGW-w64 toolchain. Install it with either:

    pip install ziglang          # build.sh finds this automatically
    # or download https://ziglang.org/download/ and put `zig` on PATH
EOF
    exit 1
fi

ZIGFLAGS=(-target "$TARGET" -O2 -Wall -Wextra -Isrc)

mkdir -p "$OBJDIR" dist

# --- compile -------------------------------------------------------------
echo "==> Compiling"
for f in "${SOURCES[@]}"; do
    printf '    %-12s ' "$f.c"
    "${ZIG[@]}" cc "${ZIGFLAGS[@]}" -c "src/$f.c" -o "$OBJDIR/$f.o"
    echo "ok"
done

# --- resources (icon, version info, manifest) ----------------------------
echo "==> Resources"
"${ZIG[@]}" rc /i src /fo "$OBJDIR/app.res.o" src/PreoMG14Driver.rc
echo "    PreoMG14Driver.rc ok"

# --- link ----------------------------------------------------------------
# -municode selects wWinMain; --subsystem,windows makes it a GUI app so no
# console window flashes up behind the window.
echo "==> Linking"
"${ZIG[@]}" cc -target "$TARGET" -O2 -municode -Wl,--subsystem,windows \
    "${LIBS[@]}" -o "$OUT" "$OBJDIR"/*.o
rm -f dist/*.pdb

# --- verify --------------------------------------------------------------
echo "==> Verifying"
python3 tools/inspect_pe.py "$OUT"

echo
echo "Built $OUT"
sha256sum "$OUT" | tee dist/PreoMG14Driver.exe.sha256
