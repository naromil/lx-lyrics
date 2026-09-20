/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include <QBuffer>
#include <QFile>
#include <QFileDevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include "feedwriter.h"
#include "host/feedreader.h"
#include "host/pipeio.h"
#include "host/tracklyrics.h"

#include <unistd.h>

// Round-trip tests for the player feed (docs/protocol.md v2).
//
// Both halves of the wire are compiled into this binary: the app's FeedReader
// and the plugin's FeedWriter (Fooyin-header-free precisely so it can be — the
// pattern the deleted protocol suite used for HostServer). The reader is driven
// over a synchronous in-memory pipe for the parsing slots and over a REAL
// ::pipe() for the EOF slot; the writer is driven in-process and paired with a
// stub child process so both directions are asserted as they appear on the
// wire.

namespace {

constexpr int kWaitMs = 5000;
constexpr int kAnalyserFrameBytes = 128;
constexpr int kMaxLineBytes = 1024 * 1024;

QByteArray jsonLine(const QJsonObject& object)
{
  return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray helloLine(bool spectrum)
{
  return jsonLine({
    {QStringLiteral("v"), 2},
    {QStringLiteral("action"), QStringLiteral("hello")},
    {QStringLiteral("host"), QStringLiteral("test")},
    {QStringLiteral("spectrum"), spectrum},
  });
}

QJsonObject parseLine(const QByteArray& line)
{
  return QJsonDocument::fromJson(line).object();
}

QList<QByteArray> splitLines(const QByteArray& bytes)
{
  QList<QByteArray> lines;
  for (const QByteArray& line : bytes.split('\n')) {
    if (!line.trimmed().isEmpty())
      lines.append(line);
  }
  return lines;
}

QString fixturePath(const QString& name)
{
  return QStringLiteral(LX_TEST_FIXTURE_DIR) + QLatin1Char('/') + name;
}

QByteArray readFile(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return QByteArray();
  return file.readAll();
}

// Writes the stub child's script and marks it executable; false on any
// filesystem failure (every caller QVERIFYs it).
bool writeStubScript(const QString& path, const QByteArray& body)
{
  QFile script(path);
  if (!script.open(QIODevice::WriteOnly) || script.write(body) != qint64(body.size())) {
    return false;
  }
  script.close();
  return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                       QFileDevice::ExeOwner);
}

} // namespace

// The host's end of the feed pipe, in memory: writing emits readyRead
// synchronously, so every parsing assertion below is deterministic (the real
// pair of pipe devices is exercised by eofEmitsExited()).
class HostPipe : public QIODevice {
public:
  HostPipe() { open(QIODevice::ReadWrite); }

  [[nodiscard]] bool isSequential() const override { return true; }
  [[nodiscard]] qint64 bytesAvailable() const override
  {
    return m_incoming.size() + QIODevice::bytesAvailable();
  }

  void feed(const QByteArray& bytes)
  {
    m_incoming += bytes;
    emit readyRead();
  }

protected:
  qint64 readData(char* data, qint64 maxSize) override
  {
    if (m_incoming.isEmpty())
      return 0;
    const qint64 count = std::min<qint64>(maxSize, m_incoming.size());
    std::memcpy(data, m_incoming.constData(), static_cast<size_t>(count));
    m_incoming.remove(0, count);
    return count;
  }
  qint64 writeData(const char*, qint64) override { return -1; }

private:
  QByteArray m_incoming;
};

// A FeedReader over a synchronous host pipe (stdin) and a QBuffer (stdout).
class FeedHarness {
public:
  explicit FeedHarness(bool spectrum, bool sendHello = true)
  {
    m_sink.open(QIODevice::WriteOnly);
    m_reader = std::make_unique<FeedReader>(&m_source, &m_sink);
    if (sendHello)
      write(helloLine(spectrum));
  }

  [[nodiscard]] FeedReader* reader() const { return m_reader.get(); }

  void write(const QJsonObject& object) { write(jsonLine(object)); }
  void write(const QByteArray& bytes) { m_source.feed(bytes); }

  [[nodiscard]] QList<QByteArray> writtenLines() const { return splitLines(m_sink.data()); }

private:
  HostPipe m_source;
  QBuffer m_sink;
  std::unique_ptr<FeedReader> m_reader;
};

// Refuses every write (like a sink whose reader is gone) and counts the
// attempts, so the reader's one-shot "sink is broken" guard is observable: a
// refused line must not be retried once per spectrum frame.
class RefusingSink : public QIODevice {
public:
  RefusingSink() { open(QIODevice::WriteOnly); }

  [[nodiscard]] bool isSequential() const override { return true; }
  [[nodiscard]] int writeAttempts() const { return m_attempts; }

protected:
  qint64 readData(char*, qint64) override { return -1; }
  qint64 writeData(const char*, qint64) override
  {
    ++m_attempts;
    return -1;
  }

private:
  int m_attempts = 0;
};

class TestFeed : public QObject {
  Q_OBJECT

private slots:
  void handshake();
  void strictParse_data();
  void strictParse();
  void reassemblesSplitLines();
  void overLongLineWithoutNewline();
  void setInfoLyricPrecedence();
  void lyricsForTrackFileFixtures();
  void analyserFrameSizes();
  void closeRequestedWrittenOnce();
  void refusedSinkIsNotRetried();
  void writeToBrokenPipeSurvives();
  void eofEmitsExited();
  void writerReaderRoundTrip();
  void writerExitStatus();
};

