#!/usr/bin/env bash
# Build ready-to-package releases for CV1KEmu.
#
# Produces two self-contained folders under release/:
#   release/cv1kemu-<version>-linux-qt6/   Qt6/Linux frontend (x86-64)
#   release/cv1kemu-<version>-win64/       Windows 64-bit frontend
#
# Each folder contains the binary plus README/LICENSE/NOTICE and is ready
# to be zipped/tarred for distribution.
#
# Requirements:
#   - Qt6 dev packages (Widgets, Multimedia) for the Linux Qt6 build
#   - x86_64-w64-mingw32-gcc (MinGW-w64) for the Win64 cross build
#   - linuxdeploy + linuxdeploy-plugin-qt + appimagetool are auto-downloaded
#     (as portable AppImages) if not already on PATH. Set
#     TOOL_CACHE_DIR to change where they are cached (default
#     ~/.cache/cv1k-release-tools). Set SKIP_TOOL_DOWNLOAD=1 to disable
#     auto-download and fall back to manual ldd-based bundling.
#
# Usage:
#   ./packaging/make_release.sh              # build both targets
#   ./packaging/make_release.sh qt6          # build only Qt6/Linux
#   ./packaging/make_release.sh win64        # build only Win64
#   HOST_TUNE=native ./packaging/make_release.sh   # tune for build host
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# -----------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
HOST_TUNE="${HOST_TUNE:-generic}"          # generic = portable, native = fast
MINGW_CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"
VERSION="${VERSION:-$(date +%Y%m%d)}"
RELEASE_DIR="$ROOT/release"
QT6_DIR="$RELEASE_DIR/cv1kemu-$VERSION-linux-qt6"
WIN64_DIR="$RELEASE_DIR/cv1kemu-$VERSION-win64"
TOOL_CACHE_DIR="${TOOL_CACHE_DIR:-$HOME/.cache/cv1k-release-tools}"

TARGET="${1:-all}"

# -----------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------
log() { printf '==> %s\n' "$*"; }
warn() { printf '!!  %s\n' "$*" >&2; }

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || { warn "required command not found: $1"; return 1; }
}

# -----------------------------------------------------------------------
# AppImage tool management (linuxdeploy, linuxdeploy-plugin-qt, appimagetool)
# -----------------------------------------------------------------------
# These are portable AppImages. If not on PATH, download them once into
# TOOL_CACHE_DIR. A FUSE-less fallback (APPIMAGE_EXTRACT_AND_RUN=1) is
# used automatically if the AppImage cannot mount itself.

# Resolves to the command for a tool, downloading if necessary.
#   ensure_appimage_tool VAR_NAME tool_name download_url filename
# Sets VAR_NAME to the command to invoke (system or cached AppImage),
# or empty if unavailable.
ensure_appimage_tool() {
    local var_name="$1" tool_name="$2" url="$3" filename="$4"
    local cached="$TOOL_CACHE_DIR/$filename"

    # 1. Already on PATH?
    if command -v "$tool_name" >/dev/null 2>&1; then
        eval "$var_name=\"\$tool_name\""
        return 0
    fi

    # 2. Skip download?
    if [ "${SKIP_TOOL_DOWNLOAD:-0}" = "1" ]; then
        eval "$var_name=\"\""
        return 1
    fi

    # 3. Already cached?
    if [ -x "$cached" ]; then
        eval "$var_name=\"\$cached\""
        return 0
    fi

    # 4. Download
    log "Downloading $tool_name"
    mkdir -p "$TOOL_CACHE_DIR"
    local tmp="$cached.part"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$tmp" "$url" || { rm -f "$tmp"; warn "download failed: $tool_name"; eval "$var_name=\"\""; return 1; }
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$tmp" "$url" || { rm -f "$tmp"; warn "download failed: $tool_name"; eval "$var_name=\"\""; return 1; }
    else
        warn "need curl or wget to download $tool_name"
        eval "$var_name=\"\""
        return 1
    fi
    mv "$tmp" "$cached"
    chmod +x "$cached"
    eval "$var_name=\"\$cached\""
    return 0
}

