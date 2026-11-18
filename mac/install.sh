#!/usr/bin/env bash
# Installs Voice Recognition and Translation on macOS from the terminal:
#   curl -fsSL https://raw.githubusercontent.com/powervac/Success-Craft-Voice-Recognition-and-Translation-Online/main/mac/install.sh | bash
# Installs the app into /Applications (or ~/Applications when that is not writable), removes the download quarantine
# (the app is signed ad-hoc, not notarized) and creates the `voice-recognition` command. Nothing else is changed.
# Uninstall:  rm -rf "/Applications/Voice Recognition.app" ~/.local/bin/voice-recognition ~/Library/Application\ Support/VoiceRecognition
set -euo pipefail

VERSION="${VOICE_VERSION:-1.0.0}"
SHA256="${VOICE_SHA256:-eecbc4744416ad4d9c7d38cdba668426fae634bbbe83f2f4a12e9621ac7ab8d7}"
URL="${VOICE_URL:-https://github.com/powervac/Success-Craft-Voice-Recognition-and-Translation-Online/releases/download/macos-v${VERSION}/VoiceRecognition-macOS.dmg}"
APP_NAME="Voice Recognition.app"

[ "$(uname -s)" = "Darwin" ] || { echo "This installer is for macOS only." >&2; exit 1; }
major="$(sw_vers -productVersion | cut -d. -f1)"
[ "$major" -ge 13 ] || { echo "macOS 13 or newer is required (this is $(sw_vers -productVersion))." >&2; exit 1; }

TMP="$(mktemp -d)"
MNT="$TMP/mnt"
cleanup() { hdiutil detach "$MNT" -quiet >/dev/null 2>&1 || true; rm -rf "$TMP"; }
trap cleanup EXIT

echo "Downloading $URL"
curl -fL --progress-bar -o "$TMP/app.dmg" "$URL"

if [ -n "$SHA256" ]; then
  actual="$(shasum -a 256 "$TMP/app.dmg" | cut -d' ' -f1)"
  if [ "$actual" != "$SHA256" ]; then
    echo "Checksum mismatch: expected $SHA256, got $actual. Nothing was installed." >&2
    exit 1
  fi
  echo "Checksum OK."
fi

mkdir -p "$MNT"
hdiutil attach "$TMP/app.dmg" -nobrowse -readonly -quiet -mountpoint "$MNT"
[ -d "$MNT/$APP_NAME" ] || { echo "The disk image does not contain $APP_NAME." >&2; exit 1; }

DEST="/Applications"
[ -w "$DEST" ] || { DEST="$HOME/Applications"; mkdir -p "$DEST"; }
rm -rf "$DEST/$APP_NAME"
ditto "$MNT/$APP_NAME" "$DEST/$APP_NAME"
xattr -cr "$DEST/$APP_NAME" 2>/dev/null || true
echo "Installed: $DEST/$APP_NAME"

# the `voice-recognition` command
BIN="$HOME/.local/bin"
mkdir -p "$BIN"
cat > "$BIN/voice-recognition" <<EOF
#!/bin/sh
exec open "$DEST/$APP_NAME"
EOF
chmod +x "$BIN/voice-recognition"
case ":$PATH:" in
  *":$BIN:"*) ;;
  *) echo "Add $BIN to your PATH to use the command:  echo 'export PATH=\"\$HOME/.local/bin:\$PATH\"' >> ~/.zshrc" ;;
esac

echo
echo "Installed. Starting the program now - its window opens in a few seconds."
echo "  - The first start sets itself up (Python, packages, speech models: a few GB, 10-30 minutes)."
echo "    The progress is shown at the bottom of the window. Keep it open and the Mac awake."
echo "  - When it says Ready: press \"+ New session\", then Record, and allow the microphone."
echo "  - Next time start it from Applications or with:  voice-recognition"
open "$DEST/$APP_NAME" || echo "Could not open it automatically: open \"$APP_NAME\" from Applications."
