"""Tests for occlusion math."""
import pytest
from occlusion import subtract_rect, subtract_region, compute_visible_unsafe


def total_area(rects):
    """Sum the areas of a list of (x, y, w, h) rects."""
    return sum(w * h for _, _, w, h in rects)


# ---------------------------------------------------------------------------
# subtract_rect
# ---------------------------------------------------------------------------

class TestSubtractRect:
    """Tests for subtract_rect(a, b) -> list of remainder rects."""

    # -- No overlap cases --

    def test_no_overlap_right(self):
        result = subtract_rect((0, 0, 10, 10), (20, 0, 10, 10))
        assert result == [(0, 0, 10, 10)]

    def test_no_overlap_left(self):
        result = subtract_rect((20, 0, 10, 10), (0, 0, 10, 10))
        assert result == [(20, 0, 10, 10)]

    def test_no_overlap_above(self):
        result = subtract_rect((0, 20, 10, 10), (0, 0, 10, 10))
        assert result == [(0, 20, 10, 10)]

    def test_no_overlap_below(self):
        result = subtract_rect((0, 0, 10, 10), (0, 20, 10, 10))
        assert result == [(0, 0, 10, 10)]

    # -- Full overlap --

    def test_full_overlap_identical(self):
        result = subtract_rect((0, 0, 10, 10), (0, 0, 10, 10))
        assert result == []

    def test_full_overlap_b_larger(self):
        result = subtract_rect((5, 5, 10, 10), (0, 0, 20, 20))
        assert result == []

    # -- Partial overlaps: strips --

    def test_top_strip(self):
        """b covers the bottom portion of a, leaving a top strip."""
        a = (0, 0, 10, 10)
        b = (0, 5, 10, 10)  # covers y=5..15
        result = subtract_rect(a, b)
        assert result == [(0, 0, 10, 5)]
        assert total_area(result) == 50

    def test_bottom_strip(self):
        """b covers the top portion of a, leaving a bottom strip."""
        a = (0, 0, 10, 10)
        b = (0, -5, 10, 10)  # covers y=-5..5
        result = subtract_rect(a, b)
        assert result == [(0, 5, 10, 5)]
        assert total_area(result) == 50

    def test_left_strip(self):
        """b covers the right portion of a, leaving a left strip."""
        a = (0, 0, 10, 10)
        b = (5, 0, 10, 10)  # covers x=5..15
        result = subtract_rect(a, b)
        assert result == [(0, 0, 5, 10)]
        assert total_area(result) == 50

    def test_right_strip(self):
        """b covers the left portion of a, leaving a right strip."""
        a = (0, 0, 10, 10)
        b = (-5, 0, 10, 10)  # covers x=-5..5
        result = subtract_rect(a, b)
        assert result == [(5, 0, 5, 10)]
        assert total_area(result) == 50

    # -- Corner overlaps --

    def test_corner_top_left(self):
        """b overlaps top-left corner of a."""
        a = (0, 0, 10, 10)
        b = (-5, -5, 10, 10)  # covers x=-5..5, y=-5..5
        result = subtract_rect(a, b)
        # bottom strip + right strip between top/bottom
        assert total_area(result) == 75

    def test_corner_bottom_right(self):
        """b overlaps bottom-right corner of a."""
        a = (0, 0, 10, 10)
        b = (5, 5, 10, 10)
        result = subtract_rect(a, b)
        assert total_area(result) == 75

    def test_corner_top_right(self):
        a = (0, 0, 10, 10)
        b = (5, -5, 10, 10)
        result = subtract_rect(a, b)
        assert total_area(result) == 75

    def test_corner_bottom_left(self):
        a = (0, 0, 10, 10)
        b = (-5, 5, 10, 10)
        result = subtract_rect(a, b)
        assert total_area(result) == 75

    def test_center_hole_four_strips(self):
        """b sits in the center of a, producing 4 strips."""
        a = (0, 0, 20, 20)
        b = (5, 5, 10, 10)
        result = subtract_rect(a, b)
        assert len(result) == 4
        assert total_area(result) == 400 - 100  # 300

    # -- Area conservation --

    def test_area_conservation_partial(self):
        a = (10, 10, 30, 30)
        b = (20, 20, 10, 10)
        result = subtract_rect(a, b)
        assert total_area(result) == 30 * 30 - 10 * 10


