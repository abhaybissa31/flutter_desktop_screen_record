import Cocoa
import FlutterMacOS

@main
class AppDelegate: FlutterAppDelegate {
  override func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    return true
  }

  override func applicationSupportsSecureRestorableState(_ app: NSApplication) -> Bool {
    return true
  }

     override func applicationDidFinishLaunching(_ notification: Notification) {
        // Get the Flutter view controller and register the plugin
        if let controller = mainFlutterWindow?.contentViewController
            as? FlutterViewController {
            ScreenRecorderPlugin.register(
                with: controller.registrar(forPlugin: "ScreenRecorderPlugin"))
        }
        super.applicationDidFinishLaunching(notification)
    }
}
