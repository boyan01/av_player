import 'dart:isolate';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'dart:developer';

import 'package:system_clock/system_clock.dart';

class AudioPlayer {
  final String _uri;

  final ValueNotifier<PlayerStatus> _state = ValueNotifier(PlayerStatus.Idle);

  final String _playerId = _AudioPlayerIdGenerater.generatePlayerId();

  int _currentTime = 0;
  int _currentUpdateUptime = -1;

  Duration _duration = Duration.zero;

  AudioPlayer._create(this._uri) : assert(_uri != null) {
    _createRemotePlayer();
  }

  factory AudioPlayer.file(String path) {
    return AudioPlayer._create(path);
  }

  factory AudioPlayer.url(String url) {
    return AudioPlayer._create(url);
  }

  void _createRemotePlayer() {
    _remotePlayerManager.create(this, _playerId, _uri);
  }

  void seek(Duration duration) {
    _remotePlayerManager.seek(_playerId, duration);
  }

  ValueNotifier<bool> _playWhenReady = ValueNotifier(false);

  set playWhenReady(bool value) {
    if (_playWhenReady.value == value) {
      return;
    }
    _playWhenReady.value = value;
    if (value) {
      _remotePlayerManager.play(_playerId);
      if (status == PlayerStatus.Ready) {}
    } else {
      _remotePlayerManager.pause(_playerId);
    }
  }

  bool get playWhenReady => _playWhenReady.value;

  Duration get currentTime {
    if (_currentUpdateUptime == -1 || _currentTime < 0) {
      return Duration.zero;
    }
    if (!isPlaying) {
      return Duration(milliseconds: _currentTime);
    }
    final int offset = SystemClock.uptime().inMilliseconds - _currentUpdateUptime;
    return Duration(milliseconds: _currentTime + offset.atleast(0));
  }

  Duration get duration {
    return _duration;
  }

  bool get isPlaying {
    return playWhenReady && status == PlayerStatus.Ready;
  }

  PlayerStatus get status => _state.value;

  Listenable _onStateChange;

  Listenable get onStateChanged {
    if (_onStateChange == null) {
      _onStateChange = Listenable.merge([_state, _playWhenReady]);
    }
    return _onStateChange;
  }

  void release() {
    _remotePlayerManager.dispose(this);
  }

  @override
  bool operator ==(Object other) {
    if (other is AudioPlayer) {
      return other._playerId == _playerId;
    }
    return false;
  }

  @override
  int get hashCode => _playerId.hashCode;

  @override
  String toString() {
    return "AudioPlayer(id = $_playerId, state = $status, playing = $isPlaying, playWhenReady = $playWhenReady, "
        "duration = $duration, time = (position: $_currentTime, update: $_currentUpdateUptime, computed: $currentTime))";
  }
}

final _RemotePlayerManager _remotePlayerManager = _RemotePlayerManager()..init();

class _RemotePlayerManager {
  final MethodChannel _channel = MethodChannel("tech.soit.flutter.audio_player");

  final Map<String, AudioPlayer> players = {};

  _RemotePlayerManager() {
    _channel.setMethodCallHandler((call) async {
      debugPrint("on method call: ${call.method} args = ${call.arguments}");
      try {
        return await _handleMessage(call);
      } catch (e, s) {
        debugPrint(e);
        debugPrintStack(stackTrace: s);
        rethrow;
      }
    });
  }

  Future<void> init() async {
    await _channel.invokeMethod("initialize");
  }

  Future<dynamic> _handleMessage(MethodCall call) async {
    final String playerId = call.argument("playerId");
    if (call.method == "onPlaybackEvent") {
      final AudioPlayer player = players[playerId];
      assert(player != null);
      final int eventId = call.argument("event");
      assert(eventId != null && eventId > 0 && eventId < _ClientPlayerEvent.values.length);
      final _ClientPlayerEvent event = _ClientPlayerEvent.values[eventId];
      _updatePlayerState(player, event, call);
    }
  }

  void _updatePlayerState(AudioPlayer player, _ClientPlayerEvent event, MethodCall call) {
    const playbackEvents = <_ClientPlayerEvent>{
      _ClientPlayerEvent.Playing,
      _ClientPlayerEvent.Paused,
      _ClientPlayerEvent.Buffering
    };
    if (playbackEvents.contains(event)) {
      final int position = call.argument("position");
      final int updateTime = call.argument("updateTime");
      assert(position != null);
      assert(updateTime != null);
      player._currentTime = position;
      player._currentUpdateUptime = updateTime;
    }

    switch (event) {
      case _ClientPlayerEvent.Preparing:
        player._state.value = PlayerStatus.Buffering;
        break;
      case _ClientPlayerEvent.Prepared:
        final int duration = call.argument("duration");
        if (duration > 0) {
          player._duration = Duration(milliseconds: duration);
        }
        player._state.value = PlayerStatus.Ready;
        break;
      case _ClientPlayerEvent.Buffering:
        player._state.value = PlayerStatus.Buffering;
        break;
      case _ClientPlayerEvent.Error:
        player._state.value = PlayerStatus.Error;
        break;
      default:
        if (player._state.value == PlayerStatus.Buffering) {
          player._state.value = PlayerStatus.Ready;
        }
        break;
    }
  }

  Future<void> create(AudioPlayer player, String playerId, String url) async {
    players[playerId] = player;
    try {
      await _channel.invokeMethod("create", {
        "playerId": playerId,
        "url": url,
      });
    } on FlutterError catch (e) {
      debugPrint("error: $e");
    }
  }

  Future<void> play(String playerId) async {
    await _channel.invokeMethod("play", {"playerId": playerId});
  }

  Future<void> pause(String playerId) async {
    await _channel.invokeMethod("pause", {"playerId": playerId});
  }

  Future<void> seek(String playerId, Duration duration) async {
    await _channel.invokeMethod("seek", {
      "playerId": playerId,
      "position": duration.inMilliseconds,
    });
  }

  void dispose(AudioPlayer audioPlayer) {
    players.remove(audioPlayer._playerId);
  }
}

enum _ClientPlayerEvent { Paused, Playing, Preparing, Prepared, Buffering, Error }

enum PlayerStatus {
  Idle,
  Buffering,
  Ready,
  Error,
}

extension _MethodCallArg on MethodCall {
  T argument<T>(String key) {
    final dynamic value = (arguments as Map)[key];
    assert(value is T);
    return value as T;
  }
}

extension _AudioPlayerIdGenerater on AudioPlayer {
  static int _id = 0;

  static String generatePlayerId() {
    final String isolateId = Service.getIsolateID(Isolate.current);
    assert(isolateId != null, "isolate id is empty");
    return "${isolateId}_${_id++}";
  }
}

extension _IntClamp on int {
  int atleast(int value) {
    return this < value ? value : this;
  }
}
