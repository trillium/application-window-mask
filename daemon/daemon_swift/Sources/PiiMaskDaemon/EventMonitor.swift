/// Event-driven window monitoring via NSWorkspace + AXObserver.
///
/// Instead of 30Hz polling, we react to:
/// - App activation changes (NSWorkspace notifications)
/// - App launch/termination
/// - Window move/resize (AXObserver per monitored app)
/// - 5Hz fallback poll for anything events miss

import AppKit
import ApplicationServices

final class EventMonitor {
    private var onEvent: (() -> Void)?
    private var coalescedWork: DispatchWorkItem?
    private var axObservers: [pid_t: AXObserver] = [:]

    func start(onEvent: @escaping () -> Void) {
        self.onEvent = onEvent

        let center = NSWorkspace.shared.notificationCenter

        // App activated (Cmd-Tab, click)
        center.addObserver(
            forName: NSWorkspace.didActivateApplicationNotification,
            object: nil, queue: .main
        ) { [weak self] _ in self?.scheduleEvent() }

        // App launched
        center.addObserver(
            forName: NSWorkspace.didLaunchApplicationNotification,
            object: nil, queue: .main
        ) { [weak self] notification in
            if let app = notification.userInfo?[NSWorkspace.applicationUserInfoKey]
                as? NSRunningApplication {
                self?.addAXObserver(for: app.processIdentifier)
            }
            self?.scheduleEvent()
        }

        // App terminated
        center.addObserver(
            forName: NSWorkspace.didTerminateApplicationNotification,
            object: nil, queue: .main
        ) { [weak self] notification in
            if let app = notification.userInfo?[NSWorkspace.applicationUserInfoKey]
                as? NSRunningApplication {
                self?.removeAXObserver(for: app.processIdentifier)
            }
            self?.scheduleEvent()
        }

        // Register AXObservers for currently running apps
        for app in NSWorkspace.shared.runningApplications {
            if app.activationPolicy == .regular {
                addAXObserver(for: app.processIdentifier)
            }
        }
    }

    func stop() {
        NSWorkspace.shared.notificationCenter.removeObserver(self)
        coalescedWork?.cancel()
        for (_, observer) in axObservers {
            CFRunLoopRemoveSource(
                CFRunLoopGetMain(),
                AXObserverGetRunLoopSource(observer),
                .defaultMode
            )
        }
        axObservers.removeAll()
    }

    /// Coalesce events within 16ms (one frame at 60fps) to prevent floods
    /// during rapid window drags.
    private func scheduleEvent() {
        coalescedWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            self?.onEvent?()
        }
        coalescedWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(16), execute: work)
    }

    // MARK: - AXObserver for window geometry changes

    private func addAXObserver(for pid: pid_t) {
        guard axObservers[pid] == nil else { return }

        var observer: AXObserver?
        let callback: AXObserverCallback = { _, _, _, refcon in
            guard let refcon else { return }
            let monitor = Unmanaged<EventMonitor>.fromOpaque(refcon).takeUnretainedValue()
            monitor.scheduleEvent()
        }

        guard AXObserverCreate(pid, callback, &observer) == .success,
              let observer
        else { return }

        let refcon = Unmanaged.passUnretained(self).toOpaque()
        let appElement = AXUIElementCreateApplication(pid)

        let notifications: [CFString] = [
            kAXWindowMovedNotification as CFString,
            kAXWindowResizedNotification as CFString,
            kAXCreatedNotification as CFString,
            kAXUIElementDestroyedNotification as CFString,
            kAXWindowMiniaturizedNotification as CFString,
            kAXWindowDeminiaturizedNotification as CFString,
        ]

        for note in notifications {
            AXObserverAddNotification(observer, appElement, note, refcon)
        }

        CFRunLoopAddSource(
            CFRunLoopGetMain(),
            AXObserverGetRunLoopSource(observer),
            .defaultMode
        )

        axObservers[pid] = observer
    }

    private func removeAXObserver(for pid: pid_t) {
        guard let observer = axObservers.removeValue(forKey: pid) else { return }
        CFRunLoopRemoveSource(
            CFRunLoopGetMain(),
            AXObserverGetRunLoopSource(observer),
            .defaultMode
        )
    }
}