void TestFeed::handshake()
{
  FeedHarness harness(/*spectrum=*/false, /*sendHello=*/false);
  FeedReader* reader = harness.reader();
  QSignalSpy errorSpy(reader, &FeedReader::protocolError);

  // Before hello nothing is supported and no request leaves the app.
  QVERIFY(!reader->spectrumSupported());
  reader->requestAnalyserData();
  QCOMPARE(harness.writtenLines().size(), 0);

  harness.write(helloLine(false));
  QCOMPARE(errorSpy.count(), 0);
  QVERIFY(!reader->spectrumSupported());

  // A host that declared no analyser never sees a request ...
  reader->requestAnalyserData();
  QCOMPARE(harness.writtenLines().size(), 0);

  // ... but a second hello is not part of the post-handshake action list.
  harness.write(helloLine(true));
  QCOMPARE(errorSpy.count(), 1);

  // A host WITH an analyser: one action line per request.
  FeedHarness playing(/*spectrum=*/true);
  QVERIFY(playing.reader()->spectrumSupported());
  playing.reader()->requestAnalyserData();
  playing.reader()->requestAnalyserData();
  const QList<QByteArray> lines = playing.writtenLines();
  QCOMPARE(lines.size(), 2);
  QCOMPARE(parseLine(lines.at(0)).value(QStringLiteral("action")).toString(),
           QStringLiteral("get_analyser_data_array"));
  QCOMPARE(parseLine(lines.at(0)).value(QStringLiteral("v")).toInt(), 2);
}

void TestFeed::strictParse_data()
{
  QTest::addColumn<bool>("helloFirst");
  QTest::addColumn<bool>("expectError");
  QTest::addColumn<bool>("expectPlay");
  QTest::addColumn<QByteArray>("payload");

  const QByteArray hello = jsonLine({{QStringLiteral("v"), 2},
                                     {QStringLiteral("action"), QStringLiteral("hello")},
                                     {QStringLiteral("host"), QStringLiteral("test")},
                                     {QStringLiteral("spectrum"), false}});
  const QByteArray setPlay = jsonLine({{QStringLiteral("v"), 2},
                                       {QStringLiteral("action"), QStringLiteral("set_play")},
                                       {QStringLiteral("time"), 1000}});

  QTest::newRow("first message is not hello")
    << false << true << false
    << jsonLine(
         {{QStringLiteral("v"), 2}, {QStringLiteral("action"), QStringLiteral("set_status")}});
  QTest::newRow("unsupported version")
    << false << true << false
    << jsonLine({{QStringLiteral("v"), 1},
                 {QStringLiteral("action"), QStringLiteral("hello")},
                 {QStringLiteral("host"), QStringLiteral("test")}});
  QTest::newRow("missing version") << false << true << false
                                   << jsonLine({{QStringLiteral("action"), QStringLiteral("hello")},
                                                {QStringLiteral("host"), QStringLiteral("test")}});
  QTest::newRow("malformed JSON") << false << true << false << QByteArray("this is not json\n");
  QTest::newRow("non-object JSON") << false << true << false << QByteArray("[1,2,3]\n");
  QTest::newRow("empty line") << true << true << false << QByteArray("\n");
  QTest::newRow("hello without a string host")
    << false << true << false
    << jsonLine({{QStringLiteral("v"), 2},
                 {QStringLiteral("action"), QStringLiteral("hello")},
                 {QStringLiteral("host"), 7},
                 {QStringLiteral("spectrum"), false}});
  QTest::newRow("missing action") << true << true << false << jsonLine({{QStringLiteral("v"), 2}});
  QTest::newRow("unknown action")
    << true << true << false
    << jsonLine({{QStringLiteral("v"), 2}, {QStringLiteral("action"), QStringLiteral("nonsense")}});
  QTest::newRow("removed v1 action")
    << true << true << false
    << jsonLine({{QStringLiteral("v"), 2}, {QStringLiteral("action"), QStringLiteral("get_info")}});
  QTest::newRow("wrong-typed string field")
    << true << true << false
    << jsonLine({{QStringLiteral("v"), 2},
                 {QStringLiteral("action"), QStringLiteral("set_info")},
                 {QStringLiteral("name"), 42}});
  QTest::newRow("wrong-typed boolean field")
    << true << true << false
    << jsonLine({{QStringLiteral("v"), 2},
                 {QStringLiteral("action"), QStringLiteral("set_status")},
                 {QStringLiteral("isPlay"), QStringLiteral("yes")}});
  QTest::newRow("wrong-typed number field")
    << true << true << false
    << jsonLine({{QStringLiteral("v"), 2},
                 {QStringLiteral("action"), QStringLiteral("set_play")},
                 {QStringLiteral("time"), QStringLiteral("soon")}});
  QTest::newRow("over-long line") << true << true << false
                                  << QByteArray(kMaxLineBytes + 1, 'x').append('\n');
  QTest::newRow("non-integral integer field")
    << true << true << false
    << jsonLine({{QStringLiteral("v"), 2},
                 {QStringLiteral("action"), QStringLiteral("set_status")},
                 {QStringLiteral("isPlay"), true},
                 {QStringLiteral("played_time"), 1.9}});
  QTest::newRow("out-of-range integer field")
    << true << true << false
    << jsonLine({{QStringLiteral("v"), 2},
                 {QStringLiteral("action"), QStringLiteral("set_play")},
                 {QStringLiteral("time"), 9223372036854775808.0}});
  QTest::newRow("integral float integer field")
    << true << false << true
    << jsonLine({{QStringLiteral("v"), 2},
                 {QStringLiteral("action"), QStringLiteral("set_play")},
                 {QStringLiteral("time"), 1000.0}});
  QTest::newRow("valid hello") << false << false << false << hello;
  QTest::newRow("valid set_play") << true << false << true << setPlay;
}

