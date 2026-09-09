#!/bin/bash
set -e

DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"

echo "=========================================="
echo "  Packaging Open Film Lab for macOS"
echo "=========================================="

DIST_DIR="$DIR/build/dist"
APP_BUNDLE="$DIST_DIR/Open Film Lab.app"
DMG_OUTPUT="$DIST_DIR/Open-Film-Lab-macOS.dmg"
ZIP_OUTPUT="$DIST_DIR/Open-Film-Lab-macOS.zip"

# 1. Build release binary
echo "--> Building release binary..."
meson setup --reconfigure -Dbuildtype=release build
ninja -C build

# 2. Prepare dist directory and base .app structure
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"
mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources/share/glib-2.0/schemas"

# Copy binary & icon
cp "$DIR/build/src/open-film-lab" "$APP_BUNDLE/Contents/MacOS/open-film-lab"
chmod +x "$APP_BUNDLE/Contents/MacOS/open-film-lab"
cp "$DIR/resources/app_icon.icns" "$APP_BUNDLE/Contents/Resources/app_icon.icns"
cp "$DIR/resources/app_icon.png" "$APP_BUNDLE/Contents/Resources/app_icon.png"

# Write Info.plist
cat << 'EOF' > "$APP_BUNDLE/Contents/Info.plist"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>open-film-lab</string>
    <key>CFBundleIconFile</key>
    <string>app_icon.icns</string>
    <key>CFBundleIdentifier</key>
    <string>lab.openfilm.openfilmlab</string>
    <key>CFBundleName</key>
    <string>Open Film Lab</string>
    <key>CFBundleDisplayName</key>
    <string>Open Film Lab</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>0.1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF

# 3. Bundle GLib/GTK schemas
echo "--> Bundling GSettings schemas..."
HOMEBREW_PREFIX="$(brew --prefix 2>/dev/null || echo '/opt/homebrew')"
if [ -d "$HOMEBREW_PREFIX/share/glib-2.0/schemas" ]; then
    cp "$HOMEBREW_PREFIX/share/glib-2.0/schemas/"* "$APP_BUNDLE/Contents/Resources/share/glib-2.0/schemas/" 2>/dev/null || true
    glib-compile-schemas "$APP_BUNDLE/Contents/Resources/share/glib-2.0/schemas/"
fi

# 4. Bundle dynamic libraries
echo "--> Bundling dependent dynamic libraries..."
python3 "$DIR/scripts/bundle_macos_dylibs.py" "$APP_BUNDLE"

# 5. Create ZIP archive
echo "--> Creating ZIP package..."
(cd "$DIST_DIR" && zip -r -q "Open-Film-Lab-macOS.zip" "Open Film Lab.app")

# 6. Create DMG disk image
echo "--> Creating DMG installer image..."
DMG_TEMP="$DIST_DIR/dmg_temp"
rm -rf "$DMG_TEMP" "$DMG_OUTPUT"
mkdir -p "$DMG_TEMP"
cp -R "$APP_BUNDLE" "$DMG_TEMP/"
ln -s /Applications "$DMG_TEMP/Applications"

hdiutil create -volname "Open Film Lab" -srcfolder "$DMG_TEMP" -ov -format UDZO "$DMG_OUTPUT"
rm -rf "$DMG_TEMP"

echo ""
echo "=========================================="
echo "  macOS Distribution Packages Ready!"
echo "=========================================="
ls -lh "$DIST_DIR"
echo "DMG Installer: $DMG_OUTPUT"
echo "ZIP Archive:   $ZIP_OUTPUT"
