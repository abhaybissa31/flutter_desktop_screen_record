# deskton_record — Context for Debugging

## What this is
Flutter desktop Linux screen recorder. Native C plugin (not a Flutter plugin package) compiled into the runner. Records a user-selected screen region with mic audio to WebM using pure native APIs.

## Architecture
```
Flutter (Dart) ──MethodChannel──► C plugin (linux/runner/screen_recorder_plugin.c)
                                    ├── X11/XShm: screen capture
                                    ├── PulseAudio: mic audio
                                    ├── GStreamer: encode + mux → .webm
                                    └── X11 overlay: fullscreen region selector
```

## Key Files
- `linux/runner/screen_recorder_plugin.c` — ALL native code (recording, region selector, Flutter method channel)
- `linux/runner/screen_recorder_plugin.h` — header with extern "C" guards
- `linux/runner/my_application.cc` — registers the plugin
- `linux/runner/CMakeLists.txt` — builds .c as separate static lib for C linkage
- `linux/CMakeLists.txt` — top-level, finds GTK
- `lib/main.dart` — Flutter UI
- `lib/screen_recorder_plugin.dart` — Dart MethodChannel wrapper
- `lib/region_selector.dart` — RegionRect class + Flutter overlay fallback (unused on Linux)

## GStreamer Pipeline (current)
```
appsrc(video,BGRx) → queue → videoconvert → vp8enc → queue → matroskamux → filesink
appsrc(audio,S16LE) → queue → audioconvert → opusenc → queue → mux.
```

Built via `gst_parse_launch`. appsrc properties (caps, format, is-live, do-timestamp)
set programmatically via `g_object_set` after `gst_bin_get_by_name` because
`gst_parse_launch` does NOT correctly parse `format=time` (GstFormat enum).

## THE BUG: 0-byte WebM output

### Status
Recording starts, frames + audio chunks are pushed successfully to GStreamer,
but the output file is always 0 bytes.

### What we've tried and what happened

1. **Original approach**: Manual element creation with `g_object_set` for all properties
   - Bug: `g_object_set(venc, "deadline", 1, ...)` — `deadline` is `gint64` (8 bytes)
     but `1` is `int` (4 bytes). Varargs type mismatch corrupted ALL subsequent
     properties (`cpu-used`, `target-bitrate` read garbage from stack).
   - Result: 0 bytes, "Internal data stream error"

2. **gst_parse_launch with do-timestamp=true**: Fixed varargs issue but forgot `format=time`
   - Result: `gst_segment_to_running_time: assertion 'segment->format == format' failed`
   - `asrc: not-negotiated (-4)` — segment format was BYTES, needed TIME

3. **Added format=time to pipeline string**: `gst_parse_launch` doesn't parse GstFormat enum correctly
   - Result: Same segment assertion + not-negotiated error

4. **Current approach (latest fix, UNTESTED)**:
   - `gst_parse_launch` for pipeline structure + encoder properties (avoids varargs issues)
   - `g_object_set` for appsrc properties (format, caps, etc.) — correct type handling
   - Manual timestamps (PTS/DTS set explicitly, do-timestamp=false)
   - `matroskamux streamable=true` instead of `webmmux`
   - All queues unlimited (`max-size-*=0`)
   - `filesink sync=false`

### Key diagnostic info from logs
- Pipeline builds and sets to PLAYING (ret=2 = ASYNC, normal for live)
- Video: XImage bpl matches expected stride (no stride mismatch)
- Video: 90-264 frames pushed with GST_FLOW_OK before error
- Audio: 257-353 chunks pushed with GST_FLOW_OK
- Error always from `asrc` (audio appsrc): "not-negotiated (-4)"
- `gst_segment_to_running_time` assertion fails → segment format mismatch

### Things that WORK
- Region selector (X11 overlay with Cairo) works perfectly
- X11 screen capture works (frames captured, correct stride)
- PulseAudio mic capture works (chunks read without error)
- Flutter ↔ C method channel works
- Build system works (separate C static lib for linkage)

### What to investigate if still broken
1. Verify `g_object_set(s->audio_src, "format", GST_FORMAT_TIME, NULL)` actually sets the format.
   Add: `GstFormat fmt; g_object_get(s->audio_src, "format", &fmt, NULL); fprintf(stderr, "format=%d\n", fmt);`
2. Try `stream-type=0` vs `stream-type=2` on appsrc
3. Try video-only pipeline (remove audio) to isolate the issue
4. Try running equivalent pipeline with `gst-launch-1.0` using `videotestsrc` + `audiotestsrc`
   to verify GStreamer plugins work:
   ```
   gst-launch-1.0 videotestsrc ! videoconvert ! vp8enc ! matroskamux name=mux ! filesink location=test.webm \
     audiotestsrc ! audioconvert ! opusenc ! mux.
   ```
5. Check installed GStreamer version: `gst-launch-1.0 --version`
6. Check if opus plugin exists: `gst-inspect-1.0 opusenc`
7. Try `vorbisenc` instead of `opusenc` (different audio codec, might negotiate differently)
8. Check if the issue is appsrc stream-type. Try adding `g_object_set(src, "stream-type", 0, NULL)`
   explicitly, or try stream-type=2 (random-access)

### Build notes
- C file compiled as separate static lib (`screen_recorder_native`) to avoid C++ name mangling
- Deps: libx11-dev libxext-dev libpulse-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good
- `flutter build linux` for release, `flutter run -d linux` for debug
- System: Ubuntu, X11, GStreamer 1.x

### Constraints
- No ffmpeg, no external binaries
- X11 only (no Wayland/PipeWire)
- GStreamer + X11 + PulseAudio are the only native deps
