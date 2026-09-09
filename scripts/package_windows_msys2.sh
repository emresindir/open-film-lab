#!/bin/bash
# Script to build and package Open Film Lab for Windows under MSYS2 UCRT64 / MINGW64
set -e

DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"

echo "=========================================="
echo "  Packaging Open Film Lab for Windows"
echo "=========================================="

DIST_DIR="$DIR/build/dist"
WIN_DIR="$DIST_DIR/open-film-lab-windows"
ZIP_OUTPUT="$DIST_DIR/Open-Film-Lab-Windows-x64.zip"

# 1. Build release binary
echo "--> Building release binary with Meson..."
meson setup --reconfigure -Dbuildtype=release build
ninja -C build

# 2. Prepare distribution directory
rm -rf "$WIN_DIR"
mkdir -p "$WIN_DIR"
mkdir -p "$WIN_DIR/share/glib-2.0/schemas"
mkdir -p "$WIN_DIR/share/icons/hicolor"

# Copy executable and icons
cp "$DIR/build/src/open-film-lab.exe" "$WIN_DIR/"
cp "$DIR/resources/app_icon.ico" "$WIN_DIR/"

# 3. Collect dependent DLLs
echo "--> Collecting runtime DLLs..."
# In MSYS2 environment (UCRT64 or MINGW64):
PREFIX="${MINGW_PREFIX:-/ucrt64}"

# Function to recursively copy DLL dependencies
copied_dlls=()
copy_dlls_recursive() {
    local target="$1"
    local deps
    deps=$(ntldd -R "$target" 2>/dev/null | grep -i "$PREFIX" | awk '{print $3}' | sort -u || true)
    for dll in $deps; do
        if [ -f "$dll" ]; then
            local bname
            bname=$(basename "$dll")
            if [ ! -f "$WIN_DIR/$bname" ]; then
                cp "$dll" "$WIN_DIR/"
                echo "  + $bname"
            fi
        fi
    done
}

if command -v ntldd >/dev/null 2>&1; then
    copy_dlls_recursive "$WIN_DIR/open-film-lab.exe"
else
    echo "  (ntldd not found, copying core GTK/GLib/LibRaw/TIFF DLLs directly from $PREFIX/bin)..."
    for pattern in gtk-4 libraw libtiff libjpeg liblcms2 pango cairo gdk_pixbuf glib gobject gio epoxy fribidi harfbuzz fontconfig freetype zlib; do
        cp "$PREFIX"/bin/*"$pattern"*.dll "$WIN_DIR/" 2>/dev/null || true
    done
fi

# 4. Copy GSettings Schemas & compile
echo "--> Bundling GSettings schemas..."
if [ -d "$PREFIX/share/glib-2.0/schemas" ]; then
    cp "$PREFIX/share/glib-2.0/schemas/"* "$WIN_DIR/share/glib-2.0/schemas/" 2>/dev/null || true
    glib-compile-schemas "$WIN_DIR/share/glib-2.0/schemas/"
fi

# 5. Copy icons
echo "--> Bundling icon assets..."
if [ -d "$PREFIX/share/icons/hicolor" ]; then
    cp -r "$PREFIX/share/icons/hicolor" "$WIN_DIR/share/icons/" 2>/dev/null || true
fi

# 6. Create portable ZIP archive
echo "--> Creating portable ZIP package..."
(cd "$DIST_DIR" && zip -r -q "Open-Film-Lab-Windows-x64.zip" "open-film-lab-windows")

echo ""
echo "=========================================="
echo "  Windows Distribution Package Ready!"
echo "=========================================="
echo "Folder:      $WIN_DIR"
echo "ZIP Archive: $ZIP_OUTPUT"
