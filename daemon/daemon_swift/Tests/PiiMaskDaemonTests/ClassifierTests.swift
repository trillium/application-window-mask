import XCTest
@testable import PiiMaskDaemon

final class ClassifierTests: XCTestCase {

    private func makeClassifier(
        allow: Set<String> = ["OBS", "Terminal", "Finder"],
        alwaysMask: Set<String> = ["NotificationCenter", "SecurityAgent"]
    ) -> Classifier {
        Classifier(allow: allow, alwaysMask: alwaysMask)
    }

    // MARK: - 1. Default deny: unknown app is unsafe

    func testUnknownAppIsUnsafe() {
        let clf = makeClassifier()
        XCTAssertTrue(clf.isUnsafe(owner: "SomeRandomApp"))
    }

    // MARK: - 2. Safe list: app in allow list is safe

    func testSafeAppIsNotUnsafe() {
        let clf = makeClassifier()
        XCTAssertFalse(clf.isUnsafe(owner: "OBS"))
        XCTAssertFalse(clf.isUnsafe(owner: "Terminal"))
        XCTAssertFalse(clf.isUnsafe(owner: "Finder"))
    }

    // MARK: - 3. Always-mask overrides allow list

    func testAlwaysMaskIsUnsafe() {
        let clf = makeClassifier()
        XCTAssertTrue(clf.isUnsafe(owner: "NotificationCenter"))
        XCTAssertTrue(clf.isUnsafe(owner: "SecurityAgent"))
    }

    func testAlwaysMaskBeatsAllow() {
        let clf = makeClassifier(
            allow: ["OBS", "NotificationCenter"],
            alwaysMask: ["NotificationCenter"]
        )
        XCTAssertTrue(clf.isUnsafe(owner: "NotificationCenter"))
    }

    // MARK: - 4. Negative layer: always safe regardless of owner

    func testNegativeLayerIsSafe() {
        let clf = makeClassifier()
        XCTAssertFalse(clf.isUnsafe(owner: "SomeRandomApp", layer: -1))
        XCTAssertFalse(clf.isUnsafe(owner: "NotificationCenter", layer: -1))
        XCTAssertFalse(clf.isUnsafe(owner: "", layer: -100))
    }

    // MARK: - 5. Layer > 0 from unknown app: unsafe

    func testOverlayFromUnknownAppIsUnsafe() {
        let clf = makeClassifier()
        XCTAssertTrue(clf.isUnsafe(owner: "UnknownOverlay", layer: 1))
        XCTAssertTrue(clf.isUnsafe(owner: "UnknownOverlay", layer: 5))
    }

    // MARK: - 6. Layer > 0 from safe app: safe

    func testOverlayFromSafeAppIsSafe() {
        let clf = makeClassifier()
        XCTAssertFalse(clf.isUnsafe(owner: "OBS", layer: 1))
        XCTAssertFalse(clf.isUnsafe(owner: "Terminal", layer: 3))
    }

    // MARK: - 7. Empty owner string: unsafe

    func testEmptyOwnerIsUnsafe() {
        let clf = makeClassifier()
        XCTAssertTrue(clf.isUnsafe(owner: ""))
        XCTAssertTrue(clf.isUnsafe(owner: "", layer: 0))
        XCTAssertTrue(clf.isUnsafe(owner: "", layer: 1))
    }

    // MARK: - 8. updateLists: hot-reload changes classification

    func testUpdateListsChangesClassification() {
        let clf = makeClassifier()
        // Before: Slack is unknown → unsafe
        XCTAssertTrue(clf.isUnsafe(owner: "Slack"))

        clf.updateLists(
            allow: ["OBS", "Terminal", "Finder", "Slack"],
            alwaysMask: ["NotificationCenter", "SecurityAgent"]
        )
        // After: Slack is safe
        XCTAssertFalse(clf.isUnsafe(owner: "Slack"))
    }

    func testUpdateListsCanRevokeSafety() {
        let clf = makeClassifier()
        // Terminal starts safe
        XCTAssertFalse(clf.isUnsafe(owner: "Terminal"))

        clf.updateLists(
            allow: ["OBS", "Finder"],
            alwaysMask: ["NotificationCenter", "SecurityAgent"]
        )
        // Terminal no longer in allow → unsafe
        XCTAssertTrue(clf.isUnsafe(owner: "Terminal"))
    }

    // MARK: - 9. Config-driven init: custom sets

    func testCustomInitSets() {
        let clf = makeClassifier(allow: ["MyApp"], alwaysMask: ["BadApp"])
        XCTAssertFalse(clf.isUnsafe(owner: "MyApp"))
        XCTAssertTrue(clf.isUnsafe(owner: "BadApp"))
        XCTAssertTrue(clf.isUnsafe(owner: "OtherApp"))
    }

    func testEmptyInitSets() {
        let clf = makeClassifier(allow: [], alwaysMask: [])
        // Empty allow → everything unsafe by default deny
        XCTAssertTrue(clf.isUnsafe(owner: "OBS"))
        XCTAssertTrue(clf.isUnsafe(owner: "Terminal"))
        // Negative layer still safe
        XCTAssertFalse(clf.isUnsafe(owner: "OBS", layer: -1))
    }
}