void TestFeed::strictParse()
{
  QFETCH(bool, helloFirst);
  QFETCH(bool, expectError);
  QFETCH(bool, expectPlay);
  QFETCH(QByteArray, payload);

  FeedHarness harness(/*spectrum=*/false, helloFirst);
  QSignalSpy errorSpy(harness.reader(), &FeedReader::protocolError);
  QSignalSpy playSpy(harness.reader(), &FeedReader::playReceived);
  QSignalSpy settingsSpy(harness.reader(), &FeedReader::openSettingsRequested);

  harness.write(payload);

  QCOMPARE(errorSpy.count(), expectError ? 1 : 0);
  QCOMPARE(playSpy.count(), expectPlay ? 1 : 0);
  if (expectPlay)
    QCOMPARE(playSpy.at(0).at(0).toLongLong(), qint64(1000));

  // Fail fast, fail loud: after a violation nothing further is applied, and
  // the app never reports a second error for the same dead session.
  harness.write(jsonLine(
    {{QStringLiteral("v"), 2}, {QStringLiteral("action"), QStringLiteral("open_settings")}}));
  QCOMPARE(settingsSpy.count(), expectError ? 0 : 1);
  QCOMPARE(errorSpy.count(), expectError ? 1 : 0);
}

void TestFeed::reassemblesSplitLines()
{
  FeedHarness harness(/*spectrum=*/false, /*sendHello=*/false);
  QSignalSpy errorSpy(harness.reader(), &FeedReader::protocolError);
  QSignalSpy playSpy(harness.reader(), &FeedReader::playReceived);
  QSignalSpy statusSpy(harness.reader(), &FeedReader::statusReceived);

  // The handshake arrives in three bursts, the last boundary landing mid-token:
  // nothing may be parsed before the terminating newline shows up.
  const QByteArray hello = helloLine(true);
  harness.write(hello.left(12));
  harness.write(hello.mid(12, 7));
  QCOMPARE(errorSpy.count(), 0);
  QCOMPARE(playSpy.count(), 0);
  harness.write(hello.mid(19));
  QCOMPARE(errorSpy.count(), 0);
  QVERIFY(harness.reader()->spectrumSupported());

  // One burst carrying a whole line plus the head of the next: the loop parses
  // the complete line, keeps the remainder buffered, and applies it only once
  // its newline arrives.
  const QByteArray play = jsonLine({{QStringLiteral("v"), 2},
                                    {QStringLiteral("action"), QStringLiteral("set_play")},
                                    {QStringLiteral("time"), 1000}});
  const QByteArray status = jsonLine({{QStringLiteral("v"), 2},
                                      {QStringLiteral("action"), QStringLiteral("set_status")},
                                      {QStringLiteral("isPlay"), true},
                                      {QStringLiteral("played_time"), 2500}});
  harness.write(play + status.left(20));
  QCOMPARE(playSpy.count(), 1);
  QCOMPARE(playSpy.at(0).at(0).toLongLong(), qint64(1000));
  QCOMPARE(statusSpy.count(), 0);
  harness.write(status.mid(20));
  QCOMPARE(statusSpy.count(), 1);
  QCOMPARE(statusSpy.at(0).at(0).value<PlaybackSnapshot>().playedTimeMs, qint64(2500));

  // A CRLF terminator is framing, not payload: the '\r' never reaches JSON.
  QByteArray crlf = jsonLine({{QStringLiteral("v"), 2},
                              {QStringLiteral("action"), QStringLiteral("set_play")},
                              {QStringLiteral("time"), 4200}});
  crlf.chop(1);
  crlf.append("\r\n");
  harness.write(crlf);
  QCOMPARE(playSpy.count(), 2);
  QCOMPARE(playSpy.at(1).at(0).toLongLong(), qint64(4200));
  QCOMPARE(errorSpy.count(), 0);
}

void TestFeed::overLongLineWithoutNewline()
{
  FeedHarness harness(/*spectrum=*/false);
  QSignalSpy errorSpy(harness.reader(), &FeedReader::protocolError);
  QSignalSpy settingsSpy(harness.reader(), &FeedReader::openSettingsRequested);

  // The 1 MiB cap is enforced while the line is still arriving: a host that
  // never sends the newline must not be able to grow the buffer instead.
  harness.write(QByteArray(kMaxLineBytes + 1, 'x'));
  QCOMPARE(errorSpy.count(), 1);

  // Fail fast: the rest of that line and any later message stay unapplied, and
  // the dead session never reports a second error.
  harness.write(QByteArray("\n"));
  harness.write(jsonLine(
    {{QStringLiteral("v"), 2}, {QStringLiteral("action"), QStringLiteral("open_settings")}}));
  QCOMPARE(settingsSpy.count(), 0);
  QCOMPARE(errorSpy.count(), 1);
}

