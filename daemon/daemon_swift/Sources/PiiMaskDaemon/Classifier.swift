/// Classifies windows as safe or unsafe based on owner name and layer.
///
/// Default-deny: everything is unsafe unless explicitly listed as safe.
/// App lists are hot-reloadable from config.

final class Classifier {
    private(set) var allowSet: Set<String>
    private(set) var alwaysMaskSet: Set<String>

    init(allow: Set<String>, alwaysMask: Set<String>) {
        self.allowSet = allow
        self.alwaysMaskSet = alwaysMask
    }

    func updateLists(allow: Set<String>, alwaysMask: Set<String>) {
        self.allowSet = allow
        self.alwaysMaskSet = alwaysMask
    }

    /// Returns true if the window should be masked (unsafe).
    ///
    /// Rules evaluated in order:
    /// 1. Negative layers → safe (desktop, wallpaper)
    /// 2. Always-mask processes → unsafe
    /// 3. Owner in safe list → safe (including overlays from safe apps)
    /// 4. Everything else → unsafe (default deny)
    func isUnsafe(owner: String, title: String = "", layer: Int32 = 0) -> Bool {
        // Rule 1: negative layers are below desktop — never mask
        if layer < 0 { return false }

        // Rule 2: always-mask processes
        if alwaysMaskSet.contains(owner) { return true }

        // Rule 3: safe list
        if allowSet.contains(owner) { return false }

        // Rule 4: default deny
        return true
    }
}
