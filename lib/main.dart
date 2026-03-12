import 'dart:io';
import 'package:flutter/material.dart';
import 'screen_recorder_plugin.dart';

void main() {
  runApp(const RecorderApp());
}

class RecorderApp extends StatelessWidget {
  const RecorderApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Screen Recorder',
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(useMaterial3: true).copyWith(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF2563EB),
          brightness: Brightness.dark,
        ),
      ),
      home: const RecorderHome(),
    );
  }
}

enum RecordingState { idle, selecting, recording }

class RecorderHome extends StatefulWidget {
  const RecorderHome({super.key});

  @override
  State<RecorderHome> createState() => _RecorderHomeState();
}

class _RecorderHomeState extends State<RecorderHome> {
  RecordingState _state = RecordingState.idle;
  RegionRect?    _region;
  String?        _lastFile;
  String?        _error;
  Duration       _elapsed = Duration.zero;
  DateTime?      _startedAt;
  late final Stream<Duration> _ticker;

  @override
  void initState() {
    super.initState();
    // Tick every second to update elapsed display
    _ticker = Stream.periodic(const Duration(seconds: 1), (_) {
      if (_startedAt == null) return Duration.zero;
      return DateTime.now().difference(_startedAt!);
    }).asBroadcastStream();
    _checkPermissions();
  }

  Future<void> _checkPermissions() async {
    final missing = await ScreenRecorderPlugin.checkDependencies();
    if (missing != null && mounted) {
      setState(() => _error = missing);
      return;
    }
    final ok = await ScreenRecorderPlugin.checkPermissions();
    if (!ok && mounted) {
      await ScreenRecorderPlugin.requestPermissions();
    }
  }

  Future<void> _selectRegion() async {
    setState(() {
      _state  = RecordingState.selecting;
      _error  = null;
      _region = null;
    });

    RegionRect? region;
    if (Platform.isLinux) {
      // Native fullscreen X11 overlay selector
      region = await ScreenRecorderPlugin.selectRegion();
    } else {
      // Flutter overlay fallback for non-Linux platforms
      region = await Navigator.of(context).push<RegionRect>(
        RegionSelectorRoute(),
      );
    }

    if (!mounted) return;

    if (region == null) {
      setState(() => _state = RecordingState.idle);
      return;
    }

    setState(() => _region = region);
    await _startRecording(region);
  }

  Future<void> _startRecording(RegionRect region) async {
    final outputPath = ScreenRecorderPlugin.defaultOutputPath();
    try {
      await ScreenRecorderPlugin.startRecording(
        rect: region,
        outputPath: outputPath,
      );
      setState(() {
        _state     = RecordingState.recording;
        _startedAt = DateTime.now();
        _elapsed   = Duration.zero;
      });
    } catch (e) {
      setState(() {
        _state = RecordingState.idle;
        _error = e.toString();
      });
    }
  }

  Future<void> _stopRecording() async {
    try {
      final path = await ScreenRecorderPlugin.stopRecording();
      setState(() {
        _state    = RecordingState.idle;
        _lastFile = path;
        _startedAt = null;
      });
    } catch (e) {
      setState(() {
        _state = RecordingState.idle;
        _error = e.toString();
        _startedAt = null;
      });
    }
  }

