import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;
import 'package:flutter/services.dart';
import 'region_selector.dart';

export 'region_selector.dart';

/// Raw desktop screenshot returned by [ScreenRecorderPlugin.captureDesktopScreenshot].
class DesktopSnapshot {
  final Uint8List bytes; // BGRA pixels
  final int width;
  final int height;

  const DesktopSnapshot({
    required this.bytes,
    required this.width,
    required this.height,
  });

  /// Decode the raw BGRA bytes into a [ui.Image] for painting.
  Future<ui.Image> decode() {
    final completer = Completer<ui.Image>();
    ui.decodeImageFromPixels(
      bytes, width, height, ui.PixelFormat.bgra8888,
      completer.complete,
    );
    return completer.future;
  }
}

class ScreenRecorderPlugin {
  static const MethodChannel _channel =
      MethodChannel('com.screenrecorder/recorder');

  /// Check if screen capture permission is granted and all dependencies are available.
  /// Returns null if OK, or a string describing missing dependencies.
static Future<String?> checkDependencies() async {
  try {
    await _channel.invokeMethod<bool>('checkPermissions');
    return null;
  } on PlatformException catch (e) {
    if (e.code == 'MISSING_DEPS' || e.code == 'UNSUPPORTED_OS') {
      return e.message;
    }
    return e.message;
  } catch (_) {
    return null;
  }
}

  /// Check if screen capture permission is granted.
  static Future<bool> checkPermissions() async {
    try {
      final result = await _channel.invokeMethod<bool>('checkPermissions');
      return result ?? false;
    } on PlatformException catch (e) {
      if (e.code == 'MISSING_DEPS') return false;
      return false;
    } catch (_) {
      return false;
    }
  }

  /// Request screen capture permission (macOS / Windows prompt handled natively).
  /// No-op on Linux.
  static Future<void> requestPermissions() async {
    if (Platform.isLinux) return;
    try {
      await _channel.invokeMethod('requestPermissions');
    } on MissingPluginException {
      // Plugin not yet registered — safe to ignore
    }
  }

  /// Launch the native OS-level region selector (fullscreen X11 overlay on Linux).
  /// Returns a [RegionRect] if the user completed a selection, or null if cancelled.
  static Future<RegionRect?> selectRegion() async {
    if (Platform.isLinux) {
      final result = await _channel.invokeMethod<Map>('selectRegion');
      if (result == null) return null;
      return RegionRect(
        x: (result['x'] as int?) ?? 0,
        y: (result['y'] as int?) ?? 0,
        width: (result['width'] as int?) ?? 0,
        height: (result['height'] as int?) ?? 0,
      );
    }
    // Non-Linux: caller should use Flutter overlay route as fallback
    return null;
  }

  /// Start recording the given region [rect] with mic audio.
  /// [outputPath] must be a full absolute path with extension (.mp4 on macOS/Windows, .webm on Linux).
  static Future<void> startRecording({
    required RegionRect rect,
    required String outputPath,
  }) async {
    // Ensure even dimensions (H.264 requirement)
    final w = rect.width % 2 == 0 ? rect.width : rect.width - 1;
    final h = rect.height % 2 == 0 ? rect.height : rect.height - 1;

    await _channel.invokeMethod('startRecording', {
      'x': rect.x,
      'y': rect.y,
      'width': w,
      'height': h,
      'outputPath': outputPath,
    });
  }

  /// Stop recording. Returns the path of the output file.
  static Future<String?> stopRecording() async {
    final result = await _channel.invokeMethod<String>('stopRecording');
    return result;
  }

  /// Capture a screenshot of the entire virtual desktop (Windows only).
  /// Returns a [DesktopSnapshot] with raw BGRA pixels, or null on failure.
  static Future<DesktopSnapshot?> captureDesktopScreenshot() async {
    if (!Platform.isWindows) return null;
    final result = await _channel.invokeMethod<Map>('captureDesktopScreenshot');
    if (result == null) return null;
    return DesktopSnapshot(
      bytes: result['bytes'] as Uint8List,
      width: result['width'] as int,
      height: result['height'] as int,
    );
  }

  /// Toggle the Flutter window between fullscreen-borderless (covering all
  /// monitors) and its previous normal state.  Used for OS-level region
  /// selection on Windows.
  static Future<void> setFullscreen(bool fullscreen) async {
    await _channel.invokeMethod('setFullscreen', fullscreen);
  }

  /// Returns a suitable default output path for the current platform.
  static String defaultOutputPath() {
    final timestamp = DateTime.now().millisecondsSinceEpoch;
    if (Platform.isLinux) {
      final home = Platform.environment['HOME'] ?? '/tmp';
      return '$home/recording_$timestamp.webm';
    } else if (Platform.isWindows) {
      final userProfile =
          Platform.environment['USERPROFILE'] ?? 'C:\\Users\\Public';
      return '$userProfile\\Videos\\recording_$timestamp.mp4';
    } else {
      // macOS
      final home = Platform.environment['HOME'] ?? '/tmp';
      return '$home/Movies/recording_$timestamp.mp4';
    }
  }
}
