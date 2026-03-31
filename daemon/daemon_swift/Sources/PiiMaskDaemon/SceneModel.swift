/// Tracks the current set of mask rects and detects changes.
///
/// Only triggers a shared memory write when the scene actually changed,
/// avoiding unnecessary writes when nothing moved.

final class SceneModel {
    private var lastRects: [MaskRect] = []
    private var firstUpdate = true

    /// Returns true if the rects changed (dirty), false if unchanged.
    /// Always returns true on the first call to clear startup full-mask.
    func update(_ rects: [MaskRect]) -> Bool {
        if firstUpdate {
            firstUpdate = false
            lastRects = rects
            return true
        }
        if rectsEqual(lastRects, rects) {
            return false
        }
        lastRects = rects
        return true
    }

    private func rectsEqual(_ a: [MaskRect], _ b: [MaskRect]) -> Bool {
        guard a.count == b.count else { return false }
        for (ra, rb) in zip(a, b) {
            if ra.x != rb.x || ra.y != rb.y ||
               ra.width != rb.width || ra.height != rb.height {
                return false
            }
        }
        return true
    }
}