# ---------------------------------------------------------------------------
# subtract_region
# ---------------------------------------------------------------------------

class TestSubtractRegion:
    """Tests for subtract_region(rect, region)."""

    def test_empty_region(self):
        result = subtract_region((0, 0, 10, 10), [])
        assert result == [(0, 0, 10, 10)]

    def test_single_cover(self):
        result = subtract_region((0, 0, 10, 10), [(0, 0, 10, 10)])
        assert result == []

    def test_multiple_covering_rects(self):
        """Two rects that together cover the original entirely."""
        rect = (0, 0, 10, 10)
        region = [(0, 0, 5, 10), (5, 0, 5, 10)]
        result = subtract_region(rect, region)
        assert result == []

    def test_complete_coverage_with_overlap(self):
        """Overlapping covers that fully cover the original."""
        rect = (0, 0, 10, 10)
        region = [(0, 0, 6, 10), (4, 0, 6, 10)]
        result = subtract_region(rect, region)
        assert result == []

    def test_partial_coverage(self):
        rect = (0, 0, 20, 10)
        region = [(5, 0, 5, 10)]  # covers x=5..10
        result = subtract_region(rect, region)
        assert total_area(result) == 200 - 50

    def test_multiple_partial_covers(self):
        rect = (0, 0, 30, 10)
        region = [(0, 0, 10, 10), (20, 0, 10, 10)]
        result = subtract_region(rect, region)
        assert total_area(result) == 100  # middle strip


# ---------------------------------------------------------------------------
# compute_visible_unsafe
# ---------------------------------------------------------------------------

def make_window(x, y, w, h, unsafe):
    return {"x": x, "y": y, "width": w, "height": h, "unsafe": unsafe}


