# desktop_record — Context for Debugging

## What this is
Flutter desktop screen recorder (Linux + Windows). Native plugin compiled directly into the runner (not a Flutter plugin package). Records a user-selected region with mic audio.

## Architecture
```
Flutter (Dart) ──MethodChannel("com.screenrecorder/recorder")──► Native plugin
```
- Linux : C plugin  → X11/XShm + PulseAudio + GStreamer → .webm (VP8+Opus)
- Windows: C++ plugin → GDI BitBlt (fallback from DXGI) + WASAPI + Media Foundation → .mp4 (H.264+AAC)

## MethodChannel methods (both platforms)
| Method                    | Args                              | Returns             |
|---------------------------|-----------------------------------|---------------------|
| checkPermissions          | —                                 | bool                |
| selectRegion              | — (Linux only; null on Windows)   | Map{x,y,w,h}       |
| startRecording            | {x,y,width,height,outputPath}     | void                |
| stopRecording             | —                                 | String (path)       |
| captureDesktopScreenshot  | — (Windows only)                  | Map{bytes,w,h}      |
| setFullscreen             | bool (Windows only)               | void                |

## Key files — ALL platforms
- `lib/main.dart` — Flutter UI (region selector, start/stop buttons)
- `lib/screen_recorder_plugin.dart` — Dart MethodChannel wrapper + defaultOutputPath()
- `lib/region_selector.dart` — Flutter overlay region selector (used on Windows in fullscreen mode)

## Key files — Linux
- `linux/runner/screen_recorder_plugin.c` — ALL native code
- `linux/runner/screen_recorder_plugin.h` — extern "C" header
- `linux/runner/my_application.cc` — registers the plugin
- `linux/runner/CMakeLists.txt` — builds .c as `screen_recorder_native` static lib

## Key files — Windows
- `windows/runner/screen_recorder_plugin.cpp` — ALL native code (capture+WASAPI+MF)
- `windows/runner/screen_recorder_plugin.h` — declares ScreenRecorderPluginRegisterWithRegistrar
- `windows/runner/flutter_window.cpp` — registers the plugin in OnCreate()
- `windows/runner/CMakeLists.txt` — builds .cpp as `screen_recorder_native` static lib

---

## Current Windows Status

### DXGI_ERROR_UNSUPPORTED (0x887A0004) — RESOLVED via GDI fallback

**Problem**: `IDXGIOutput1::DuplicateOutput()` returns `DXGI_ERROR_UNSUPPORTED` (0x887A0004) on this system (Lenovo laptop, Windows 11 Home). This persisted even after:
1. Fixing adapter enumeration (creating D3D device on the correct adapter)
2. Trying every adapter+output combination
3. Trying the default hardware device as last resort

All approaches returned the same error. Root cause is likely GPU driver / Windows compositor limitation that prevents DXGI Desktop Duplication on this specific hardware.

**Fix**: Added GDI BitBlt fallback capture (`init_gdi_capture`). The `init_capture()` function tries DXGI first, and if all attempts fail, falls back to GDI screen capture which is universally compatible. The video thread branches on `use_gdi` flag.

### Region selection — OS-level fullscreen overlay

**Problem**: The Flutter region selector overlay was confined to the app window, not the full desktop.

**Fix**: On Windows, before showing the region selector:
1. `captureDesktopScreenshot` — GDI BitBlt of the entire virtual desktop
2. `setFullscreen(true)` — makes the Flutter window borderless, topmost, covering all monitors
3. The region selector route displays the desktop screenshot as background, giving the user a view of the real desktop to select from
4. Recording starts while still fullscreen (so `ClientToScreen` maps coordinates correctly)
5. `setFullscreen(false)` restores the window

Coordinates from the selector are logical pixels relative to the fullscreen window. The existing `ClientToScreen + logical * DPI_scale` conversion in `StartRecording` gives correct absolute physical screen coordinates.
