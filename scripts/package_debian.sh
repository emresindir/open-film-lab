#!/bin/bash
# Script to build and package Open Film Lab into a Debian package (.deb)
set -e

DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"

echo "=========================================="
echo "  Packaging Open Film Lab for Debian/Linux"
echo "=========================================="

# 1. Determine architecture
if command -v dpkg >/dev/null 2>&1; then
    ARCH="$(dpkg --print-architecture)"
else
    UNAME_M="$(uname -m)"
    case "$UNAME_M" in
        x86_64)  ARCH="amd64" ;;
        aarch64) ARCH="arm64" ;;
        armv7l)  ARCH="armhf" ;;
        riscv64) ARCH="riscv64" ;;
        *)       ARCH="$UNAME_M" ;;
    esac
fi

VERSION="0.1.0"
REVISION="1"
PKG_NAME="open-film-lab"
DEB_NAME="${PKG_NAME}_${VERSION}-${REVISION}_${ARCH}.deb"

DIST_DIR="$DIR/build/dist"
STAGING_DIR="$DIR/build/debian-staging"
BUILD_DIR="$DIR/build"

# 2. Build release binary with prefix /usr
echo "--> Configuring Meson with --prefix=/usr..."
if [ -d "$BUILD_DIR" ]; then
    meson setup --reconfigure --prefix=/usr -Dbuildtype=release "$BUILD_DIR"
else
    meson setup --prefix=/usr -Dbuildtype=release "$BUILD_DIR"
fi

echo "--> Compiling with Ninja..."
ninja -C "$BUILD_DIR"

# 3. Stage files using DESTDIR
echo "--> Staging files to $STAGING_DIR..."
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"
DESTDIR="$STAGING_DIR" ninja -C "$BUILD_DIR" install

# Strip binary if strip is available
if command -v strip >/dev/null 2>&1; then
    echo "--> Stripping debug symbols..."
    strip --strip-unneeded "$STAGING_DIR/usr/bin/open-film-lab" 2>/dev/null || true
fi

# 4. Compute installed size (in KB)
INSTALLED_SIZE=$(du -sk "$STAGING_DIR" | cut -f1)

# 5. Determine dependencies
# Fallback compatible list (Debian 12/13, Ubuntu 22.04/24.04, Mint)
DEPENDS="libc6, libglib2.0-0t64 | libglib2.0-0, libgtk-4-1, libraw23t64 | libraw23 | libraw24, libtiff6, libjpeg62-turbo | libjpeg8, liblcms2-2"

# If dpkg-shlibdeps is available, dynamically query the exact installed library versions from the ELF binary
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
    echo "--> Calculating runtime dependencies with dpkg-shlibdeps..."
    mkdir -p "$STAGING_DIR/debian"
    touch "$STAGING_DIR/debian/control"
    SHLIBDEPS=$(dpkg-shlibdeps -O "$STAGING_DIR/usr/bin/open-film-lab" 2>/dev/null | grep '^shlibs:Depends=' | sed 's/^shlibs:Depends=//' || true)
    rm -rf "$STAGING_DIR/debian"
    if [ -n "$SHLIBDEPS" ]; then
        DEPENDS="$SHLIBDEPS"
        echo "    Detected: $DEPENDS"
    fi
fi

# 6. Create DEBIAN control directory and files
echo "--> Creating DEBIAN package control files..."
mkdir -p "$STAGING_DIR/DEBIAN"

cat <<EOF > "$STAGING_DIR/DEBIAN/control"
Package: ${PKG_NAME}
Version: ${VERSION}-${REVISION}
Section: graphics
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${INSTALLED_SIZE}
Maintainer: Open Film Lab Team <contact@openfilmlab.org>
Depends: ${DEPENDS}
Homepage: https://github.com/open-film-lab/open-film-lab
Description: RAW and film negative processor
 Open Film Lab is a high-performance application for converting and editing
 analog film negatives digitized via camera scanning or film scanners.
 It supports camera RAW formats, uncompressed TIFF scans, negative inversion,
 color and B&W conversion, crop tools, and multi-format export.
EOF

# Post-installation script (refresh desktop and icon caches)
cat <<'EOF' > "$STAGING_DIR/DEBIAN/postinst"
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
fi
exit 0
EOF
chmod 755 "$STAGING_DIR/DEBIAN/postinst"

# Post-removal script (refresh desktop and icon caches)
cat <<'EOF' > "$STAGING_DIR/DEBIAN/postrm"
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
fi
exit 0
EOF
chmod 755 "$STAGING_DIR/DEBIAN/postrm"

# 7. Build Debian package (.deb)
echo "--> Building .deb package..."
mkdir -p "$DIST_DIR"

if dpkg-deb --help 2>&1 | grep -q -- '--root-owner-group'; then
    dpkg-deb --build --root-owner-group "$STAGING_DIR" "$DIST_DIR/$DEB_NAME"
else
    dpkg-deb --build "$STAGING_DIR" "$DIST_DIR/$DEB_NAME"
fi

echo ""
echo "=========================================="
echo "  Debian Package Ready!"
echo "=========================================="
echo "Package: $DIST_DIR/$DEB_NAME"
echo ""
echo "To install on your Debian system:"
echo "  sudo dpkg -i $DIST_DIR/$DEB_NAME"
echo "  sudo apt-get install -f   # (if any dependencies need resolving)"
echo "=========================================="
