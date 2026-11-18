cask "voice-recognition" do
  version "1.0.1"
  sha256 "ee9909c53c75c6ab68a709a69cf51e0a01acd202a0afa9722f685cc591548ca5"

  url "https://github.com/powervac/Success-Craft-Voice-Recognition-and-Translation-Online/releases/download/macos-v#{version}/VoiceRecognition-macOS.dmg"
  name "Voice Recognition and Translation"
  desc "Live speech recognition and text translation, fully offline"
  homepage "https://altcom.cc/voice-recognizer.html"

  depends_on macos: ">= :ventura"

  app "Voice Recognition.app"
  # `voice-recognition` starts the app from the terminal
  binary "#{appdir}/Voice Recognition.app/Contents/MacOS/VoiceRecognition", target: "voice-recognition"

  # the app is signed ad-hoc only (not notarized): drop the download quarantine so Gatekeeper does not block it
  postflight do
    system_command "/usr/bin/xattr",
                   args: ["-cr", "#{appdir}/Voice Recognition.app"],
                   sudo: false
  end

  zap trash: [
    "~/Library/Application Support/VoiceRecognition",
    "~/Library/Preferences/VoiceRecognition.plist",
  ]
end
