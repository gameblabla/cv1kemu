#!/usr/bin/env bash
# Build a Qt6 Linux AppDir/AppImage for CV1000.
# Requires Qt6 development packages. If appimagetool or linuxdeploy are absent,
# the script still leaves a runnable AppDir under build/appdir/CV1000.AppDir.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="CV1000"
BUILD="$ROOT/build/qt6-package"
APPDIR="$BUILD/$APP.AppDir"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
HOST_TUNE="${HOST_TUNE:-generic}"
VERSION="${VERSION:-$(date +%Y.%m.%d)}"

echo "==> building Qt6 frontend"
make -C "$ROOT" -f Makefile.qt6 HOST_TUNE="$HOST_TUNE" -j"$JOBS"

rm -rf "$APPDIR"
install -Dm755 "$ROOT/cv1k_qt6" "$APPDIR/usr/bin/cv1k_qt6"
install -Dm644 "$ROOT/packaging/cv1k.desktop" "$APPDIR/usr/share/applications/cv1k.desktop"
install -Dm644 "$ROOT/resources/cv1k.svg" "$APPDIR/usr/share/icons/hicolor/scalable/apps/cv1k.svg"
install -Dm644 "$ROOT/resources/cv1k.svg" "$APPDIR/cv1k.svg"
if [ -f "$ROOT/third_party/sdl/SDL3-3.4.10.tar.gz" ]; then
    install -Dm644 "$ROOT/third_party/sdl/SDL3-3.4.10.tar.gz" "$APPDIR/usr/share/cv1k/third_party/sdl/SDL3-3.4.10.tar.gz"
fi
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/cv1k_qt6" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"

# Optional dependency bundling. Prefer linuxdeploy when available because it can
# collect Qt plugins and non-system shared libraries. Keep the AppDir even if
# bundling tools are unavailable.
if command -v linuxdeploy >/dev/null 2>&1; then
    echo "==> running linuxdeploy"
    linuxdeploy --appdir "$APPDIR" \
        --executable "$APPDIR/usr/bin/cv1k_qt6" \
        --desktop-file "$APPDIR/usr/share/applications/cv1k.desktop" \
        --icon-file "$ROOT/resources/cv1k.svg" || true
fi

if command -v linuxdeploy-plugin-qt >/dev/null 2>&1; then
    echo "==> running linuxdeploy Qt plugin"
    QMAKE="${QMAKE:-qmake6}" linuxdeploy-plugin-qt --appdir "$APPDIR" || true
fi

if command -v appimagetool >/dev/null 2>&1; then
    echo "==> building AppImage"
    ARCH="${ARCH:-$(uname -m)}" VERSION="$VERSION" appimagetool "$APPDIR" "$ROOT/CV1000-$VERSION-${ARCH:-$(uname -m)}.AppImage"
else
    echo "==> appimagetool not found; AppDir is ready at $APPDIR"
fi
