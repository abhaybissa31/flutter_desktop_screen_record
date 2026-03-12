#ifndef SCREEN_RECORDER_PLUGIN_H_
#define SCREEN_RECORDER_PLUGIN_H_

#include <flutter_linux/flutter_linux.h>

G_DECLARE_FINAL_TYPE(ScreenRecorderPlugin, screen_recorder_plugin,
                     SCREEN_RECORDER, PLUGIN, GObject)

#ifdef __cplusplus
extern "C" {
#endif

void screen_recorder_plugin_register_with_registrar(
    FlPluginRegistrar* registrar);

#ifdef __cplusplus
}
#endif

#endif  // SCREEN_RECORDER_PLUGIN_H_