  String _formatDuration(Duration d) {
    final h = d.inHours;
    final m = d.inMinutes.remainder(60);
    final s = d.inSeconds.remainder(60);
    if (h > 0) return '$h:${m.toString().padLeft(2, '0')}:${s.toString().padLeft(2, '0')}';
    return '${m.toString().padLeft(2, '0')}:${s.toString().padLeft(2, '0')}';
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF0F172A),
      body: Center(
        child: SizedBox(
          width: 420,
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              // Logo / title
              const Icon(Icons.videocam_rounded,
                  size: 56, color: Color(0xFF2563EB)),
              const SizedBox(height: 16),
              const Text(
                'Screen Recorder',
                style: TextStyle(
                    fontSize: 28,
                    fontWeight: FontWeight.w700,
                    color: Colors.white),
              ),
              const SizedBox(height: 8),
              Text(
                'Select a screen region · Record with mic audio',
                style: TextStyle(
                    fontSize: 14, color: Colors.white.withOpacity(0.5)),
              ),
              const SizedBox(height: 48),

              // Main card
              _buildCard(),

              const SizedBox(height: 24),

              // Error message
              if (_error != null)
                Container(
                  padding: const EdgeInsets.all(12),
                  decoration: BoxDecoration(
                    color: Colors.red.withOpacity(0.15),
                    borderRadius: BorderRadius.circular(8),
                    border: Border.all(
                        color: Colors.red.withOpacity(0.3), width: 1),
                  ),
                  child: Row(
                    children: [
                      const Icon(Icons.error_outline,
                          color: Colors.redAccent, size: 18),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Text(_error!,
                            style: const TextStyle(
                                color: Colors.redAccent, fontSize: 13)),
                      ),
                    ],
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildCard() {
    return Container(
      padding: const EdgeInsets.all(28),
      decoration: BoxDecoration(
        color: const Color(0xFF1E293B),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(
            color: Colors.white.withOpacity(0.08), width: 1),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (_state == RecordingState.recording) ...[
            _buildRecordingIndicator(),
            const SizedBox(height: 24),
            _buildRegionInfo(),
            const SizedBox(height: 24),
            ElevatedButton.icon(
              onPressed: _stopRecording,
              icon: const Icon(Icons.stop_rounded),
              label: const Text('Stop Recording'),
              style: ElevatedButton.styleFrom(
                backgroundColor: Colors.red,
                foregroundColor: Colors.white,
                padding: const EdgeInsets.symmetric(vertical: 16),
                textStyle: const TextStyle(
                    fontSize: 16, fontWeight: FontWeight.w600),
                shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(10)),
              ),
            ),
          ] else ...[
            if (_lastFile != null) ...[
              _buildSavedFile(),
              const SizedBox(height: 20),
              const Divider(color: Color(0xFF334155)),
              const SizedBox(height: 20),
            ],
            ElevatedButton.icon(
              onPressed:
                  _state == RecordingState.idle ? _selectRegion : null,
              icon: const Icon(Icons.crop_rounded),
              label: const Text('Select Region & Record'),
              style: ElevatedButton.styleFrom(
                backgroundColor: const Color(0xFF2563EB),
                foregroundColor: Colors.white,
                disabledBackgroundColor:
                    const Color(0xFF2563EB).withOpacity(0.4),
                padding: const EdgeInsets.symmetric(vertical: 16),
                textStyle: const TextStyle(
                    fontSize: 16, fontWeight: FontWeight.w600),
                shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(10)),
              ),
            ),
            const SizedBox(height: 12),
            Text(
              _platformHint(),
              textAlign: TextAlign.center,
              style: TextStyle(
                  fontSize: 12,
                  color: Colors.white.withOpacity(0.35)),
            ),
          ]
        ],
      ),
    );
  }

  Widget _buildRecordingIndicator() {
    return StreamBuilder<Duration>(
      stream: _ticker,
      builder: (context, snap) {
        final dur = snap.data ?? Duration.zero;
        return Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Container(
              width: 10,
              height: 10,
              decoration: const BoxDecoration(
                  color: Colors.redAccent, shape: BoxShape.circle),
            ),
            const SizedBox(width: 8),
            Text(
              'REC  ${_formatDuration(dur)}',
              style: const TextStyle(
                  color: Colors.redAccent,
                  fontSize: 20,
                  fontWeight: FontWeight.w700,
                  letterSpacing: 1.5),
            ),
          ],
        );
      },
    );
  }

  Widget _buildRegionInfo() {
    if (_region == null) return const SizedBox.shrink();
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
      decoration: BoxDecoration(
        color: Colors.white.withOpacity(0.05),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Icon(Icons.crop_free_rounded,
              size: 16, color: Colors.white.withOpacity(0.5)),
          const SizedBox(width: 6),
          Text(
            '${_region!.width} × ${_region!.height}  at  (${_region!.x}, ${_region!.y})',
            style: TextStyle(
                fontSize: 13, color: Colors.white.withOpacity(0.6)),
          ),
        ],
      ),
    );
  }

  Widget _buildSavedFile() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            const Icon(Icons.check_circle_rounded,
                color: Color(0xFF22C55E), size: 18),
            const SizedBox(width: 6),
            const Text('Recording saved',
                style: TextStyle(
                    color: Color(0xFF22C55E),
                    fontWeight: FontWeight.w600)),
          ],
        ),
        const SizedBox(height: 8),
        GestureDetector(
          onTap: () {
            if (_lastFile != null) {
              final dir = File(_lastFile!).parent.path;
              // Open parent directory — works on all platforms
              if (Platform.isMacOS) {
                Process.run('open', [dir]);
              } else if (Platform.isLinux) {
                Process.run('xdg-open', [dir]);
              } else if (Platform.isWindows) {
                Process.run('explorer', [dir]);
              }
            }
          },
          child: Container(
            padding:
                const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
            decoration: BoxDecoration(
              color: Colors.white.withOpacity(0.05),
              borderRadius: BorderRadius.circular(6),
            ),
            child: Row(
              children: [
                Icon(Icons.folder_open_rounded,
                    size: 14, color: Colors.white.withOpacity(0.4)),
                const SizedBox(width: 6),
                Expanded(
                  child: Text(
                    _lastFile ?? '',
                    style: TextStyle(
                        fontSize: 12,
                        color: Colors.white.withOpacity(0.5),
                        overflow: TextOverflow.ellipsis),
                  ),
                ),
              ],
            ),
          ),
        ),
      ],
    );
  }

  String _platformHint() {
    if (Platform.isMacOS) {
      return 'Saves to ~/Movies/  •  macOS will ask for screen recording permission on first run';
    } else if (Platform.isLinux) {
      return 'Saves to ~/  •  Output format: WebM (VP8 + Opus)';
    } else {
      return 'Saves to Videos folder  •  Output format: MP4 (H.264 + AAC)';
    }
  }
}