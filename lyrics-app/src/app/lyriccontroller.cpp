/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
// Assembles the lyric pipeline into the lyric window (task 2.13):
// LyricSelector -> LyricPlayer -> LyricRenderer, driven by the host protocol's
// typed snapshots (docs/protocol.md §5) and kept live with DesktopLyricConfig.
//
// The pipeline mirrors the reference renderer-lyric window:
// - core/lyric.ts setLyric() selects lxlrc/lrc and builds the extended list,
//   exactly like LyricSelector.
// - core/mainWindowChannel.ts maps set_info/set_lyric/set_status/set_offset/
//   set_playbackRate/set_play/set_pause/set_stop onto those entry points.
// - index.js's onPlay/onSetLyric drive the view's active line; here the
//   renderer paints line-by-line (active line in the played color, inactive
//   lines in the unplay color) with no per-word karaoke fill.

#include "app/lyriccontroller.h"

#include <QVBoxLayout>
#include <QVariant>

#include "app/appcontext.h"
#include "bridge/wsclient.h"
#include "config/desktoplyricconfig.h"
#include "engine/lyricplayer.h"
#include "engine/lyricselector.h"
#include "renderer/lyricrenderer.h"
#include "window/lyricwindow.h"

namespace {

const QString kKeyFont = QStringLiteral("desktopLyric.style.font");
const QString kKeyFontSize = QStringLiteral("desktopLyric.style.fontSize");
const QString kKeyLineGap = QStringLiteral("desktopLyric.style.lineGap");
const QString kKeyOpacity = QStringLiteral("desktopLyric.style.opacity");
const QString kKeyEllipsis = QStringLiteral("desktopLyric.style.ellipsis");
const QString kKeyZoomActiveLrc = QStringLiteral("desktopLyric.style.isZoomActiveLrc");
const QString kKeyFontWeightFont = QStringLiteral("desktopLyric.style.isFontWeightFont");
const QString kKeyFontWeightLine = QStringLiteral("desktopLyric.style.isFontWeightLine");
const QString kKeyFontWeightExtended = QStringLiteral("desktopLyric.style.isFontWeightExtended");
const QString kKeyUnplayColor = QStringLiteral("desktopLyric.style.lyricUnplayColor");
const QString kKeyPlayedColor = QStringLiteral("desktopLyric.style.lyricPlayedColor");
const QString kKeyShadowColor = QStringLiteral("desktopLyric.style.lyricShadowColor");
const QString kKeyDirection = QStringLiteral("desktopLyric.direction");
const QString kKeyScrollAlign = QStringLiteral("desktopLyric.scrollAlign");
const QString kKeyDelayScroll = QStringLiteral("desktopLyric.isDelayScroll");
const QString kKeyAlign = QStringLiteral("desktopLyric.style.align");
const QString kKeyIsLock = QStringLiteral("desktopLyric.isLock");
const QString kKeyShowNoLyricMetadata = QStringLiteral("desktopLyric.isShowNoLyricMetadata");

const QString kKeyPlayLxlrc = QStringLiteral("player.isPlayLxlrc");
const QString kKeyShowTranslation = QStringLiteral("player.isShowLyricTranslation");
const QString kKeyShowRoma = QStringLiteral("player.isShowLyricRoma");
const QString kKeySwapTranslationAndRoma = QStringLiteral("player.isSwapLyricTranslationAndRoma");
const QString kKeyPlaybackRate = QStringLiteral("player.playbackRate");

// Keys whose change must re-apply the renderer styling.
bool isRendererKey(const QString& key)
{
    static const QStringList keys = {
        kKeyFont, kKeyFontSize, kKeyLineGap, kKeyOpacity, kKeyEllipsis, kKeyZoomActiveLrc,
        kKeyFontWeightFont, kKeyFontWeightLine, kKeyFontWeightExtended,
        kKeyUnplayColor, kKeyPlayedColor, kKeyShadowColor,
        kKeyDirection, kKeyScrollAlign, kKeyDelayScroll, kKeyAlign, kKeyIsLock,
    };
    return keys.contains(key);
}

// Keys whose change must re-select the lyric (reference useLyric.ts watches).
bool isSelectorKey(const QString& key)
{
    static const QStringList keys = {
        kKeyPlayLxlrc, kKeyShowTranslation, kKeyShowRoma, kKeySwapTranslationAndRoma,
    };
    return keys.contains(key);
}

Qt::Alignment alignFromString(const QString& align)
{
    if (align == QLatin1String("left"))
        return Qt::AlignLeft;
    if (align == QLatin1String("right"))
        return Qt::AlignRight;
    return Qt::AlignHCenter;
}

} // namespace

