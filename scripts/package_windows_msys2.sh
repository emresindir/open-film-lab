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

# In MSYS2 environment (UCRT64 or MINGW64):
PREFIX="${MINGW_PREFIX:-/ucrt64}"

# 3. Seed distribution with core GTK4, LibRaw, LibTIFF, and GLib runtime DLLs
echo "--> Seeding core runtime DLLs from $PREFIX/bin..."
for pattern in gtk-4 libraw libtiff libjpeg liblcms2 pango cairo gdk_pixbuf glib gobject gio gmodule graphene epoxy fribidi harfbuzz fontconfig freetype zlib bz2 libpng brotli ffi intl iconv winpthread gcc_s stdc++ pcre2; do
    cp "$PREFIX"/bin/*"$pattern"*.dll "$WIN_DIR/" 2>/dev/null || true
done

# 4. Copy GDK-Pixbuf loaders if available
if [ -d "$PREFIX/lib/gdk-pixbuf-2.0/2.10.0/loaders" ]; then
    echo "--> Bundling GDK-Pixbuf loaders..."
    mkdir -p "$WIN_DIR/lib/gdk-pixbuf-2.0/2.10.0/loaders"
    cp "$PREFIX/lib/gdk-pixbuf-2.0/2.10.0/loaders/"*.dll "$WIN_DIR/lib/gdk-pixbuf-2.0/2.10.0/loaders/" 2>/dev/null || true
    if command -v gdk-pixbuf-query-loaders >/dev/null 2>&1; then
        gdk-pixbuf-query-loaders > "$WIN_DIR/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" 2>/dev/null || true
    fi
fi

# 5. Iteratively resolve all transitive dependencies using ntldd
if command -v ntldd >/dev/null 2>&1; then
    echo "--> Scanning and copying transitive dependencies with ntldd..."
    new_found=1
    pass=1
    while [ $new_found -eq 1 ] && [ $pass -le 10 ]; do
        new_found=0
        echo "  Dependency scan pass $pass..."
        for binary in "$WIN_DIR"/*.exe "$WIN_DIR"/*.dll; do
            [ -f "$binary" ] || continue
            
            # Extract raw paths from ntldd
            deps=$(ntldd "$binary" 2>/dev/null | awk -F'=>' '{if (NF>1) print $2}' | awk '{print $1}' || true)
            for raw_path in $deps; do
                [ -z "$raw_path" ] && continue
                [ "$raw_path" = "not" ] && continue
                
                # Convert Windows path (e.g. C:\msys64\ucrt64\bin\foo.dll) to MSYS2 path (/ucrt64/bin/foo.dll)
                unix_path=$(cygpath -u "$raw_path" 2>/dev/null || true)
                [ -f "$unix_path" ] || continue
                
                # Skip Windows system DLLs (System32, Windows, etc.)
                case "$unix_path" in
                    *[Ww][Ii][Nn][Dd][Oo][Ww][Ss]*|*[Ss][Yy][Ss][Tt][Ee][Mm]32*|*api-ms-win*)
                        continue
                        ;;
                esac
                
                bname=$(basename "$unix_path")
                if [ ! -f "$WIN_DIR/$bname" ]; then
                    cp "$unix_path" "$WIN_DIR/"
                    echo "    + $bname"
                    new_found=1
                fi
            done
        done
        pass=$((pass + 1))
    done
fi

# Validate bundled DLL count
dll_count=$(ls -1 "$WIN_DIR"/*.dll 2>/dev/null | wc -l)
echo "--> Total DLLs bundled: $dll_count"
if [ "$dll_count" -lt 15 ]; then
    echo "ERROR: Only $dll_count DLLs were bundled! Windows distribution is incomplete."
    exit 1
fi

# 6. Copy GSettings Schemas & compile
echo "--> Bundling GSettings schemas..."
if [ -d "$PREFIX/share/glib-2.0/schemas" ]; then
    cp "$PREFIX/share/glib-2.0/schemas/"* "$WIN_DIR/share/glib-2.0/schemas/" 2>/dev/null || true
    glib-compile-schemas "$WIN_DIR/share/glib-2.0/schemas/"
fi

# 7. Copy icons
echo "--> Bundling icon assets..."
if [ -d "$PREFIX/share/icons/hicolor" ]; then
    cp -r "$PREFIX/share/icons/hicolor" "$WIN_DIR/share/icons/" 2>/dev/null || true
fi
if [ -d "$DIR/resources/icons/hicolor" ]; then
    cp -r "$DIR/resources/icons/hicolor"/* "$WIN_DIR/share/icons/hicolor/" 2>/dev/null || true
fi

# 8. Create portable ZIP archive
echo "--> Creating portable ZIP package..."
(cd "$DIST_DIR" && zip -r -q "Open-Film-Lab-Windows-x64.zip" "open-film-lab-windows")

echo ""
echo "=========================================="
echo "  Windows Distribution Package Ready!"
echo "=========================================="
echo "Folder:      $WIN_DIR"
echo "DLL count:   $dll_count"
echo "ZIP Archive: $ZIP_OUTPUT"