# Run an AppImage tool, transparently falling back to extract-and-run
# if FUSE mounting fails (e.g. inside containers/sandboxes).
#   run_appimage_tool <appimage_path> [args...]
run_appimage_tool() {
    local ai="$1"; shift
    # Try direct execution first (fast, uses FUSE)
    if "$ai" "$@" 2>/tmp/opencode/_ai_err; then
        return 0
    fi
    # FUSE failure? Fall back to extract-and-run
    if grep -qiE "fuse|mount|appimage" /tmp/opencode/_ai_err 2>/dev/null; then
        warn "FUSE unavailable for $(basename "$ai"); using extract-and-run"
        APPIMAGE_EXTRACT_AND_RUN=1 "$ai" "$@"
    else
        # Re-emit the original stderr and return the failure
        cat /tmp/opencode/_ai_err >&2
        return 1
    fi
}

install_doc_files() {
    # $1 = destination dir
    local dst="$1"
    install -Dm644 "$ROOT/LICENSE"  "$dst/LICENSE"
    install -Dm644 "$ROOT/NOTICE"   "$dst/NOTICE"
    install -Dm644 "$ROOT/README.md" "$dst/README.md"
    install -Dm644 "$ROOT/assets/example_mapping.cfg" "$dst/example_mapping.cfg"
}

