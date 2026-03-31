/// Z-order aware occlusion calculation.
///
/// Walks windows front-to-back, tracking which screen regions are covered
/// by safe windows. Only emits mask rects for unsafe window regions that
/// are actually visible (not hidden behind safe windows).

typealias Rect = (x: Float, y: Float, w: Float, h: Float)

/// Subtract rect `b` from rect `a`. Returns 0-4 remainder rects.
func subtractRect(_ a: Rect, _ b: Rect) -> [Rect] {
    // Compute overlap
    let ox1 = max(a.x, b.x)
    let oy1 = max(a.y, b.y)
    let ox2 = min(a.x + a.w, b.x + b.w)
    let oy2 = min(a.y + a.h, b.y + b.h)

    if ox1 >= ox2 || oy1 >= oy2 {
        return [a]  // no overlap
    }

    var result: [Rect] = []

    // Top strip
    if a.y < oy1 {
        result.append((a.x, a.y, a.w, oy1 - a.y))
    }
    // Bottom strip
    if a.y + a.h > oy2 {
        result.append((a.x, oy2, a.w, (a.y + a.h) - oy2))
    }
    // Left strip (between top and bottom)
    if a.x < ox1 {
        result.append((a.x, oy1, ox1 - a.x, oy2 - oy1))
    }
    // Right strip (between top and bottom)
    if a.x + a.w > ox2 {
        result.append((ox2, oy1, (a.x + a.w) - ox2, oy2 - oy1))
    }

    return result
}

/// Subtract a region (list of rects) from a single rect.
func subtractRegion(_ rect: Rect, _ region: [Rect]) -> [Rect] {
    var fragments = [rect]
    for coverRect in region {
        var newFragments: [Rect] = []
        for frag in fragments {
            newFragments.append(contentsOf: subtractRect(frag, coverRect))
        }
        fragments = newFragments
        if fragments.isEmpty { break }
    }
    return fragments
}

struct ClassifiedWindow {
    let x: Float
    let y: Float
    let width: Float
    let height: Float
    let unsafe: Bool
}

/// Given windows in front-to-back z-order, return rects where unsafe
/// windows are the topmost visible layer.
func computeVisibleUnsafe(_ windows: [ClassifiedWindow]) -> [Rect] {
    var covered: [Rect] = []
    var unsafeRects: [Rect] = []

    for w in windows {
        let rect: Rect = (w.x, w.y, w.width, w.height)

        if w.unsafe {
            let visible = subtractRegion(rect, covered)
            unsafeRects.append(contentsOf: visible)
        }

        let newCoverage = subtractRegion(rect, covered)
        covered.append(contentsOf: newCoverage)
    }

    return unsafeRects
}
