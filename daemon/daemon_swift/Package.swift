// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "pii-mask-daemon",
    platforms: [.macOS(.v13)],
    targets: [
        .target(name: "CSeqlock"),
        .executableTarget(
            name: "PiiMaskDaemon",
            dependencies: ["CSeqlock"]
        ),
        .testTarget(
            name: "PiiMaskDaemonTests",
            dependencies: ["PiiMaskDaemon"]
        ),
    ]
)