# =======================================================================
# Qt6 / Linux
# =======================================================================
build_qt6() {
    log "Building Qt6/Linux release"

    if ! need_cmd pkg-config; then
        return 1
    fi
    if ! pkg-config --exists Qt6Widgets Qt6Multimedia 2>/dev/null; then
        warn "Qt6 development packages not found (Qt6Widgets/Qt6Multimedia)"
        warn "Install them or set QT6_NO_PKGCONFIG=1 with manual QT6_CFLAGS/QT6_LIBS"
        return 1
    fi

    log "Compiling cv1kemu_qt6 (HOST_TUNE=$HOST_TUNE, $JOBS jobs)"
    make -f Makefile.qt6 clean >/dev/null 2>&1 || true
    make -f Makefile.qt6 HOST_TUNE="$HOST_TUNE" -j"$JOBS"

    [ -f "$ROOT/cv1kemu_qt6" ] || { warn "cv1kemu_qt6 was not produced"; return 1; }

    log "Assembling release folder: $QT6_DIR"
    rm -rf "$QT6_DIR"
    mkdir -p "$QT6_DIR/bin" "$QT6_DIR/lib" "$QT6_DIR/share/applications" \
             "$QT6_DIR/share/icons/hicolor/scalable/apps" "$QT6_DIR/share/cv1k"

    install -Dm755 "$ROOT/cv1kemu_qt6" "$QT6_DIR/bin/cv1kemu_qt6"
    install -Dm644 "$ROOT/packaging/cv1k.desktop" "$QT6_DIR/share/applications/cv1k.desktop"
    install -Dm644 "$ROOT/resources/cv1k.svg" "$QT6_DIR/share/icons/hicolor/scalable/apps/cv1k.svg"
    install -Dm644 "$ROOT/resources/cv1k.svg" "$QT6_DIR/share/cv1k/cv1k.svg"
    install_doc_files "$QT6_DIR"

    # Bundle SDL3 source tarball if present (for source redistribution parity)
    if [ -f "$ROOT/third_party/sdl/SDL3-3.4.10.tar.gz" ]; then
        install -Dm644 "$ROOT/third_party/sdl/SDL3-3.4.10.tar.gz" \
            "$QT6_DIR/share/cv1k/third_party/sdl/SDL3-3.4.10.tar.gz"
    fi

    # --- Bundle shared libraries ---------------------------------------
    # Try linuxdeploy + linuxdeploy-plugin-qt first: it correctly collects
    # Qt platform plugins, image formats, multimedia backends, etc.
    # The tools are auto-downloaded as portable AppImages if missing.
    local linuxdeploy_bin="" plugin_qt_bin="" appimagetool_bin=""
    ensure_appimage_tool linuxdeploy_bin linuxdeploy \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" \
        "linuxdeploy-x86_64.AppImage" || true
    ensure_appimage_tool plugin_qt_bin linuxdeploy-plugin-qt \
        "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage" \
        "linuxdeploy-plugin-qt-x86_64.AppImage" || true
    ensure_appimage_tool appimagetool_bin appimagetool \
        "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage" \
        "appimagetool-x86_64.AppImage" || true

    local used_linuxdeploy=0
    if [ -n "$linuxdeploy_bin" ]; then
        log "Running linuxdeploy to bundle Qt/SDL libraries"
        local appdir="$RELEASE_DIR/_qt6_appdir/CV1000.AppDir"
        rm -rf "$RELEASE_DIR/_qt6_appdir"
        mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" \
                 "$appdir/usr/share/icons/hicolor/scalable/apps"
        cp "$QT6_DIR/bin/cv1kemu_qt6" "$appdir/usr/bin/"
        cp "$QT6_DIR/share/applications/cv1k.desktop" "$appdir/usr/share/applications/"
        cp "$ROOT/resources/cv1k.svg" "$appdir/usr/share/icons/hicolor/scalable/apps/cv1k.svg"

        run_appimage_tool "$linuxdeploy_bin" --appdir "$appdir" \
            --executable "$appdir/usr/bin/cv1kemu_qt6" \
            --desktop-file "$appdir/usr/share/applications/cv1k.desktop" \
            --icon-file "$ROOT/resources/cv1k.svg" || true

        if [ -n "$plugin_qt_bin" ]; then
            log "Running linuxdeploy-plugin-qt"
            QMAKE="${QMAKE:-qmake6}" \
            run_appimage_tool "$plugin_qt_bin" --appdir "$appdir" || true
        else
            warn "linuxdeploy-plugin-qt unavailable; Qt plugins may be missing"
        fi

        # Copy bundled libs from the AppDir into our release folder
        if [ -d "$appdir/usr/lib" ]; then
            cp -a "$appdir/usr/lib/." "$QT6_DIR/lib/"
            # Also copy Qt plugin directories if present
            for plugindir in "$appdir/usr/plugins" "$appdir/usr/lib/plugins"; do
                if [ -d "$plugindir" ]; then
                    mkdir -p "$QT6_DIR/plugins"
                    cp -a "$plugindir/." "$QT6_DIR/plugins/"
                    break
                fi
            done
            # Copy qt.conf generated by linuxdeploy-plugin-qt (tells Qt
            # where to find plugins relative to the binary)
            if [ -f "$appdir/usr/bin/qt.conf" ]; then
                cp -a "$appdir/usr/bin/qt.conf" "$QT6_DIR/bin/qt.conf"
            fi
            used_linuxdeploy=1
        fi

        # Optionally produce an AppImage
        if [ -n "$appimagetool_bin" ]; then
            log "Creating AppImage"
            # Replace the AppRun symlink with a wrapper that forces the
            # xcb platform plugin (linuxdeploy only bundles xcb, not
            # wayland, so the AppImage would fail on Wayland sessions
            # where QT_QPA_PLATFORM=wayland is inherited).
            cat > "$appdir/AppRun" <<'APPRUN'
#!/usr/bin/env bash
# AppImage runtime sets APPDIR to the mount root.
HERE="${APPDIR:-$(cd "$(dirname "$0")" && pwd)}"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
req="${QT_QPA_PLATFORM:-}"
if [ -n "$req" ] && [ -f "$HERE/usr/plugins/platforms/libq${req}.so" ]; then
    : # requested platform is available, keep it
elif [ -f "$HERE/usr/plugins/platforms/libqxcb.so" ]; then
    export QT_QPA_PLATFORM=xcb
fi
exec "$HERE/usr/bin/cv1kemu_qt6" "$@"
APPRUN
            chmod +x "$appdir/AppRun"
            ARCH="${ARCH:-$(uname -m)}" \
            VERSION="$VERSION" \
            run_appimage_tool "$appimagetool_bin" "$appdir" \
                "$RELEASE_DIR/cv1kemu-$VERSION-linux-qt6.AppImage" || \
                warn "appimagetool failed; AppDir contents still bundled"
        fi

        rm -rf "$RELEASE_DIR/_qt6_appdir"
    fi

    if [ "$used_linuxdeploy" -eq 0 ]; then
        log "linuxdeploy unavailable; bundling libraries manually via ldd"
        bundle_libs_manual "$ROOT/cv1kemu_qt6" "$QT6_DIR/lib"
        bundle_qt_plugins_manual "$QT6_DIR"
    fi

    # --- Wrapper script -------------------------------------------------
    cat > "$QT6_DIR/run.sh" <<'WRAPPER'
#!/usr/bin/env bash
# Launch CV1KEmu Qt6 with bundled libraries and plugins.
HERE="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Tell Qt to load plugins from the bundled folder if present
if [ -d "$HERE/plugins" ]; then
    export QT_PLUGIN_PATH="$HERE/plugins"
fi
# linuxdeploy bundles the xcb platform plugin but not wayland.  If the
# requested platform plugin isn't bundled, fall back to xcb (works on X11
# and Wayland via XWayland).  Override by setting QT_QPA_PLATFORM after
# sourcing this script, or remove this block for auto-detection.
req="${QT_QPA_PLATFORM:-}"
if [ -n "$req" ] && [ -f "$HERE/plugins/platforms/libq${req}.so" ]; then
    : # requested platform is available, keep it
elif [ -f "$HERE/plugins/platforms/libqxcb.so" ]; then
    export QT_QPA_PLATFORM=xcb
fi
exec "$HERE/bin/cv1kemu_qt6" "$@"
WRAPPER
    chmod +x "$QT6_DIR/run.sh"

    log "Qt6/Linux release ready: $QT6_DIR"
    log "  Run with: $QT6_DIR/run.sh <romset.zip>"
}

