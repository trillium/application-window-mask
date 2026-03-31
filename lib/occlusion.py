"""
Z-order aware occlusion calculation.

Walks windows front-to-back, tracking which screen regions are covered
by safe windows. Only emits mask rects for unsafe window regions that
are actually visible (not hidden behind safe windows).
"""


def subtract_rect(a, b):
    """
    Subtract rect b from rect a. Returns 0-4 remainder rects.

    Each rect is (x, y, w, h).
    """
    ax, ay, aw, ah = a
    bx, by, bw, bh = b

    # Compute overlap
    ox1 = max(ax, bx)
    oy1 = max(ay, by)
    ox2 = min(ax + aw, bx + bw)
    oy2 = min(ay + ah, by + bh)

    if ox1 >= ox2 or oy1 >= oy2:
        return [a]  # no overlap

    result = []

    # Top strip
    if ay < oy1:
        result.append((ax, ay, aw, oy1 - ay))

    # Bottom strip
    if ay + ah > oy2:
        result.append((ax, oy2, aw, (ay + ah) - oy2))

    # Left strip (between top and bottom)
    if ax < ox1:
        result.append((ax, oy1, ox1 - ax, oy2 - oy1))

    # Right strip (between top and bottom)
    if ax + aw > ox2:
        result.append((ox2, oy1, (ax + aw) - ox2, oy2 - oy1))

    return result


def subtract_region(rect, region):
    """
    Subtract a region (list of rects) from a single rect.

    Returns the visible fragments of rect not covered by region.
    """
    fragments = [rect]
    for cover_rect in region:
        new_fragments = []
        for frag in fragments:
            new_fragments.extend(subtract_rect(frag, cover_rect))
        fragments = new_fragments
        if not fragments:
            break
    return fragments


def compute_visible_unsafe(windows):
    """
    Given windows in front-to-back z-order, return rects where unsafe
    windows are the topmost visible layer.

    Each window dict must have: x, y, width, height, unsafe (bool).

    Returns a list of (x, y, w, h) tuples for regions to mask.
    """
    covered = []
    unsafe_rects = []

    for w in windows:
        rect = (w["x"], w["y"], w["width"], w["height"])

        if w["unsafe"]:
            # Visible unsafe = this rect minus everything already covered
            visible = subtract_region(rect, covered)
            unsafe_rects.extend(visible)

        # This window now covers its region regardless of safe/unsafe
        # Add only the uncovered portion to avoid duplicates
        new_coverage = subtract_region(rect, covered)
        covered.extend(new_coverage)

    return unsafe_rects
