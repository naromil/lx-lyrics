/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QObject>
#include <QString>

#include <memory>

class AppContext;
class LyricPlayer;
class LyricRenderer;
class LyricSelector;
class LyricWindow;
struct TrackSnapshot;
struct LyricSnapshot;
struct PlaybackSnapshot;

// Assembles the full lyric pipeline — LyricSelector (lyric selection) ->
// LyricPlayer (timing) -> LyricRenderer (painting) — into the lyric window and
// keeps every stage in sync with DesktopLyricConfig.
//
// Music-state entry points mirror the host protocol (docs/protocol.md §5):
// setTrack/setLyric ingest lyric snapshots, play/pause/stop/setStatus drive the
// player, setOffset accumulates the delta sent by the host, setPlaybackRate
// forwards the rate.
class LyricController : public QObject {
    Q_OBJECT

public:
    explicit LyricController(AppContext& ctx, LyricWindow& window, QObject* parent = nullptr);
    ~LyricController() override;

    // --- music-state entry points (WsClient signals and --demo self-feed) ---
    void setTrack(const TrackSnapshot& track);
    void setLyric(const LyricSnapshot& lyric);
    void setStatus(const PlaybackSnapshot& status);
    void play(qint64 timeMs);
    void pause();
    void stop();
    void setOffset(qint64 tempOffset);
    void setPlaybackRate(double rate);

private:
    void applyRendererConfig();
    void applySelectorConfig();
    void applyLyricText(const QString& lrc, const QString& tlyric,
                        const QString& rlyric, const QString& lxlyric);
    void reapplyLyricSelection();
    void pushLyricsToRenderer();
    void onSettingChanged(const QString& key, const QVariant& value);
    void onLineChanged(int line);

    AppContext& m_ctx;
    LyricWindow& m_window;
    LyricRenderer* m_renderer = nullptr;
    LyricPlayer* m_player = nullptr;
    std::unique_ptr<LyricSelector> m_selector;
    // Last raw lyric fields, kept so a selection-config change can re-select
    // without a fresh host snapshot.
    QString m_lastLrc, m_lastTlrc, m_lastRlrc, m_lastLxlrc;
    bool m_hasLyric = false;
    qint64 m_userOffsetMs = 0;
};