# Manually copy shared library dependencies (fallback when linuxdeploy
# is unavailable).  Also recurses into the copied libraries' own deps.
bundle_libs_manual() {
    # $1 = binary, $2 = dest lib dir
    local bin="$1" dst="$2"
    mkdir -p "$dst"

    local pending=("$bin")
    local processed=""
    while [ ${#pending[@]} -gt 0 ]; do
        local current="${pending[0]}"
        pending=("${pending[@]:1}")
        local deps
        deps=$(ldd "$current" 2>/dev/null | awk '/=> \// {print $3}' || true)
        local lib
        for lib in $deps; do
            [ -f "$lib" ] || continue
            local base
            base=$(basename "$lib")
            # Skip if already copied
            case " $processed " in
                *" $base "*) continue ;;
            esac
            cp -nL "$lib" "$dst/" 2>/dev/null || true
            processed="$processed $base"
            pending+=("$dst/$base")
        done
    done

    # Copy the dynamic linker as well
    local ldso
    ldso=$(ldd "$bin" 2>/dev/null | awk '/ld-linux/ {print $1}' | head -1 || true)
    if [ -n "$ldso" ] && [ -f "$ldso" ]; then
        cp -nL "$ldso" "$dst/" 2>/dev/null || true
    fi
}

# Bundle essential Qt6 platform/multimedia plugins and their library deps.
# Without the platforms plugin (libqxcb.so / libqwayland.so) the app cannot
# create a window.  linuxdeploy-plugin-qt does this properly; this is the
# manual fallback.
bundle_qt_plugins_manual() {
    # $1 = release dir (contains lib/ and will get plugins/)
    local rel="$1"
    local qt_plugin_dir=""

    # Locate the Qt6 plugins directory
    for candidate in \
        "$(pkg-config --variable=plugindir Qt6Core 2>/dev/null)" \
        "/usr/lib64/qt6/plugins" \
        "/usr/lib/qt6/plugins" \
        "/usr/lib/x86_64-linux-gnu/qt6/plugins"; do
        if [ -n "$candidate" ] && [ -d "$candidate/platforms" ]; then
            qt_plugin_dir="$candidate"
            break
        fi
    done

    if [ -z "$qt_plugin_dir" ]; then
        warn "Could not locate Qt6 plugins directory; the app may fail to start"
        warn "Install linuxdeploy + linuxdeploy-plugin-qt for proper bundling"
        return
    fi

    log "Bundling Qt plugins from $qt_plugin_dir"
    local plugin_dst="$rel/plugins"
    mkdir -p "$plugin_dst"

    # Essential plugin subdirectories for a Qt6 Widgets + Multimedia app
    local subdirs=(
        platforms
        imageformats
        iconengines
        multimedia
        platforminputcontexts
        xcbglintegrations
        wayland-shell-integration
        wayland-graphics-integration-client
        wayland-decoration-client
        egldeviceintegrations
        styles
    )

    local subdir
    for subdir in "${subdirs[@]}"; do
        local src="$qt_plugin_dir/$subdir"
        [ -d "$src" ] || continue
        mkdir -p "$plugin_dst/$subdir"
        local so
        for so in "$src"/*.so; do
            [ -f "$so" ] || continue
            cp -nL "$so" "$plugin_dst/$subdir/" 2>/dev/null || true
            # Bundle the plugin's own shared library dependencies
            local deps
            deps=$(ldd "$so" 2>/dev/null | awk '/=> \// {print $3}' || true)
            local dep
            for dep in $deps; do
                [ -f "$dep" ] || continue
                cp -nL "$dep" "$rel/lib/" 2>/dev/null || true
            done
        done
    done

    # Recursively resolve deps of newly added libs
    local lib
    for lib in "$rel/lib"/*.so*; do
        [ -f "$lib" ] || continue
        local deps
        deps=$(ldd "$lib" 2>/dev/null | awk '/=> \// {print $3}' || true)
        local dep
        for dep in $deps; do
            [ -f "$dep" ] || continue
            cp -nL "$dep" "$rel/lib/" 2>/dev/null || true
        done
    done
}

# =======================================================================
# Windows 64-bit
# =======================================================================
build_win64() {
    log "Building Windows 64-bit release"

    if ! command -v "$MINGW_CC" >/dev/null 2>&1; then
        warn "MinGW-w64 C compiler not found: $MINGW_CC"
        warn "Install mingw-w64 (e.g. 'dnf install mingw64-gcc' or 'apt install mingw-w64')"
        return 1
    fi

    # Build SDL3 from source for MinGW if not already built (enables gamepad input)
    if [ ! -f "$ROOT/third_party/sdl/sdl3-mingw/lib/libSDL3.a" ]; then
        log "Building SDL3 for MinGW (gamepad input support, ~3-5 min)"
        if command -v cmake >/dev/null 2>&1; then
            make -f Makefile.win64 sdl3-mingw CC="$MINGW_CC" -j"$JOBS" || {
                warn "SDL3 build failed; continuing without gamepad support"
            }
        else
            warn "cmake not found; SDL3 gamepad support will be disabled"
        fi
    fi

    log "Cross-compiling cv1kemu_win64.exe (static, $JOBS jobs)"
    make -f Makefile.win64 clean >/dev/null 2>&1 || true
    # -static links libgcc/libwinpthread statically so the .exe only
    # depends on Windows system DLLs (KERNEL32, USER32, GDI32, etc.)
    make -f Makefile.win64 \
        CC="$MINGW_CC" \
        LDFLAGS="-mwindows -static" \
        -j"$JOBS"

    [ -f "$ROOT/cv1kemu_win64.exe" ] || { warn "cv1kemu_win64.exe was not produced"; return 1; }

    log "Assembling release folder: $WIN64_DIR"
    rm -rf "$WIN64_DIR"
    mkdir -p "$WIN64_DIR"

    install -Dm755 "$ROOT/cv1kemu_win64.exe" "$WIN64_DIR/cv1kemu_win64.exe"
    install_doc_files "$WIN64_DIR"

    # Win64-specific quick-start note
    cat > "$WIN64_DIR/README.txt" <<'TXT'
CV1KEmu - Windows 64-bit
========================

Requirements: Windows 10 or later (64-bit). No additional runtime or
DLLs are needed; the executable is statically linked (including SDL3
for gamepad input).

Usage:
  cv1kemu_win64.exe <romset.zip>

Keyboard Controls:
  Arrow keys         - P1 movement
  Z X C V            - P1 buttons
  1                  - P1 start
  5                  - P1 coin
  I K J L            - P2 movement
  A S D F            - P2 buttons
  2                  - P2 start
  6                  - P2 coin
  9                  - service
  F2                 - write control template
  F5 / F8            - quick save / quick load
  Esc                - exit fullscreen (never quits)

Gamepad Controls (SDL3, DirectInput forced):
  D-pad              - movement
  A/B/X/Y (South/East/West/North) - buttons
  Start              - player start
  Back/Select        - coin
  Options > Controls - rebind keyboard and gamepad

All controls are configurable via Options > Controls.

See README.md and NOTICE for licensing and MAME-derived attribution.
TXT

    log "Windows 64-bit release ready: $WIN64_DIR"
}

# =======================================================================
# Main
# =======================================================================
case "$TARGET" in
    qt6|linux)
        build_qt6
        ;;
    win64|win|windows)
        build_win64
        ;;
    all)
        build_qt6 || warn "Qt6/Linux build failed"
        echo
        build_win64 || warn "Win64 build failed"
        ;;
    *)
        warn "Unknown target: $TARGET"
        warn "Usage: $0 [qt6|win64|all]"
        exit 1
        ;;
esac

echo
log "Done. Release folders:"
[ -d "$QT6_DIR" ]   && echo "  $QT6_DIR"
[ -d "$WIN64_DIR" ] && echo "  $WIN64_DIR"
