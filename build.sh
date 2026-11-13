#!/bin/bash
# Builds Pipes.saver and LCD Snake.saver into ./dist (universal, ad-hoc signed).
set -euo pipefail
cd "$(dirname "$0")"

FLAGS=(-fobjc-arc -O2 -Wall -arch arm64 -arch x86_64 -mmacosx-version-min=12.0)
rm -rf dist build
mkdir -p dist build

build_saver() {   # name  source  principal-class  bundle-id  frameworks...
    local name="$1" src="$2" cls="$3" id="$4"; shift 4
    local bundle="dist/$name.saver"
    mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources"
    clang "${FLAGS[@]}" -bundle "$src" -o "$bundle/Contents/MacOS/$name" \
        -framework Cocoa -framework ScreenSaver -framework QuartzCore "$@"
    cat > "$bundle/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>$name</string>
    <key>CFBundleIdentifier</key><string>$id</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>$name</string>
    <key>CFBundlePackageType</key><string>BNDL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>12.0</string>
    <key>NSPrincipalClass</key><string>$cls</string>
</dict>
</plist>
EOF
}

build_saver "Pipes"     src/Pipes.m PipesSaverView local.retro-screensavers.pipes -framework SceneKit
build_saver "LCD Snake" src/Snake.m SnakeSaverView local.retro-screensavers.lcd-snake

clang -fobjc-arc -O1 tools/snapshot.m -o build/snapshot -framework Cocoa -framework ScreenSaver

# Use a rendered frame as each saver's thumbnail in System Settings.
build/snapshot "dist/Pipes.saver"     "dist/Pipes.saver/Contents/Resources/thumbnail@2x.png"     14 592 368
build/snapshot "dist/LCD Snake.saver" "dist/LCD Snake.saver/Contents/Resources/thumbnail@2x.png" 25 592 368
for s in "Pipes" "LCD Snake"; do
    sips -z 184 296 "dist/$s.saver/Contents/Resources/thumbnail@2x.png" \
        --out "dist/$s.saver/Contents/Resources/thumbnail.png" >/dev/null
    codesign --force --sign - "dist/$s.saver"
done

(cd dist && rm -f Retro-Screensavers-macOS.zip && zip -qry Retro-Screensavers-macOS.zip "Pipes.saver" "LCD Snake.saver")
echo "Built: dist/Pipes.saver, dist/LCD Snake.saver, dist/Retro-Screensavers-macOS.zip"
