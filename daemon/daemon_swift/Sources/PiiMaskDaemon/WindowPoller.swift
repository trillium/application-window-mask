/// Polls macOS for all on-screen windows via CGWindowListCopyWindowInfo.
///
/// Direct CoreGraphics call — no PyObjC bridging overhead.
/// Expected: ~2ms avg vs 11.8ms in Python.

import CoreGraphics
import Foundation

struct WindowInfo {
    let owner: String
    let title: String
    let pid: Int32
    let layer: Int32
    let x: Float
    let y: Float
    let width: Float
    let height: Float
    let windowID: UInt32
}

final class WindowPoller {

    func poll() -> [WindowInfo] {
        guard let list = CGWindowListCopyWindowInfo(
            [.optionOnScreenOnly], kCGNullWindowID
        ) as? [[CFString: Any]] else {
            return []
        }

        var windows: [WindowInfo] = []
        windows.reserveCapacity(list.count)

        for dict in list {
            guard let boundsDict = dict[kCGWindowBounds] as? [String: Any],
                  let owner = dict[kCGWindowOwnerName] as? String,
                  !owner.isEmpty
            else { continue }

            let x = (boundsDict["X"] as? NSNumber)?.floatValue ?? 0
            let y = (boundsDict["Y"] as? NSNumber)?.floatValue ?? 0
            let w = (boundsDict["Width"] as? NSNumber)?.floatValue ?? 0
            let h = (boundsDict["Height"] as? NSNumber)?.floatValue ?? 0

            windows.append(WindowInfo(
                owner: owner,
                title: (dict[kCGWindowName] as? String) ?? "",
                pid: (dict[kCGWindowOwnerPID] as? NSNumber)?.int32Value ?? 0,
                layer: (dict[kCGWindowLayer] as? NSNumber)?.int32Value ?? 0,
                x: x, y: y, width: w, height: h,
                windowID: (dict[kCGWindowNumber] as? NSNumber)?.uint32Value ?? 0
            ))
        }

        return windows
    }
}
