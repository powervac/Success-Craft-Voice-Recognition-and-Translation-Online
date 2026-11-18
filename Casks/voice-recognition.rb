cask "voice-recognition" do
  version "1.0.0"
  sha256 "eecbc4744416ad4d9c7d38cdba668426fae634bbbe83f2f4a12e9621ac7ab8d7"

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
