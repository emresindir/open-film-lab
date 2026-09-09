#!/bin/bash
set -e

DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="$DIR/build/Open Film Lab.app"

echo "Creating macOS Application Bundle at $APP_DIR..."
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS"
mkdir -p "$APP_DIR/Contents/Resources"

cp "$DIR/build/src/open-film-lab" "$APP_DIR/Contents/MacOS/open-film-lab"
chmod +x "$APP_DIR/Contents/MacOS/open-film-lab"

cp "$DIR/resources/app_icon.icns" "$APP_DIR/Contents/Resources/app_icon.icns"

cat << 'EOF' > "$APP_DIR/Contents/Info.plist"
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

echo "✓ Created $APP_DIR"
