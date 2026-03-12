import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

/// Immutable rectangle representing a screen region in logical pixels.
class RegionRect {
  final int x, y, width, height;
  const RegionRect({
    required this.x,
    required this.y,
    required this.width,
    required this.height,
  });

  @override
  String toString() => 'RegionRect($x, $y, ${width}x$height)';
}

/// Push this route to let the user drag-select a screen region.
/// Resolves with [RegionRect] on selection, or null if cancelled (Escape).
///
/// Usage:
/// ```dart
/// final region = await Navigator.of(context).push<RegionRect>(
///   RegionSelectorRoute(),
/// );
/// ```
class RegionSelectorRoute extends PageRoute<RegionRect> {
  RegionSelectorRoute() : super(fullscreenDialog: true);

  @override
  Color? get barrierColor => Colors.black.withOpacity(0.45);

  @override
  String? get barrierLabel => 'Cancel';

  @override
  bool get barrierDismissible => false;

  @override
  bool get opaque => false;

  @override
  bool get maintainState => false;

  @override
  Duration get transitionDuration => Duration.zero;

  @override
  Widget buildPage(BuildContext context, Animation<double> animation,
      Animation<double> secondaryAnimation) {
    return _RegionSelectorPage(
      onSelected: (rect) => Navigator.of(context).pop(rect),
      onCancel: () => Navigator.of(context).pop(null),
    );
  }
}

class _RegionSelectorPage extends StatefulWidget {
  final ValueChanged<RegionRect> onSelected;
  final VoidCallback onCancel;

  const _RegionSelectorPage(
      {required this.onSelected, required this.onCancel});

  @override
  State<_RegionSelectorPage> createState() => _RegionSelectorPageState();
}

class _RegionSelectorPageState extends State<_RegionSelectorPage> {
  Offset? _start;
  Offset? _current;
  bool _isDragging = false;

  static const _minSize = 20.0;

  Rect? get _selection {
    if (_start == null || _current == null) return null;
    return Rect.fromPoints(_start!, _current!);
  }

  @override
  Widget build(BuildContext context) {
    return KeyboardListener(
      focusNode: FocusNode()..requestFocus(),
      autofocus: true,
      onKeyEvent: (e) {
        if (e is KeyDownEvent &&
            e.logicalKey == LogicalKeyboardKey.escape) {
          widget.onCancel();
        }
      },
      child: MouseRegion(
        cursor: SystemMouseCursors.precise,
        child: GestureDetector(
          onPanStart: (d) => setState(() {
            _start = d.globalPosition;
            _current = d.globalPosition;
            _isDragging = true;
          }),
          onPanUpdate: (d) => setState(() {
            _current = d.globalPosition;
          }),
          onPanEnd: (_) {
            final sel = _selection;
            if (sel != null &&
                sel.width > _minSize &&
                sel.height > _minSize) {
              widget.onSelected(RegionRect(
                x: sel.left.round(),
                y: sel.top.round(),
                width: sel.width.round(),
                height: sel.height.round(),
              ));
            } else {
              setState(() {
                _start = null;
                _current = null;
                _isDragging = false;
              });
            }
          },
          child: CustomPaint(
            size: Size.infinite,
            painter: _SelectionPainter(
              selection: _selection,
              isDragging: _isDragging,
            ),
            child: Stack(
              children: [
                // Top hint
                if (!_isDragging)
                  Positioned(
                    top: 32,
                    left: 0,
                    right: 0,
                    child: Center(
                      child: Container(
                        padding: const EdgeInsets.symmetric(
                            horizontal: 20, vertical: 10),
                        decoration: BoxDecoration(
                          color: Colors.black.withOpacity(0.7),
                          borderRadius: BorderRadius.circular(8),
                        ),
                        child: const Text(
                          'Drag to select recording area  •  Esc to cancel',
                          style: TextStyle(
                              color: Colors.white,
                              fontSize: 14,
                              decoration: TextDecoration.none),
                        ),
                      ),
                    ),
                  ),
                // Size indicator
                if (_isDragging && _selection != null)
                  Positioned(
                    left: _selection!.left,
                    top: _selection!.top - 26,
                    child: Container(
                      padding: const EdgeInsets.symmetric(
                          horizontal: 8, vertical: 4),
                      decoration: BoxDecoration(
                        color: const Color(0xFF2563EB),
                        borderRadius: BorderRadius.circular(4),
                      ),
                      child: Text(
                        '${_selection!.width.round()} × ${_selection!.height.round()}',
                        style: const TextStyle(
                            color: Colors.white,
                            fontSize: 12,
                            fontWeight: FontWeight.w600,
                            decoration: TextDecoration.none),
                      ),
                    ),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _SelectionPainter extends CustomPainter {
  final Rect? selection;
  final bool isDragging;

  _SelectionPainter({this.selection, required this.isDragging});

  @override
  void paint(Canvas canvas, Size size) {
    final darkenPaint = Paint()..color = Colors.black.withOpacity(0.5);

    if (selection == null || selection!.isEmpty) {
      // Full dark overlay when not selecting
      canvas.drawRect(Offset.zero & size, darkenPaint);
      return;
    }

    final sel = selection!;

    // Draw dark overlay around selection using 4 rects (hole punch effect)
    // Top
    canvas.drawRect(
        Rect.fromLTWH(0, 0, size.width, sel.top), darkenPaint);
    // Bottom
    canvas.drawRect(
        Rect.fromLTWH(0, sel.bottom, size.width, size.height - sel.bottom),
        darkenPaint);
    // Left
    canvas.drawRect(
        Rect.fromLTWH(0, sel.top, sel.left, sel.height), darkenPaint);
    // Right
    canvas.drawRect(
        Rect.fromLTWH(sel.right, sel.top, size.width - sel.right, sel.height),
        darkenPaint);

    // Selection border
    final borderPaint = Paint()
      ..color = const Color(0xFF2563EB)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2.0;
    canvas.drawRect(sel, borderPaint);

    // Corner handles
    _drawCornerHandles(canvas, sel);
  }

  void _drawCornerHandles(Canvas canvas, Rect sel) {
    const handleLen = 12.0;
    const handleWidth = 3.0;
    final paint = Paint()
      ..color = const Color(0xFF2563EB)
      ..strokeWidth = handleWidth
      ..strokeCap = StrokeCap.round;

    final corners = [
      sel.topLeft,
      sel.topRight,
      sel.bottomLeft,
      sel.bottomRight,
    ];

    for (final corner in corners) {
      final dx = corner == sel.topLeft || corner == sel.bottomLeft
          ? handleLen
          : -handleLen;
      final dy = corner == sel.topLeft || corner == sel.topRight
          ? handleLen
          : -handleLen;
      canvas.drawLine(corner, corner + Offset(dx, 0), paint);
      canvas.drawLine(corner, corner + Offset(0, dy), paint);
    }
  }

  @override
  bool shouldRepaint(_SelectionPainter old) =>
      old.selection != selection || old.isDragging != isDragging;
}