class TestComputeVisibleUnsafe:
    """Tests for compute_visible_unsafe(windows)."""

    def test_single_unsafe_window(self):
        windows = [make_window(0, 0, 100, 100, True)]
        result = compute_visible_unsafe(windows)
        assert result == [(0, 0, 100, 100)]

    def test_single_safe_window(self):
        windows = [make_window(0, 0, 100, 100, False)]
        result = compute_visible_unsafe(windows)
        assert result == []

    def test_unsafe_behind_safe_fully_covered(self):
        """Safe window in front fully covers unsafe window behind it."""
        windows = [
            make_window(0, 0, 100, 100, False),  # front (safe)
            make_window(0, 0, 100, 100, True),    # back (unsafe)
        ]
        result = compute_visible_unsafe(windows)
        assert result == []

    def test_unsafe_behind_safe_partially_covered(self):
        """Safe window partially covers unsafe window behind it."""
        windows = [
            make_window(0, 0, 50, 100, False),   # front-left safe
            make_window(0, 0, 100, 100, True),    # back unsafe
        ]
        result = compute_visible_unsafe(windows)
        assert total_area(result) == 5000  # right half visible

    def test_unsafe_in_front_of_safe(self):
        """Unsafe window in front, safe behind -- unsafe is fully visible."""
        windows = [
            make_window(0, 0, 100, 100, True),   # front (unsafe)
            make_window(0, 0, 100, 100, False),   # back (safe)
        ]
        result = compute_visible_unsafe(windows)
        assert total_area(result) == 10000

    def test_multiple_layers(self):
        """Safe-unsafe-unsafe stack, front to back."""
        windows = [
            make_window(0, 0, 50, 100, False),   # front safe covers left half
            make_window(0, 0, 100, 100, True),    # middle unsafe
            make_window(50, 0, 100, 100, True),   # back unsafe
        ]
        result = compute_visible_unsafe(windows)
        # Middle unsafe: visible part is right half (50*100 = 5000)
        # Back unsafe: covered by safe (left 50) and middle unsafe (0..100),
        #   its rect is x=50..150, covered portion from middle is x=50..100.
        #   visible part is x=100..150 = 50*100 = 5000
        assert total_area(result) == 10000

    def test_all_safe_windows(self):
        windows = [
            make_window(0, 0, 100, 100, False),
            make_window(50, 50, 100, 100, False),
        ]
        result = compute_visible_unsafe(windows)
        assert result == []

    def test_all_unsafe_windows_no_overlap(self):
        windows = [
            make_window(0, 0, 50, 50, True),
            make_window(100, 100, 50, 50, True),
        ]
        result = compute_visible_unsafe(windows)
        assert total_area(result) == 5000

    def test_all_unsafe_windows_with_overlap(self):
        """Two overlapping unsafe windows, front-to-back."""
        windows = [
            make_window(0, 0, 100, 100, True),
            make_window(50, 50, 100, 100, True),
        ]
        result = compute_visible_unsafe(windows)
        # Front: full 10000
        # Back: its rect minus front rect
        front_area = 10000
        back_visible = 100 * 100 - 50 * 50  # 7500
        assert total_area(result) == front_area + back_visible

    def test_sandwich_safe_between_unsafe(self):
        """Unsafe, then safe, then unsafe behind -- safe blocks the back unsafe."""
        windows = [
            make_window(0, 0, 100, 100, True),    # front unsafe
            make_window(0, 0, 100, 100, False),    # middle safe (same area)
            make_window(0, 0, 100, 100, True),     # back unsafe
        ]
        result = compute_visible_unsafe(windows)
        # Only the front unsafe is visible
        assert total_area(result) == 10000

    def test_empty_window_list(self):
        assert compute_visible_unsafe([]) == []


# ---------------------------------------------------------------------------
# Edge cases
# ---------------------------------------------------------------------------

class TestEdgeCases:
    def test_zero_width_rect(self):
        result = subtract_rect((0, 0, 0, 10), (0, 0, 10, 10))
        # Zero-width a: ox1=max(0,0)=0, ox2=min(0,10)=0 -> ox1>=ox2 -> no overlap
        assert result == [(0, 0, 0, 10)]

    def test_zero_height_rect(self):
        result = subtract_rect((0, 0, 10, 0), (0, 0, 10, 10))
        assert result == [(0, 0, 10, 0)]

    def test_zero_size_b(self):
        result = subtract_rect((0, 0, 10, 10), (5, 5, 0, 0))
        assert result == [(0, 0, 10, 10)]

    def test_adjacent_rects_no_overlap(self):
        """Rects touching at an edge should not overlap."""
        result = subtract_rect((0, 0, 10, 10), (10, 0, 10, 10))
        assert result == [(0, 0, 10, 10)]

    def test_adjacent_rects_vertical(self):
        result = subtract_rect((0, 0, 10, 10), (0, 10, 10, 10))
        assert result == [(0, 0, 10, 10)]

    def test_identical_rects(self):
        result = subtract_rect((5, 5, 20, 20), (5, 5, 20, 20))
        assert result == []

    def test_negative_coordinates(self):
        a = (-10, -10, 20, 20)
        b = (-5, -5, 10, 10)
        result = subtract_rect(a, b)
        assert total_area(result) == 400 - 100

    def test_float_coordinates(self):
        a = (0.5, 0.5, 10.0, 10.0)
        b = (5.5, 5.5, 10.0, 10.0)
        result = subtract_rect(a, b)
        assert len(result) > 0
        assert total_area(result) == pytest.approx(100.0 - 25.0)