void TestFeed::setInfoLyricPrecedence()
{
  FeedHarness harness(/*spectrum=*/true);
  QSignalSpy infoSpy(harness.reader(), &FeedReader::infoReceived);

  // 1. A non-empty host `lrc` wins over anything on disk.
  harness.write({
    {QStringLiteral("v"), 2},
    {QStringLiteral("action"), QStringLiteral("set_info")},
    {QStringLiteral("path"), fixturePath(QStringLiteral("sample.mp3"))},
    {QStringLiteral("singer"), QStringLiteral("Singer")},
    {QStringLiteral("name"), QStringLiteral("Name")},
    {QStringLiteral("album"), QStringLiteral("Album")},
    {QStringLiteral("lrc"), QStringLiteral("[00:03.00]from the host")},
    {QStringLiteral("isPlay"), true},
    {QStringLiteral("played_time"), 1234},
  });
  QCOMPARE(infoSpy.count(), 1);
  TrackSnapshot info = infoSpy.at(0).at(0).value<TrackSnapshot>();
  QCOMPARE(info.lrc, QStringLiteral("[00:03.00]from the host"));
  QCOMPARE(info.singer, QStringLiteral("Singer"));
  QCOMPARE(info.name, QStringLiteral("Name"));
  QCOMPARE(info.album, QStringLiteral("Album"));
  QCOMPARE(info.path, fixturePath(QStringLiteral("sample.mp3")));
  QCOMPARE(info.isPlay, true);
  QCOMPARE(info.playedTimeMs, qint64(1234));
  // Omitted lyric keys are empty, never a protocol error (the Fooyin adapter
  // sends no lyric keys at all).
  QCOMPARE(info.tlrc, QString());
  QCOMPARE(info.lxlrc, QString());

  // 2. Empty host `lrc`: the app reads the sidecar next to `path`.
  harness.write({
    {QStringLiteral("v"), 2},
    {QStringLiteral("action"), QStringLiteral("set_info")},
    {QStringLiteral("path"), fixturePath(QStringLiteral("sample.mp3"))},
    {QStringLiteral("lrc"), QString()},
  });
  QCOMPARE(infoSpy.count(), 2);
  info = infoSpy.at(1).at(0).value<TrackSnapshot>();
  QCOMPARE(info.lrc, QString::fromUtf8(readFile(fixturePath(QStringLiteral("sample.lrc")))));

  // 3. No sidecar: the embedded tag is used.
  harness.write({
    {QStringLiteral("v"), 2},
    {QStringLiteral("action"), QStringLiteral("set_info")},
    {QStringLiteral("path"), fixturePath(QStringLiteral("embedded-lyrics.flac"))},
  });
  QCOMPARE(infoSpy.count(), 3);
  info = infoSpy.at(2).at(0).value<TrackSnapshot>();
  QCOMPARE(info.lrc, QStringLiteral("[00:00.00]Embedded line one\n[00:01.00]Embedded line two"));

  // 4. No path at all: nothing is invented.
  harness.write({
    {QStringLiteral("v"), 2},
    {QStringLiteral("action"), QStringLiteral("set_info")},
  });
  QCOMPARE(infoSpy.count(), 4);
  info = infoSpy.at(3).at(0).value<TrackSnapshot>();
  QCOMPARE(info.lrc, QString());
  QCOMPARE(info.path, QString());

  // A bare set_lyric replaces the text without touching metadata.
  QSignalSpy lyricSpy(harness.reader(), &FeedReader::lyricReceived);
  harness.write({
    {QStringLiteral("v"), 2},
    {QStringLiteral("action"), QStringLiteral("set_lyric")},
    {QStringLiteral("lrc"), QStringLiteral("[00:01.00]replacement")},
    {QStringLiteral("tlrc"), QStringLiteral("translation")},
  });
  QCOMPARE(lyricSpy.count(), 1);
  const LyricSnapshot lyric = lyricSpy.at(0).at(0).value<LyricSnapshot>();
  QCOMPARE(lyric.lrc, QStringLiteral("[00:01.00]replacement"));
  QCOMPARE(lyric.tlrc, QStringLiteral("translation"));
  QCOMPARE(lyric.rlrc, QString());
}