LyricController::LyricController(AppContext& ctx, LyricWindow& window, QObject* parent)
    : QObject(parent)
    , m_ctx(ctx)
    , m_window(window)
{
    m_player = new LyricPlayer(this);
    m_selector = std::make_unique<LyricSelector>();
    m_renderer = new LyricRenderer(window.contentContainer());

    // contentContainer's QVBoxLayout is [control bar, spectrum, trailing
    // stretch]. Insert the renderer between the spectrum and the stretch so it
    // expands into the lyric area; the trailing stretch then collapses to zero.
    auto* layout = qobject_cast<QVBoxLayout*>(window.contentContainer()->layout());
    layout->insertWidget(layout->count() - 1, m_renderer, 1);
    layout->setStretch(layout->count() - 1, 0);

    // Player -> renderer: lyricsChanged rebuilds the render lines; lineChanged
    // moves the active line.
    connect(m_player, &LyricPlayer::lyricsChanged,
            this, &LyricController::pushLyricsToRenderer);
    connect(m_player, &LyricPlayer::lineChanged,
            this, &LyricController::onLineChanged);

    connect(&m_ctx.config, &DesktopLyricConfig::settingChanged,
            this, &LyricController::onSettingChanged);

    applyRendererConfig();
    applySelectorConfig();
    m_player->setPlaybackRate(m_ctx.config.get(kKeyPlaybackRate).toDouble());
}

LyricController::~LyricController() = default;

void LyricController::setTrack(const TrackSnapshot& track)
{
    // Remember the metadata: the no-lyrics placeholder shows the track
    // title/artist/album when the player has no lyric lines to paint.
    m_trackName = track.name;
    m_trackSinger = track.singer;
    m_trackAlbum = track.album;

    applyLyricText(track.lrc, track.tlrc, track.rlrc, track.lxlrc);

    // Window title from the track metadata (name - singer).
    const QString title = track.singer.isEmpty()
        ? track.name
        : QStringLiteral("%1 - %2").arg(track.name, track.singer);
    m_window.setWindowTitle(title);
}

void LyricController::setLyric(const LyricSnapshot& lyric)
{
    applyLyricText(lyric.lrc, lyric.tlrc, lyric.rlrc, lyric.lxlrc);
}

void LyricController::setStatus(const PlaybackSnapshot& status)
{
    if (status.isPlay)
        play(status.playedTimeMs);
    else
        pause();
}

void LyricController::play(qint64 timeMs)
{
    m_player->play(timeMs);
    // The renderer's 3 s resume re-center only runs while playing (reference
    // isPlay gate in startLyricScrollTimeout).
    m_renderer->setPlaying(true);
}

void LyricController::pause()
{
    m_player->pause();
    m_renderer->setPlaying(false);
}

void LyricController::stop()
{
    // player.stop() clears the lyric and emits lyricsChanged + lineChanged(-1),
    // which pushLyricsToRenderer turns into the blank renderer state.
    m_player->stop();
    m_renderer->setPlaying(false);
}

void LyricController::setOffset(qint64 tempOffset)
{
    // Protocol §5: set_offset carries a DELTA from the lyric's own [offset:]
    // tag. Accumulate into the user offset, then let the player re-anchor.
    m_userOffsetMs += tempOffset;
    m_player->setOffset(m_userOffsetMs);
}

void LyricController::setPlaybackRate(double rate)
{
    m_player->setPlaybackRate(rate);
}

void LyricController::applyRendererConfig()
{
    DesktopLyricConfig& config = m_ctx.config;
    m_renderer->setFontFamily(config.get(kKeyFont).toString());
    m_renderer->setFontSize(config.fontSize());
    m_renderer->setLineGap(config.get(kKeyLineGap).toInt());
    m_renderer->setOpacityPercent(config.opacity());
    m_renderer->setEllipsis(config.get(kKeyEllipsis).toBool());
    m_renderer->setZoomActiveLrc(config.get(kKeyZoomActiveLrc).toBool());
    m_renderer->setFontWeightFont(config.get(kKeyFontWeightFont).toBool());
    m_renderer->setFontWeightLine(config.get(kKeyFontWeightLine).toBool());
    m_renderer->setFontWeightExtended(config.get(kKeyFontWeightExtended).toBool());
    m_renderer->setUnplayColor(config.unplayColor());
    m_renderer->setPlayedColor(config.playedColor());
    m_renderer->setShadowColor(config.shadowColor());
    m_renderer->setVertical(config.direction() == QStringLiteral("vertical"));
    m_renderer->setScrollAlign(config.get(kKeyScrollAlign).toString() == QStringLiteral("top"));
    m_renderer->setDelayScroll(config.get(kKeyDelayScroll).toBool());
    m_renderer->setAlign(alignFromString(config.get(kKeyAlign).toString()));
    m_renderer->setInteractive(!config.isLock()); // wheel/drag gated off when locked.
}

void LyricController::applySelectorConfig()
{
    DesktopLyricConfig& config = m_ctx.config;
    m_selector->setPlayLxlrc(config.get(kKeyPlayLxlrc).toBool());
    m_selector->setIsShowLyricTranslation(config.get(kKeyShowTranslation).toBool());
    m_selector->setIsShowLyricRoma(config.get(kKeyShowRoma).toBool());
    m_selector->setIsSwapLyricTranslationAndRoma(config.get(kKeySwapTranslationAndRoma).toBool());
}

