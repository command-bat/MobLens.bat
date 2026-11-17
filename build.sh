#!/usr/bin/env bash
# MobLens - compila o aplicativo em ./dist/MobLens
#
# Execute no terminal "MSYS2 UCRT64":
#   ./build.sh          compila
#   ./build.sh --deps   instala antes o compilador (pacman)
#   ./build.sh --installer  também gera dist/MobLens-Setup.exe (NSIS)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
OUT="$ROOT/dist/MobLens"

if [[ "${MSYSTEM:-}" != "UCRT64" ]]; then
    echo "Execute este script no terminal 'MSYS2 UCRT64'." >&2
    exit 1
fi

if [[ "${1:-}" == "--deps" ]]; then
    pacman -S --needed --noconfirm \
        mingw-w64-ucrt-x86_64-gcc \
        mingw-w64-ucrt-x86_64-cmake \
        mingw-w64-ucrt-x86_64-ninja
fi

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD"

mkdir -p "$OUT"
cp "$BUILD/MobLens.exe" "$BUILD/moblens_vcam.dll" "$BUILD/moblens_dshow.dll" "$OUT/"
# -u: não recopia (o adb.exe pode estar em execução)
cp -u "$ROOT"/third_party/scrcpy/{adb.exe,AdbWinApi.dll,AdbWinUsbApi.dll,scrcpy-server} "$OUT/"
cp -u "$ROOT/third_party/scrcpy/LICENSE.txt" "$OUT/LICENSE-scrcpy.txt"

echo
echo "Pronto: $OUT"
echo "Execute MobLens.exe (na primeira vez ele instala a webcam virtual)."

if [[ " $* " == *" --installer "* ]]; then
    makensis -V2 "$ROOT/installer/moblens.nsi"
    echo "Instalador: $ROOT/dist/MobLens-Setup.exe"
fi