void TestFeed::lyricsForTrackFileFixtures()
{
  const QString sampleText = QString::fromUtf8(readFile(fixturePath(QStringLiteral("sample.lrc"))));
  QVERIFY(!sampleText.isEmpty());

  // The sidecar is found from the TRACK file's path, whatever its extension.
  QCOMPARE(lyricsForTrackFile(fixturePath(QStringLiteral("sample.mp3"))), sampleText);

  // The GBK fixture must decode to exactly the UTF-8 text (the automated form
  // of the README's `iconv -f GBK -t UTF-8` check).
  const QString gbk = lyricsForTrackFile(fixturePath(QStringLiteral("sample-gbk.mp3")));
  QCOMPARE(gbk, sampleText);
  QVERIFY(!gbk.contains(QChar(0xFFFD)));

  // Embedded tags, through TagLib.
  QCOMPARE(lyricsForTrackFile(fixturePath(QStringLiteral("embedded-lyrics.flac"))),
           QStringLiteral("[00:00.00]Embedded line one\n[00:01.00]Embedded line two"));

  // Neither source.
  QCOMPARE(lyricsForTrackFile(fixturePath(QStringLiteral("nothing-here.mp3"))), QString());
  QCOMPARE(lyricsForTrackFile(QString()), QString());

  // BOTH sources: sidecar first, then the embedded tag, '\n'-joined.
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString trackPath = dir.filePath(QStringLiteral("song.flac"));
  QVERIFY(QFile::copy(fixturePath(QStringLiteral("embedded-lyrics.flac")), trackPath));
  const QByteArray sidecarText("[00:00.00]Sidecar line");
  QFile sidecar(dir.filePath(QStringLiteral("song.lrc")));
  QVERIFY(sidecar.open(QIODevice::WriteOnly));
  QCOMPARE(sidecar.write(sidecarText), qint64(sidecarText.size()));
  sidecar.close();

  QCOMPARE(lyricsForTrackFile(trackPath),
           QString::fromUtf8(sidecarText) + QLatin1Char('\n') +
             QStringLiteral("[00:00.00]Embedded line one\n[00:01.00]Embedded line two"));

  // Decode chain spot checks: BOMs are stripped, UTF-16 is converted.
  QCOMPARE(decodeLyricsBytes(QByteArray("\xEF\xBB\xBFhello")), QStringLiteral("hello"));
  QCOMPARE(decodeLyricsBytes(QByteArray("\xFF\xFEh\x00i\x00", 6)), QStringLiteral("hi"));
  QCOMPARE(decodeLyricsBytes(QByteArray("\xFE\xFF\x00h\x00i", 6)), QStringLiteral("hi"));
  QCOMPARE(decodeLyricsBytes(QByteArray()), QString());
  // Truncated GBK/BIG5 lead bytes: neither encoding yields clean text, so the
  // chain gives up (empty) instead of returning U+FFFD soup.
  QCOMPARE(decodeLyricsBytes(QByteArray("\x81\x40\x81", 3)), QString());
}

void TestFeed::analyserFrameSizes()
{
  FeedHarness harness(/*spectrum=*/true);
  QSignalSpy frameSpy(harness.reader(), &FeedReader::analyserDataReceived);
  QSignalSpy statusSpy(harness.reader(), &FeedReader::statusReceived);
  QSignalSpy errorSpy(harness.reader(), &FeedReader::protocolError);

  QByteArray frame(kAnalyserFrameBytes, '\0');
  for (int i = 0; i < frame.size(); ++i)
    frame[i] = static_cast<char>(i);

  const auto spectrumLine = [](const QByteArray& payload) {
    return jsonLine({
      {QStringLiteral("v"), 2},
      {QStringLiteral("action"), QStringLiteral("spectrum")},
      {QStringLiteral("data"), QString::fromLatin1(payload.toBase64())},
    });
  };

  harness.write(spectrumLine(frame));
  QCOMPARE(frameSpy.count(), 1);
  QCOMPARE(frameSpy.at(0).at(0).toByteArray(), frame);
  QCOMPARE(errorSpy.count(), 0);

  // Wrong sizes are dropped loudly, and the session survives them.
  harness.write(spectrumLine(frame.left(kAnalyserFrameBytes - 1)));
  harness.write(spectrumLine(frame + QByteArray(1, 'x')));
  QCOMPARE(frameSpy.count(), 1);
  QCOMPARE(errorSpy.count(), 0);

  harness.write({
    {QStringLiteral("v"), 2},
    {QStringLiteral("action"), QStringLiteral("set_status")},
    {QStringLiteral("isPlay"), false},
    {QStringLiteral("played_time"), 999},
  });
  QCOMPARE(statusSpy.count(), 1);
  QCOMPARE(statusSpy.at(0).at(0).value<PlaybackSnapshot>().playedTimeMs, qint64(999));
  QCOMPARE(errorSpy.count(), 0);
}

void TestFeed::closeRequestedWrittenOnce()
{
  FeedHarness harness(/*spectrum=*/true);
  harness.reader()->sendCloseRequested();

  const QList<QByteArray> lines = harness.writtenLines();
  QCOMPARE(lines.size(), 1);
  const QJsonObject close = parseLine(lines.at(0));
  QCOMPARE(close.value(QStringLiteral("v")).toInt(), 2);
  QCOMPARE(close.value(QStringLiteral("action")).toString(), QStringLiteral("close_requested"));
  QCOMPARE(close.size(), 2); // no payload: version + action only
}

void TestFeed::refusedSinkIsNotRetried()
{
  HostPipe source;
  RefusingSink sink;
  FeedReader reader(&source, &sink);
  source.feed(helloLine(true));

  // The first app→host line reaches the sink and is refused ...
  reader.requestAnalyserData();
  QCOMPARE(sink.writeAttempts(), 1);

  // ... and the sink is latched broken: later frames and a user close are
  // dropped silently instead of retrying (and warning) once per frame.
  reader.requestAnalyserData();
  reader.requestAnalyserData();
  reader.sendCloseRequested();
  QCOMPARE(sink.writeAttempts(), 1);
}

void TestFeed::writeToBrokenPipeSurvives()
{
  std::array<int, 2> fds{-1, -1};
  QVERIFY(::pipe(fds.data()) == 0);

  PipeWriteDevice sink(fds[1]);
  ::close(fds[0]); // the host dropped its end of the app's stdout

  // A bare ::write() here raises SIGPIPE, whose default action kills the
  // process before the call can return EPIPE — this slot would not finish at
  // all. The device must report the failed write instead, and stay usable.
  const QByteArray line = "{\"v\":2,\"action\":\"close_requested\"}\n";
  QCOMPARE(sink.write(line), qint64(-1));
  QCOMPARE(sink.write(line), qint64(-1)); // latched: no second ::write

  ::close(fds[1]);
}

