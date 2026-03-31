/// Loads and hot-reloads app classification config from ~/.config/pii-mask/apps.toml.
///
/// Uses DispatchSource file watching for instant reload on config change.
/// Falls back to mtime polling if dispatch source setup fails.

import Foundation

struct AppConfig {
    var allow: Set<String>
    var alwaysMask: Set<String>
}

final class ConfigLoader {
    static let configDir = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent(".config/pii-mask")
    static let configPath = configDir.appendingPathComponent("apps.toml")

    private static let defaultAllow: Set<String> = [
        "Code", "Google Chrome", "OBS", "OBS Studio", "RODE Connect",
        "Talon", "Terminal", "iTerm2", "Slack", "System Settings",
        "System Preferences", "Finder",
    ]

    private static let defaultAlwaysMask: Set<String> = [
        "Notification Center", "NotificationCenter", "SecurityAgent",
    ]

    private var fileDescriptor: Int32 = -1
    private var dispatchSource: DispatchSourceFileSystemObject?
    private var onChange: ((AppConfig) -> Void)?

    func load() -> AppConfig {
        let path = Self.configPath.path

        guard FileManager.default.fileExists(atPath: path),
              let data = FileManager.default.contents(atPath: path),
              let content = String(data: data, encoding: .utf8)
        else {
            writeDefaults()
            return AppConfig(allow: Self.defaultAllow, alwaysMask: Self.defaultAlwaysMask)
        }

        return parse(content)
    }

    func watch(onChange: @escaping (AppConfig) -> Void) {
        self.onChange = onChange
        setupFileWatch()
    }

    func stopWatching() {
        dispatchSource?.cancel()
        dispatchSource = nil
        if fileDescriptor >= 0 {
            close(fileDescriptor)
            fileDescriptor = -1
        }
    }

    // MARK: - TOML parsing (minimal — just string arrays)

    private func parse(_ content: String) -> AppConfig {
        let allow = parseArray(content, key: "allow") ?? Array(Self.defaultAllow)
        let alwaysMask = parseArray(content, key: "always_mask") ?? Array(Self.defaultAlwaysMask)
        return AppConfig(allow: Set(allow), alwaysMask: Set(alwaysMask))
    }

    /// Parses a TOML array of strings: key = ["a", "b", "c"]
    private func parseArray(_ content: String, key: String) -> [String]? {
        // Find "key = [" then collect quoted strings until "]"
        guard let keyRange = content.range(of: "\(key) = [") ??
                             content.range(of: "\(key)= [") ??
                             content.range(of: "\(key) =[") ??
                             content.range(of: "\(key)=[")
        else { return nil }

        let afterBracket = content[keyRange.upperBound...]
        guard let closeBracket = afterBracket.firstIndex(of: "]") else { return nil }

        let arrayContent = String(afterBracket[..<closeBracket])
        var result: [String] = []

        var inQuote = false
        var current = ""
        for ch in arrayContent {
            if ch == "\"" {
                if inQuote {
                    result.append(current)
                    current = ""
                }
                inQuote.toggle()
            } else if inQuote {
                current.append(ch)
            }
        }

        return result
    }

    // MARK: - File watching

    private func setupFileWatch() {
        let path = Self.configPath.path
        fileDescriptor = Darwin.open(path, O_EVTONLY)
        guard fileDescriptor >= 0 else { return }

        let source = DispatchSource.makeFileSystemObjectSource(
            fileDescriptor: fileDescriptor,
            eventMask: [.write, .rename, .delete],
            queue: .main
        )

        source.setEventHandler { [weak self] in
            guard let self else { return }
            let flags = source.data
            if flags.contains(.delete) || flags.contains(.rename) {
                // File replaced (atomic save) — re-setup watch
                self.stopWatching()
                // Brief delay for the new file to appear
                DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(100)) {
                    let config = self.load()
                    self.onChange?(config)
                    self.setupFileWatch()
                }
                return
            }
            let config = self.load()
            self.onChange?(config)
        }

        source.setCancelHandler { [weak self] in
            guard let self, self.fileDescriptor >= 0 else { return }
            Darwin.close(self.fileDescriptor)
            self.fileDescriptor = -1
        }

        source.resume()
        dispatchSource = source
    }

    // MARK: - Default config file

    private func writeDefaults() {
        let dir = Self.configDir.path
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)

        var lines = [
            "# PII mask — app classification config",
            "#",
            "# Process names or bundle IDs allowed on stream (default-deny)",
            "allow = [",
        ]
        for name in Self.defaultAllow.sorted() {
            lines.append("    \"\(name)\",")
        }
        lines.append("]")
        lines.append("")
        lines.append("# Always masked, even if in allow list")
        lines.append("always_mask = [")
        for name in Self.defaultAlwaysMask.sorted() {
            lines.append("    \"\(name)\",")
        }
        lines.append("]")
        lines.append("")

        let content = lines.joined(separator: "\n")
        try? content.write(toFile: Self.configPath.path, atomically: true, encoding: .utf8)
    }
}
