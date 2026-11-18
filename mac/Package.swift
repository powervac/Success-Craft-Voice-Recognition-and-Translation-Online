// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "VoiceRecognition",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(name: "VoiceRecognition", path: "Sources/VoiceRecognition")
    ]
)