void TestFeed::eofEmitsExited()
{
  std::array<int, 2> fds{-1, -1};
  QVERIFY(::pipe(fds.data()) == 0);

  PipeReadDevice source(fds[0]);
  QBuffer sink;
  sink.open(QIODevice::WriteOnly);
  FeedReader reader(&source, &sink);

  QSignalSpy infoSpy(&reader, &FeedReader::infoReceived);
  QSignalSpy exitSpy(&reader, &FeedReader::exited);
  QSignalSpy errorSpy(&reader, &FeedReader::protocolError);

  const QByteArray bytes =
    helloLine(true) + jsonLine({{QStringLiteral("v"), 2},
                                {QStringLiteral("action"), QStringLiteral("set_info")},
                                {QStringLiteral("name"), QStringLiteral("Pipe Song")}});
  QCOMPARE(::write(fds[1], bytes.constData(), static_cast<size_t>(bytes.size())),
           static_cast<ssize_t>(bytes.size()));

  QTRY_COMPARE_WITH_TIMEOUT(infoSpy.count(), 1, kWaitMs);
  QCOMPARE(infoSpy.at(0).at(0).value<TrackSnapshot>().name, QStringLiteral("Pipe Song"));
  QCOMPARE(exitSpy.count(), 0);

  // The parent closing the pipe is the protocol's quit signal (§2).
  ::close(fds[1]);
  QTRY_COMPARE_WITH_TIMEOUT(exitSpy.count(), 1, kWaitMs);
  QCOMPARE(errorSpy.count(), 0);

  // A closed pipe cannot double-fire (the notifier is disarmed at EOF).
  QTest::qWait(50);
  QCOMPARE(exitSpy.count(), 1);
  QCOMPARE(splitLines(sink.data()).size(), 0);

  // §4 is about the USER closing the window: after the player ended the
  // session nothing is reported back (the app's shutdown fires a window close
  // event, which must not look like a user close).
  reader.sendCloseRequested();
  reader.requestAnalyserData();
  QCOMPARE(splitLines(sink.data()).size(), 0);

  ::close(fds[0]);
}

