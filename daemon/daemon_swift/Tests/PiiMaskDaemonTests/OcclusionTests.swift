import XCTest
@testable import PiiMaskDaemon

// MARK: - Helpers

func totalArea(_ rects: [Rect]) -> Float {
    rects.reduce(0) { $0 + $1.w * $1.h }
}

func rectEqual(_ a: Rect, _ b: Rect) -> Bool {
    a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h
}

func rectsEqual(_ a: [Rect], _ b: [Rect]) -> Bool {
    guard a.count == b.count else { return false }
    return zip(a, b).allSatisfy { rectEqual($0, $1) }
}

func win(_ x: Float, _ y: Float, _ w: Float, _ h: Float, unsafe: Bool) -> ClassifiedWindow {
    ClassifiedWindow(x: x, y: y, width: w, height: h, unsafe: unsafe)
}

// MARK: - subtractRect tests

final class SubtractRectTests: XCTestCase {

    // -- No overlap --

    func testNoOverlapRight() {
        let result = subtractRect((0, 0, 10, 10), (20, 0, 10, 10))
        XCTAssertTrue(rectsEqual(result, [(0, 0, 10, 10)]))
    }

    func testNoOverlapLeft() {
        let result = subtractRect((20, 0, 10, 10), (0, 0, 10, 10))
        XCTAssertTrue(rectsEqual(result, [(20, 0, 10, 10)]))
    }

    func testNoOverlapAbove() {
        let result = subtractRect((0, 20, 10, 10), (0, 0, 10, 10))
        XCTAssertTrue(rectsEqual(result, [(0, 20, 10, 10)]))
    }

    func testNoOverlapBelow() {
        let result = subtractRect((0, 0, 10, 10), (0, 20, 10, 10))
        XCTAssertTrue(rectsEqual(result, [(0, 0, 10, 10)]))
    }

    // -- Full overlap --

    func testFullOverlapIdentical() {
        let result = subtractRect((0, 0, 10, 10), (0, 0, 10, 10))
        XCTAssertTrue(result.isEmpty)
    }

    func testFullOverlapBLarger() {
        let result = subtractRect((5, 5, 10, 10), (0, 0, 20, 20))
        XCTAssertTrue(result.isEmpty)
    }

    // -- Partial overlaps: strips --

    func testTopStrip() {
        let result = subtractRect((0, 0, 10, 10), (0, 5, 10, 10))
        XCTAssertTrue(rectsEqual(result, [(0, 0, 10, 5)]))
        XCTAssertEqual(totalArea(result), 50)
    }

    func testBottomStrip() {
        let result = subtractRect((0, 0, 10, 10), (0, -5, 10, 10))
        XCTAssertTrue(rectsEqual(result, [(0, 5, 10, 5)]))
        XCTAssertEqual(totalArea(result), 50)
    }

    func testLeftStrip() {
        let result = subtractRect((0, 0, 10, 10), (5, 0, 10, 10))
        XCTAssertTrue(rectsEqual(result, [(0, 0, 5, 10)]))
        XCTAssertEqual(totalArea(result), 50)
    }

    func testRightStrip() {
        let result = subtractRect((0, 0, 10, 10), (-5, 0, 10, 10))
        XCTAssertTrue(rectsEqual(result, [(5, 0, 5, 10)]))
        XCTAssertEqual(totalArea(result), 50)
    }

    // -- Corner overlaps --

    func testCornerTopLeft() {
        let result = subtractRect((0, 0, 10, 10), (-5, -5, 10, 10))
        XCTAssertEqual(totalArea(result), 75)
    }

    func testCornerBottomRight() {
        let result = subtractRect((0, 0, 10, 10), (5, 5, 10, 10))
        XCTAssertEqual(totalArea(result), 75)
    }

    func testCornerTopRight() {
        let result = subtractRect((0, 0, 10, 10), (5, -5, 10, 10))
        XCTAssertEqual(totalArea(result), 75)
    }

    func testCornerBottomLeft() {
        let result = subtractRect((0, 0, 10, 10), (-5, 5, 10, 10))
        XCTAssertEqual(totalArea(result), 75)
    }

    func testCenterHoleFourStrips() {
        let result = subtractRect((0, 0, 20, 20), (5, 5, 10, 10))
        XCTAssertEqual(result.count, 4)
        XCTAssertEqual(totalArea(result), 300)
    }

    func testAreaConservationPartial() {
        let result = subtractRect((10, 10, 30, 30), (20, 20, 10, 10))
        XCTAssertEqual(totalArea(result), 30 * 30 - 10 * 10)
    }
}

// MARK: - subtractRegion tests

final class SubtractRegionTests: XCTestCase {

    func testEmptyRegion() {
        let result = subtractRegion((0, 0, 10, 10), [])
        XCTAssertTrue(rectsEqual(result, [(0, 0, 10, 10)]))
    }

    func testSingleCover() {
        let result = subtractRegion((0, 0, 10, 10), [(0, 0, 10, 10)])
        XCTAssertTrue(result.isEmpty)
    }

    func testMultipleCoveringRects() {
        let result = subtractRegion((0, 0, 10, 10), [(0, 0, 5, 10), (5, 0, 5, 10)])
        XCTAssertTrue(result.isEmpty)
    }

