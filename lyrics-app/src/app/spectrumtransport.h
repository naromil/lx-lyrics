/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QByteArray>
#include <QObject>

class WsClient;

// Where the spectrum frames come from. SpectrumBridge owns the single
// (playing && audioVisualization) gate and the request loop; a transport only
// carries one frame in each direction, so the host-driven app and the
// standalone `--demo` self-feed run through exactly the same gate and loop
// (an app that is fed differently in demo mode would not exercise the real
// path — which is how the demo used to show an empty visualizer).
class SpectrumTransport : public QObject {
  Q_OBJECT

public:
  using QObject::QObject;

  // Ask the source for one snapshot. It may answer with nothing at all (a host
  // with no audio to analyse offers no window), and it may answer more than
  // once — the frame signal is the only contract.
  virtual void requestFrame() = 0;

  // The current playback state, so a bridge constructed after the first state
  // message still starts from the truth. Host transports track the messages
  // they forward; the demo always plays.
  virtual bool isPlaying() const { return false; }

signals:
  // Exactly 128 bytes of log-scaled spectrum magnitudes (docs/protocol.md §5).
  void frameReceived(const QByteArray& frame);
  // Playback started/stopped: set_info/set_status carry isPlay, set_play
  // implies true, set_pause/set_stop imply false (the same rules PauseHide
  // applies to the lyric window).
  void playStateChanged(bool playing);
};

// Host-driven transport: forwards the WsClient spectrum messages and the
// analyser request, and tracks the play boolean carried by the state messages.
class WsSpectrumTransport : public SpectrumTransport {
  Q_OBJECT

public:
  explicit WsSpectrumTransport(WsClient* ws, QObject* parent = nullptr);

  void requestFrame() override;
  bool isPlaying() const override { return m_playing; }

private:
  WsClient* m_ws;
  bool m_playing = false;
};

// Standalone `--demo` transport: answers every request with a synthetic
// music-like frame (a bass-heavy spectral tilt shaped by two slow LFOs and a
// beat pulse — no RNG, so a surprising screenshot is reproducible) and reports
// playing, since the demo plays. The frame buffer is allocated once.
class DemoSpectrumTransport : public SpectrumTransport {
  Q_OBJECT

public:
  explicit DemoSpectrumTransport(QObject* parent = nullptr);

  void requestFrame() override;
  bool isPlaying() const override { return true; }

private:
  QByteArray m_frame;
  int m_tick = 0;
};
