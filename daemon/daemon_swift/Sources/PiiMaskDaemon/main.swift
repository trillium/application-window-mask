/// pii-mask-daemon — Swift rewrite.
///
/// Monitors macOS windows, classifies them as safe/unsafe, and writes
/// mask geometry to shared memory for the OBS plugin to read.
///
/// Event-driven with 5Hz fallback polling.
/// Expected: ~2ms per poll vs 11.8ms in Python.

import AppKit
import Darwin

// MARK: - Screen dimensions

let mainDisplay = CGMainDisplayID()
let screenW = UInt16(CGDisplayPixelsWide(mainDisplay))
let screenH = UInt16(CGDisplayPixelsHigh(mainDisplay))

// MARK: - Initialize components

let configLoader = ConfigLoader()
let config = configLoader.load()

let classifier = Classifier(allow: config.allow, alwaysMask: config.alwaysMask)
let poller = WindowPoller()
let sceneModel = SceneModel()
let shmWriter = ShmWriter()
let eventMonitor = EventMonitor()

do {
    try shmWriter.open(screenWidth: screenW, screenHeight: screenH)
} catch {
    fputs("Fatal: failed to open shm: \(error)\n", stderr)
    exit(1)
}

print("Screen: \(screenW)x\(screenH)")
print("Config: \(config.allow.count) allowed, \(config.alwaysMask.count) always-masked")

// Fail-safe: start fully masked
shmWriter.writeFullMask()

// MARK: - Pipeline

/// Benchmark accumulators
var benchFrame = 0
let benchInterval = 300  // log every ~10s at 30Hz, ~60s at 5Hz
var benchPoll: [Double] = []
var benchClassify: [Double] = []
var benchOcclusion: [Double] = []
var benchWrite: [Double] = []
var benchTotal: [Double] = []

func runPipeline() {
    let loopStart = ContinuousClock.now

    // Poll all on-screen windows
    let t0 = ContinuousClock.now
    let windows = poller.poll()
    let tPoll = ContinuousClock.now - t0

    // Classify (layer 0 only — same as Python daemon)
    let t1 = ContinuousClock.now
    var classified: [ClassifiedWindow] = []
    for w in windows {
        if w.layer != 0 { continue }
        classified.append(ClassifiedWindow(
            x: w.x, y: w.y, width: w.width, height: w.height,
            unsafe: classifier.isUnsafe(owner: w.owner, title: w.title, layer: w.layer)
        ))
    }
    let tClassify = ContinuousClock.now - t1

    // Compute visible unsafe regions (z-order aware)
    let t2 = ContinuousClock.now
    let visibleUnsafe = computeVisibleUnsafe(classified)
    let tOcclusion = ContinuousClock.now - t2

    // Clip to screen bounds and build mask rects
    var rects: [MaskRect] = []
    for r in visibleUnsafe {
        var x = r.x, y = r.y, w = r.w, h = r.h
        if x < 0 { w += x; x = 0 }
        if y < 0 { h += y; y = 0 }
        if w <= 1 || h <= 1 { continue }
        rects.append(MaskRect(
            x: x, y: y, width: w, height: h,
            cornerRadius: 0, flags: rectFlagUnsafe
        ))
    }

    // Write to shm if changed, or heartbeat
    let t3 = ContinuousClock.now
    if sceneModel.update(rects) {
        shmWriter.writeRects(rects)
    } else {
        shmWriter.heartbeat()
    }
    let tWrite = ContinuousClock.now - t3

    let tTotal = ContinuousClock.now - loopStart

    // Benchmark accumulation
    benchPoll.append(tPoll.milliseconds)
    benchClassify.append(tClassify.milliseconds)
    benchOcclusion.append(tOcclusion.milliseconds)
    benchWrite.append(tWrite.milliseconds)
    benchTotal.append(tTotal.milliseconds)
    benchFrame += 1

    if benchFrame >= benchInterval {
        logBench("poll", benchPoll)
        logBench("classify", benchClassify)
        logBench("occlusion", benchOcclusion)
        logBench("write", benchWrite)
        logBench("total", benchTotal)
        print("  BENCH windows=\(windows.count) classified=\(classified.count) rects=\(rects.count)")

        benchPoll.removeAll(keepingCapacity: true)
        benchClassify.removeAll(keepingCapacity: true)
        benchOcclusion.removeAll(keepingCapacity: true)
        benchWrite.removeAll(keepingCapacity: true)
        benchTotal.removeAll(keepingCapacity: true)
        benchFrame = 0
    }
}

func logBench(_ name: String, _ values: [Double]) {
    let sorted = values.sorted()
    let n = sorted.count
    let p50 = sorted[n / 2]
    let p99 = sorted[Int(Double(n) * 0.99)]
    let avg = sorted.reduce(0, +) / Double(n)
    let padded = name.padding(toLength: 10, withPad: " ", startingAt: 0)
    print("  BENCH \(padded): avg=\(String(format: "%.3f", avg))ms  p50=\(String(format: "%.3f", p50))ms  p99=\(String(format: "%.3f", p99))ms")
}

// MARK: - Duration extension

extension Duration {
    var milliseconds: Double {
        let (seconds, attoseconds) = self.components
        return Double(seconds) * 1000.0 + Double(attoseconds) / 1_000_000_000_000_000.0
    }
}

// MARK: - Signal handling

signal(SIGTERM, SIG_IGN)
signal(SIGINT, SIG_IGN)
signal(SIGHUP, SIG_IGN)

let sigTermSource = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
sigTermSource.setEventHandler {
    print("pii-mask-daemon shutting down (SIGTERM)")
    shmWriter.close()
    exit(0)
}
sigTermSource.resume()

let sigIntSource = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
sigIntSource.setEventHandler {
    print("pii-mask-daemon shutting down (SIGINT)")
    shmWriter.close()
    exit(0)
}
sigIntSource.resume()

let sigHupSource = DispatchSource.makeSignalSource(signal: SIGHUP, queue: .main)
sigHupSource.setEventHandler {
    print("Config reload requested (SIGHUP)")
    let newConfig = configLoader.load()
    classifier.updateLists(allow: newConfig.allow, alwaysMask: newConfig.alwaysMask)
    print("Config reloaded: \(newConfig.allow.count) allowed, \(newConfig.alwaysMask.count) always-masked")
    runPipeline()  // immediate re-classify
}
sigHupSource.resume()

// MARK: - Config hot-reload via file watch

configLoader.watch { newConfig in
    classifier.updateLists(allow: newConfig.allow, alwaysMask: newConfig.alwaysMask)
    print("Config reloaded: \(newConfig.allow.count) allowed, \(newConfig.alwaysMask.count) always-masked")
    runPipeline()
}

// MARK: - Event-driven monitoring

eventMonitor.start { runPipeline() }

// MARK: - 5Hz fallback timer

let fallbackTimer = DispatchSource.makeTimerSource(queue: .main)
fallbackTimer.schedule(deadline: .now(), repeating: .milliseconds(200))  // 5Hz
fallbackTimer.setEventHandler { runPipeline() }
fallbackTimer.resume()

print("pii-mask-daemon started (event-driven + 5Hz fallback)")

// Run the main loop
RunLoop.main.run()
