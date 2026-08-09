/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#pragma once

#include <QByteArray>
#include <QObject>
#include <QVector>

#include <memory>

class HostServer;

namespace Fooyin {
class EngineController;
class VisualisationSession;
} // namespace Fooyin

class SpectrumSource : public QObject {
  Q_OBJECT

public:
  explicit SpectrumSource(Fooyin::EngineController* engine, HostServer* host,
                          QObject* parent = nullptr);

  /// Pure helper (no engine/host state): compress float spectrum bins into the
  /// exactly-128-byte protocol frame (protocol.md §5). Bins >= 128: the first
  /// 128 are kept (FFT bin 0 is DC through bin 127 just below Nyquist; the
  /// final Nyquist bin is dropped). Bins < 128: zero-padded. Each kept value
  /// is log-scaled per protocol.md §5 and clamped to 0-255. Static so the
  /// conversion can be unit-tested without a live Fooyin engine.
  static QByteArray scaleToBytes(const QVector<float>& bins);


  /// Pull one fresh spectrum frame and push it to the connected app.
  void onAnalyserDataRequested();

private:
  /// Lazy-create the shared pull session on first use; null when unavailable
  /// (no engine or no visualisation service).
  [[nodiscard]] std::shared_ptr<Fooyin::VisualisationSession> ensureSession();

  Fooyin::EngineController* m_engine = nullptr;
  HostServer* m_host = nullptr;
  std::shared_ptr<Fooyin::VisualisationSession> m_session;
};