void TestFeed::writerReaderRoundTrip()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString scriptPath = dir.filePath(QStringLiteral("stub-app.sh"));
  const QString receivedPath = dir.filePath(QStringLiteral("received.txt"));

  // Stub child standing in for the display app: it immediately reports the two
  // app→host actions and logs every host→app line it reads.
  QVERIFY(writeStubScript(
    scriptPath, QByteArray("#!/bin/sh\n"
                           "dir=$(dirname \"$0\")\n"
                           "printf '{\"v\":2,\"action\":\"get_analyser_data_array\"}\\n'\n"
                           "printf '{\"v\":2,\"action\":\"close_requested\"}\\n'\n"
                           "while IFS= read -r line; do\n"
                           "  printf '%s\\n' \"$line\" >> \"$dir/received.txt\"\n"
                           "done\n")));

  FeedWriter writer;
  writer.setAppPath(scriptPath);
  writer.setSpectrumAvailable(true);

  QSignalSpy startedSpy(&writer, &FeedWriter::appStarted);
  QSignalSpy analyserSpy(&writer, &FeedWriter::analyserDataRequested);
  QSignalSpy closeSpy(&writer, &FeedWriter::closeRequested);

  QVERIFY(writer.spawn());
  QCOMPARE(startedSpy.count(), 1);
  QVERIFY(writer.isRunning());

  // app→host: the child's two lines are parsed strictly into the two actions.
  QTRY_COMPARE_WITH_TIMEOUT(analyserSpy.count(), 1, kWaitMs);
  QTRY_COMPARE_WITH_TIMEOUT(closeSpy.count(), 1, kWaitMs);

  // host→app: every helper writes exactly one v2 line, in order.
  writer.sendSetInfo({
    {QStringLiteral("path"), QStringLiteral("/music/song.mp3")},
    {QStringLiteral("singer"), QStringLiteral("Singer")},
    {QStringLiteral("name"), QStringLiteral("Name")},
    {QStringLiteral("album"), QStringLiteral("Album")},
    {QStringLiteral("isPlay"), true},
    {QStringLiteral("played_time"), 4242},
  });
  writer.sendSetLyric(QStringLiteral("lrc"), QStringLiteral("tlrc"), QString(), QString());
  writer.sendSetStatus(true, 1000);
  writer.sendSetOffset(-250);
  writer.sendSetPlaybackRate(1.5);
  writer.sendSetPlay(3000);
  writer.sendSetPause();
  writer.sendSetStop();
  writer.sendSetFullscreen(true);
  writer.sendOpenSettings();
  writer.sendAnalyserData(QByteArray(kAnalyserFrameBytes, '\x7f'));
  writer.sendAnalyserData(QByteArray(kAnalyserFrameBytes - 1, '\x7f')); // refused, not sent

  constexpr int kExpectedLines = 12;
  QTRY_VERIFY_WITH_TIMEOUT(splitLines(readFile(receivedPath)).size() >= kExpectedLines, kWaitMs);

  const QList<QByteArray> lines = splitLines(readFile(receivedPath));
  QCOMPARE(lines.size(), kExpectedLines);

  const QJsonObject hello = parseLine(lines.at(0));
  QCOMPARE(hello.value(QStringLiteral("v")).toInt(), 2);
  QCOMPARE(hello.value(QStringLiteral("action")).toString(), QStringLiteral("hello"));
  QCOMPARE(hello.value(QStringLiteral("host")).toString(), QStringLiteral("fooyin"));
  QCOMPARE(hello.value(QStringLiteral("spectrum")).toBool(), true);

  const QJsonObject info = parseLine(lines.at(1));
  QCOMPARE(info.value(QStringLiteral("action")).toString(), QStringLiteral("set_info"));
  QCOMPARE(info.value(QStringLiteral("v")).toInt(), 2);
  QCOMPARE(info.value(QStringLiteral("path")).toString(), QStringLiteral("/music/song.mp3"));
  QCOMPARE(info.value(QStringLiteral("singer")).toString(), QStringLiteral("Singer"));
  QCOMPARE(info.value(QStringLiteral("name")).toString(), QStringLiteral("Name"));
  QCOMPARE(info.value(QStringLiteral("album")).toString(), QStringLiteral("Album"));
  QCOMPARE(info.value(QStringLiteral("isPlay")).toBool(), true);
  QCOMPARE(info.value(QStringLiteral("played_time")).toInteger(), qint64(4242));

  const QJsonObject lyric = parseLine(lines.at(2));
  QCOMPARE(lyric.value(QStringLiteral("action")).toString(), QStringLiteral("set_lyric"));
  QCOMPARE(lyric.value(QStringLiteral("v")).toInt(), 2);
  QCOMPARE(lyric.value(QStringLiteral("lrc")).toString(), QStringLiteral("lrc"));
  QCOMPARE(lyric.value(QStringLiteral("tlrc")).toString(), QStringLiteral("tlrc"));
  QCOMPARE(lyric.value(QStringLiteral("rlrc")).toString(), QString());
  QCOMPARE(lyric.value(QStringLiteral("lxlrc")).toString(), QString());

  const QJsonObject status = parseLine(lines.at(3));
  QCOMPARE(status.value(QStringLiteral("action")).toString(), QStringLiteral("set_status"));
  QCOMPARE(status.value(QStringLiteral("isPlay")).toBool(), true);
  QCOMPARE(status.value(QStringLiteral("played_time")).toInteger(), qint64(1000));

  const QJsonObject offset = parseLine(lines.at(4));
  QCOMPARE(offset.value(QStringLiteral("action")).toString(), QStringLiteral("set_offset"));
  QCOMPARE(offset.value(QStringLiteral("tempOffset")).toInteger(), qint64(-250));

  const QJsonObject rate = parseLine(lines.at(5));
  QCOMPARE(rate.value(QStringLiteral("action")).toString(), QStringLiteral("set_playbackRate"));
  QCOMPARE(rate.value(QStringLiteral("rate")).toDouble(), 1.5);

  const QJsonObject play = parseLine(lines.at(6));
  QCOMPARE(play.value(QStringLiteral("action")).toString(), QStringLiteral("set_play"));
  QCOMPARE(play.value(QStringLiteral("time")).toInteger(), qint64(3000));

  QCOMPARE(parseLine(lines.at(7)).value(QStringLiteral("action")).toString(),
           QStringLiteral("set_pause"));
  QCOMPARE(parseLine(lines.at(8)).value(QStringLiteral("action")).toString(),
           QStringLiteral("set_stop"));
  QCOMPARE(parseLine(lines.at(9)).value(QStringLiteral("action")).toString(),
           QStringLiteral("set_fullscreen"));
  QCOMPARE(parseLine(lines.at(9)).value(QStringLiteral("isFullscreen")).toBool(), true);
  QCOMPARE(parseLine(lines.at(10)).value(QStringLiteral("action")).toString(),
           QStringLiteral("open_settings"));

  const QJsonObject spectrum = parseLine(lines.at(11));
  QCOMPARE(spectrum.value(QStringLiteral("action")).toString(), QStringLiteral("spectrum"));
  QCOMPARE(spectrum.value(QStringLiteral("v")).toInt(), 2);
  QCOMPARE(QByteArray::fromBase64(spectrum.value(QStringLiteral("data")).toString().toLatin1()),
           QByteArray(kAnalyserFrameBytes, '\x7f'));

  // Close the loop: the exact lines the child received ARE a valid session for
  // the app's own reader — FeedWriter's output parses into the same typed
  // snapshots the app renders from.
  HostPipe hostPipe;
  QBuffer appSink;
  appSink.open(QIODevice::WriteOnly);
  FeedReader reader(&hostPipe, &appSink);
  QSignalSpy infoSpy(&reader, &FeedReader::infoReceived);
  QSignalSpy lyricSpy(&reader, &FeedReader::lyricReceived);
  QSignalSpy statusSpy(&reader, &FeedReader::statusReceived);
  QSignalSpy offsetSpy(&reader, &FeedReader::offsetReceived);
  QSignalSpy rateSpy(&reader, &FeedReader::playbackRateReceived);
  QSignalSpy playSpy(&reader, &FeedReader::playReceived);
  QSignalSpy pauseSpy(&reader, &FeedReader::pauseReceived);
  QSignalSpy stopSpy(&reader, &FeedReader::stopReceived);
  QSignalSpy fullscreenSpy(&reader, &FeedReader::fullscreenReceived);
  QSignalSpy settingsSpy(&reader, &FeedReader::openSettingsRequested);
  QSignalSpy frameSpy(&reader, &FeedReader::analyserDataReceived);
  QSignalSpy errorSpy(&reader, &FeedReader::protocolError);

  for (const QByteArray& line : lines)
    hostPipe.feed(line + '\n');

  QCOMPARE(errorSpy.count(), 0);
  QCOMPARE(infoSpy.count(), 1);
  const TrackSnapshot snapshot = infoSpy.at(0).at(0).value<TrackSnapshot>();
  QCOMPARE(snapshot.name, QStringLiteral("Name"));
  QCOMPARE(snapshot.singer, QStringLiteral("Singer"));
  QCOMPARE(snapshot.path, QStringLiteral("/music/song.mp3"));
  QCOMPARE(snapshot.isPlay, true);
  QCOMPARE(snapshot.playedTimeMs, qint64(4242));
  QCOMPARE(lyricSpy.count(), 1);
  QCOMPARE(statusSpy.count(), 1);
  QCOMPARE(statusSpy.at(0).at(0).value<PlaybackSnapshot>().playedTimeMs, qint64(1000));
  QCOMPARE(offsetSpy.count(), 1);
  QCOMPARE(offsetSpy.at(0).at(0).toLongLong(), qint64(-250));
  QCOMPARE(rateSpy.count(), 1);
  QCOMPARE(rateSpy.at(0).at(0).toDouble(), 1.5);
  QCOMPARE(playSpy.count(), 1);
  QCOMPARE(playSpy.at(0).at(0).toLongLong(), qint64(3000));
  QCOMPARE(pauseSpy.count(), 1);
  QCOMPARE(stopSpy.count(), 1);
  QCOMPARE(fullscreenSpy.count(), 1);
  QCOMPARE(fullscreenSpy.at(0).at(0).toBool(), true);
  QCOMPARE(settingsSpy.count(), 1);
  QCOMPARE(frameSpy.count(), 1);
  QCOMPARE(frameSpy.at(0).at(0).toByteArray(), QByteArray(kAnalyserFrameBytes, '\x7f'));

  // The host→app line for a spectrum request never appears: the app asks, the
  // host answers. The stub's own two lines are the only app→host traffic.
  QSignalSpy exitedSpy(&writer, &FeedWriter::appExited);

  // stop() is the plugin-initiated teardown: stdin EOF, child exits, and no
  // appExited() (the session was not lost).
  writer.stop();
  QVERIFY(!writer.isRunning());
  QCOMPARE(exitedSpy.count(), 0);
}