void LyricController::applyLyricText(const QString& lrc, const QString& tlyric,
                                     const QString& rlyric, const QString& lxlyric)
{
    m_lastLrc = lrc;
    m_lastTlrc = tlyric;
    m_lastRlrc = rlyric;
    m_lastLxlrc = lxlyric;
    m_hasLyric = true;
    reapplyLyricSelection();
}

bool LyricController::reapplyLyricSelection()
{
    // Re-run the selector over the last raw fields: [awlrc:] extraction is
    // idempotent, so re-feeding is safe for config-driven re-selection too.
    m_selector->setLyrics(m_lastLrc, m_lastTlrc, m_lastRlrc, m_lastLxlrc);
    if (m_selector->hasLyrics()) {
        return m_player->setLyric(m_selector->selectedLyric(), m_selector->extendedLyrics());
    }
    // No playable lyric (no embedded tags, no sidecar .lrc): clear any stale
    // player lines (a later play() must not resurrect the previous track), then
    // show the track placeholder so the window isn't a blank pane. The changed
    // flag feeds the caller's resume decision (setLyric("") returns true when
    // it cleared a previous lyric, false when the player was already empty).
    const bool changed = m_player->setLyric(QString());
    showNoLyricsPlaceholder();
    return changed;
}

void LyricController::showNoLyricsPlaceholder()
{
    // Track title / artist / album as three inactive lines in the unplay
    // color — centered and never highlighted (setActiveLine(-1) keeps the
    // active-line color and zoom off), so the window shows the track instead
    // of a blank pane. Empty fields are skipped; a track with no metadata at
    // all falls back to a plain "No lyrics". Replaced by the real lines as
    // soon as a lyric arrives (pushLyricsToRenderer feeds from the player).
    QVector<RenderLine> lines;
    if (m_ctx.config.get(kKeyShowNoLyricMetadata).toBool()) {
        if (m_trackName.isEmpty())
            lines.append(RenderLine{ QStringLiteral("No lyrics"), {} });
        else
            lines.append(RenderLine{ m_trackName, {} });
        if (!m_trackSinger.isEmpty())
            lines.append(RenderLine{ m_trackSinger, {} });
        if (!m_trackAlbum.isEmpty())
            lines.append(RenderLine{ m_trackAlbum, {} });
    }
    // Toggle off, or a track with no metadata at all: the plain literal.
    if (lines.isEmpty())
        lines.append(RenderLine{ QStringLiteral("No lyrics"), {} });
    m_renderer->setLines(lines);
    m_renderer->setActiveLine(-1);
    // Center the block in the viewport; the lines still follow the alignment
    // (and font/color) settings.
    m_renderer->setCenteredBlock(true);
}

void LyricController::pushLyricsToRenderer()
{
    // Map the player's parsed lines onto the renderer's line groups. The
    // renderer strips any karaoke word tags and paints line-by-line.
    QVector<RenderLine> lines;
    const QVector<LrcLine>& playerLines = m_player->lines();
    lines.reserve(playerLines.size());
    for (const LrcLine& line : playerLines)
        lines.append({ line.text, line.extendedLyrics });

    m_renderer->setCenteredBlock(false); // Real lyrics: back to the scroll layout.
    m_renderer->setLines(lines);
    m_renderer->setActiveLine(m_player->currentLine());
}

void LyricController::onSettingChanged(const QString& key, const QVariant& value)
{
    if (key == kKeyShowNoLyricMetadata) {
        // Live apply: re-running the selector re-reads the new value inside
        // showNoLyricsPlaceholder. Must return here so the key never falls
        // through to the renderer/selector dispatch below.
        if (!m_selector->hasLyrics())
            reapplyLyricSelection();
        return;
    }
    if (isRendererKey(key)) {
        applyRendererConfig();
        return;
    }
    if (isSelectorKey(key)) {
        applySelectorConfig();
        if (!m_hasLyric)
            return;
        const bool wasPlaying = m_player->isPlaying();
        const qint64 positionMs = m_player->currentPositionMs();
        const bool applied = reapplyLyricSelection();
        // Resume only when the lyric actually changed (a dedup'd re-selection
        // never paused the player, so replaying would re-emit the current line
        // spuriously) and there are lines to play.
        if (applied && wasPlaying && m_selector->hasLyrics())
            m_player->play(positionMs);
        // Mirror the play state — isPlaying() is the single source of truth for
        // the renderer's 3 s resume re-center gate.
        m_renderer->setPlaying(m_player->isPlaying());
        return;
    }
    if (key == kKeyPlaybackRate)
        m_player->setPlaybackRate(value.toDouble());
}

void LyricController::onLineChanged(int line)
{
    m_renderer->setActiveLine(line);
}
