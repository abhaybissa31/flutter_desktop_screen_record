#ifndef WINDOWS_RUNNER_SCREEN_RECORDER_PLUGIN_H_
#define WINDOWS_RUNNER_SCREEN_RECORDER_PLUGIN_H_

#include <windows.h>
#include <flutter/plugin_registrar.h>

// Register the screen recorder plugin.
// |flutter_hwnd| is the top-level Win32 window handle used for DPI-aware
// logical→physical coordinate conversion when the region is received from Dart.
void ScreenRecorderPluginRegisterWithRegistrar(
    flutter::PluginRegistrar* registrar,
    HWND flutter_hwnd);

#endif  // WINDOWS_RUNNER_SCREEN_RECORDER_PLUGIN_H_
