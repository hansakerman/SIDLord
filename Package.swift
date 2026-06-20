// swift-tools-version: 6.1
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "SIDLord",
    platforms: [
        .macOS(.v14)
    ],
    targets: [
        .target(
            name: "SIDCore",
            publicHeadersPath: "include",
            cxxSettings: [
                .define("VERSION", to: "\"SIDLord-reSID\"")
            ]
        ),
        .executableTarget(
            name: "SIDLord",
            dependencies: ["SIDCore"],
            resources: [
                .process("Resources")
            ]
        ),
        .testTarget(
            name: "SIDCoreCompatibilityTests",
            dependencies: ["SIDCore"]
        ),
    ]
)
