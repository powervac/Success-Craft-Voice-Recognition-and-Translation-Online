#!/usr/bin/env bash
# Builds "Voice Recognition.app" and VoiceRecognition-macOS.dmg (universal: Apple Silicon + Intel).
# Run on a Mac with the Xcode command line tools:   ./mac/build.sh
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd .. && pwd)"
OUT="$PWD/dist"
APP="$OUT/Voice Recognition.app"

rm -rf "$OUT" .build/release 2>/dev/null || true
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

echo "== compiling (arm64 + x86_64)"
swift build -c release --arch arm64 --arch x86_64
BIN="$(swift build -c release --arch arm64 --arch x86_64 --show-bin-path)/VoiceRecognition"
cp "$BIN" "$APP/Contents/MacOS/VoiceRecognition"

echo "== bundle"
cp Info.plist "$APP/Contents/Info.plist"
cp "$ROOT/worker.py" "$ROOT/dl_progress.py" "$ROOT/requirements.txt" "$APP/Contents/Resources/"
cp "$ROOT/assets/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"

echo "== signing (ad-hoc: no Apple Developer account needed; Gatekeeper still asks on the first start)"
codesign --force --deep --sign - --entitlements VoiceRecognition.entitlements "$APP"
codesign --verify --deep --strict "$APP"

echo "== disk image"
STAGE="$OUT/dmg"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
cat > "$STAGE/READ ME FIRST.txt" <<'EOF'
Voice Recognition and Translation

1. Drag "Voice Recognition" onto "Applications".
2. The first time, right-click the app in Applications and choose Open (the app is not signed by Apple),
   then confirm Open. After that it starts normally.
3. On the first start the app sets itself up (Python, libraries, speech models: a few GB, needs internet).
   Allow the microphone, and - for the other side of a call - "Screen & System Audio Recording".

Everything is stored in ~/Library/Application Support/VoiceRecognition. Delete that folder to uninstall completely.
EOF
hdiutil create -volname "Voice Recognition" -srcfolder "$STAGE" -ov -format UDZO "$OUT/VoiceRecognition-macOS.dmg"
rm -rf "$STAGE"
shasum -a 256 "$OUT/VoiceRecognition-macOS.dmg"
echo "Done: $OUT/VoiceRecognition-macOS.dmg"