    func testCompleteCoverageWithOverlap() {
        let result = subtractRegion((0, 0, 10, 10), [(0, 0, 6, 10), (4, 0, 6, 10)])
        XCTAssertTrue(result.isEmpty)
    }

    func testPartialCoverage() {
        let result = subtractRegion((0, 0, 20, 10), [(5, 0, 5, 10)])
        XCTAssertEqual(totalArea(result), 150)
    }

    func testMultiplePartialCovers() {
        let result = subtractRegion((0, 0, 30, 10), [(0, 0, 10, 10), (20, 0, 10, 10)])
        XCTAssertEqual(totalArea(result), 100)
    }
}

// MARK: - computeVisibleUnsafe tests

final class ComputeVisibleUnsafeTests: XCTestCase {

    func testSingleUnsafeWindow() {
        let result = computeVisibleUnsafe([win(0, 0, 100, 100, unsafe: true)])
        XCTAssertTrue(rectsEqual(result, [(0, 0, 100, 100)]))
    }

    func testSingleSafeWindow() {
        let result = computeVisibleUnsafe([win(0, 0, 100, 100, unsafe: false)])
        XCTAssertTrue(result.isEmpty)
    }

    func testUnsafeBehindSafeFullyCovered() {
        let result = computeVisibleUnsafe([
            win(0, 0, 100, 100, unsafe: false),
            win(0, 0, 100, 100, unsafe: true),
        ])
        XCTAssertTrue(result.isEmpty)
    }

    func testUnsafeBehindSafePartiallyCovered() {
        let result = computeVisibleUnsafe([
            win(0, 0, 50, 100, unsafe: false),
            win(0, 0, 100, 100, unsafe: true),
        ])
        XCTAssertEqual(totalArea(result), 5000)
    }

    func testUnsafeInFrontOfSafe() {
        let result = computeVisibleUnsafe([
            win(0, 0, 100, 100, unsafe: true),
            win(0, 0, 100, 100, unsafe: false),
        ])
        XCTAssertEqual(totalArea(result), 10000)
    }

    func testMultipleLayers() {
        let result = computeVisibleUnsafe([
            win(0, 0, 50, 100, unsafe: false),
            win(0, 0, 100, 100, unsafe: true),
            win(50, 0, 100, 100, unsafe: true),
        ])
        XCTAssertEqual(totalArea(result), 10000)
    }

    func testAllSafeWindows() {
        let result = computeVisibleUnsafe([
            win(0, 0, 100, 100, unsafe: false),
            win(50, 50, 100, 100, unsafe: false),
        ])
        XCTAssertTrue(result.isEmpty)
    }

    func testAllUnsafeNoOverlap() {
        let result = computeVisibleUnsafe([
            win(0, 0, 50, 50, unsafe: true),
            win(100, 100, 50, 50, unsafe: true),
        ])
        XCTAssertEqual(totalArea(result), 5000)
    }

    func testAllUnsafeWithOverlap() {
        let result = computeVisibleUnsafe([
            win(0, 0, 100, 100, unsafe: true),
            win(50, 50, 100, 100, unsafe: true),
        ])
        XCTAssertEqual(totalArea(result), 10000 + 7500)
    }

    func testSandwichSafeBetweenUnsafe() {
        let result = computeVisibleUnsafe([
            win(0, 0, 100, 100, unsafe: true),
            win(0, 0, 100, 100, unsafe: false),
            win(0, 0, 100, 100, unsafe: true),
        ])
        XCTAssertEqual(totalArea(result), 10000)
    }

    func testEmptyWindowList() {
        XCTAssertTrue(computeVisibleUnsafe([]).isEmpty)
    }
}

// MARK: - Edge cases

final class EdgeCaseTests: XCTestCase {

    func testZeroWidthRect() {
        let result = subtractRect((0, 0, 0, 10), (0, 0, 10, 10))
        XCTAssertTrue(rectsEqual(result, [(0, 0, 0, 10)]))
    }

    func testZeroHeightRect() {
        let result = subtractRect((0, 0, 10, 0), (0, 0, 10, 10))
        XCTAssertTrue(rectsEqual(result, [(0, 0, 10, 0)]))
    }

    func testZeroSizeB() {
        let result = subtractRect((0, 0, 10, 10), (5, 5, 0, 0))
        XCTAssertTrue(rectsEqual(result, [(0, 0, 10, 10)]))
    }

    func testAdjacentRectsNoOverlap() {
        let result = subtractRect((0, 0, 10, 10), (10, 0, 10, 10))
        XCTAssertTrue(rectsEqual(result, [(0, 0, 10, 10)]))
    }

    func testAdjacentRectsVertical() {
        let result = subtractRect((0, 0, 10, 10), (0, 10, 10, 10))
        XCTAssertTrue(rectsEqual(result, [(0, 0, 10, 10)]))
    }

    func testIdenticalRects() {
        let result = subtractRect((5, 5, 20, 20), (5, 5, 20, 20))
        XCTAssertTrue(result.isEmpty)
    }

    func testNegativeCoordinates() {
        let result = subtractRect((-10, -10, 20, 20), (-5, -5, 10, 10))
        XCTAssertEqual(totalArea(result), 300)
    }
}