void TestFeed::writerExitStatus()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());

  { // A clean exit is reported as the child's own status.
    const QString scriptPath = dir.filePath(QStringLiteral("clean-exit.sh"));
    QVERIFY(writeStubScript(scriptPath, QByteArray("#!/bin/sh\nexit 3\n")));

    FeedWriter writer;
    writer.setAppPath(scriptPath);
    QSignalSpy exitedSpy(&writer, &FeedWriter::appExited);
    QVERIFY(writer.spawn());
    QTRY_COMPARE_WITH_TIMEOUT(exitedSpy.count(), 1, kWaitMs);
    QCOMPARE(exitedSpy.at(0).at(0).toInt(), 3);
    QCOMPARE(exitedSpy.at(0).at(1).toBool(), false);
    QCOMPARE(writer.exitCode(), 3);
  }

  { // close_requested before the exit marks it as a user close.
    const QString scriptPath = dir.filePath(QStringLiteral("close-exit.sh"));
    writeStubScript(scriptPath, QByteArray("#!/bin/sh\n"
                                           "printf '{\"v\":2,\"action\":\"close_requested\"}\\n'\n"
                                           "exit 0\n"));

    FeedWriter writer;
    writer.setAppPath(scriptPath);
    QSignalSpy exitedSpy(&writer, &FeedWriter::appExited);
    QSignalSpy closeSpy(&writer, &FeedWriter::closeRequested);
    QVERIFY(writer.spawn());
    QTRY_COMPARE_WITH_TIMEOUT(exitedSpy.count(), 1, kWaitMs);
    QCOMPARE(closeSpy.count(), 1);
    QCOMPARE(exitedSpy.at(0).at(0).toInt(), 0);
    QCOMPARE(exitedSpy.at(0).at(1).toBool(), true);
  }

  { // Noise on the app's stdout is a protocol error (§7): the session is
    // terminated and reported non-zero, never respawned as if it were healthy.
    const QString scriptPath = dir.filePath(QStringLiteral("garbage.sh"));
    writeStubScript(scriptPath, QByteArray("#!/bin/sh\n"
                                           "printf 'not json at all\\n'\n"
                                           "sleep 30\n"));

    FeedWriter writer;
    writer.setAppPath(scriptPath);
    QSignalSpy exitedSpy(&writer, &FeedWriter::appExited);
    QVERIFY(writer.spawn());
    QTRY_COMPARE_WITH_TIMEOUT(exitedSpy.count(), 1, kWaitMs);
    QCOMPARE(exitedSpy.at(0).at(1).toBool(), false);
    QVERIFY(exitedSpy.at(0).at(0).toInt() != 0);
    QVERIFY(!writer.isRunning());
  }
}

QTEST_GUILESS_MAIN(TestFeed)

#include "tst_feed.moc"
