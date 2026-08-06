/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTest>

#include "config/desktoplyricconfig.h"

// Persistence tests for DesktopLyricConfig (src/config/desktoplyricconfig.{h,cpp}).
//
// The user's real ~/.config/lx-lyrics/config.json is never touched:
// initTestCase() enables QStandardPaths test mode (ConfigLocation is redirected
// under the temp dir) and also overrides XDG_CONFIG_HOME to a per-process temp
// dir, so every run starts from a clean, isolated config directory.

namespace {

bool fileContainsIsLockTrue(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject()
        && doc.object().value(QStringLiteral("desktopLyric.isLock")).toBool();
}

} // namespace

class TestConfig : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void firstRunSeedsDefaults();
    void setFlushLoadRoundTrip();
    void unknownKeySetFailsFast();
    void unknownJsonKeysIgnoredOnLoad();
    void resetRestoresDefault();
    void debouncedSaveWritesFile();

private:
    QString m_configRoot;
};

void TestConfig::initTestCase()
{
    // Never touch the user's real config: test mode redirects ConfigLocation to
    // a temp dir, and XDG_CONFIG_HOME is overridden as a second safety net.
    QStandardPaths::setTestModeEnabled(true);
    m_configRoot = QDir::tempPath() + QStringLiteral("/lxlyrics-config-test-")
        + QString::number(QCoreApplication::applicationPid());
    qputenv("XDG_CONFIG_HOME", m_configRoot.toUtf8());

    // A previous (possibly aborted) run must not leak into the assertions.
    const QString configPath = DesktopLyricConfig::configFilePath();
    QDir(QFileInfo(configPath).absolutePath()).removeRecursively();
}

void TestConfig::cleanupTestCase()
{
    QDir(m_configRoot).removeRecursively();
    // Test mode puts the config under <temp>/qttest/<appname>/; remove it too.
    QDir(QDir::tempPath() + QStringLiteral("/qttest/")
         + QCoreApplication::applicationName()).removeRecursively();
}

void TestConfig::firstRunSeedsDefaults()
{
    DesktopLyricConfig config;
    config.load(); // first run: no file yet, so defaults are seeded

    // Defaults are the user's tuned preferences, not the upstream file's:
    // enable, isAlwaysOnTop, isAlwaysOnTopLoop, fullscreenHide, width,
    // fontSize, opacity, isZoomActiveLrc deviate from defaultSetting.ts.
    QCOMPARE(config.get(QStringLiteral("desktopLyric.enable")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.isLock")).toBool(), false);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.isAlwaysOnTop")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.isAlwaysOnTopLoop")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.isShowTaskbar")).toBool(), false);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.pauseHide")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.audioVisualization")).toBool(), false);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.fullscreenHide")).toBool(), false);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.isDelayScroll")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.isLockScreen")).toBool(), false); // !isWin
    QCOMPARE(config.get(QStringLiteral("desktopLyric.isHoverHide")).toBool(), false);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.isShowNoLyricMetadata")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.direction")).toString(), QStringLiteral("horizontal"));
    QCOMPARE(config.get(QStringLiteral("desktopLyric.scrollAlign")).toString(), QStringLiteral("center"));
    QCOMPARE(config.get(QStringLiteral("desktopLyric.width")).toInt(), 300);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.height")).toInt(), 300);
    QVERIFY(!config.get(QStringLiteral("desktopLyric.x")).isValid()); // null
    QVERIFY(!config.get(QStringLiteral("desktopLyric.y")).isValid()); // null
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.align")).toString(), QStringLiteral("center"));
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.font")).toString(), QString());
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.fontSize")).toInt(), 14);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.lineGap")).toInt(), 15);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.lyricUnplayColor")).toString(),
             QStringLiteral("rgba(255, 255, 255, 1)"));
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.lyricPlayedColor")).toString(),
             QStringLiteral("rgba(7, 197, 86, 1)"));
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.lyricShadowColor")).toString(),
             QStringLiteral("rgba(0, 0, 0, 0.18)"));
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.opacity")).toInt(), 100);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.ellipsis")).toBool(), false);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.isZoomActiveLrc")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.isFontWeightFont")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.isFontWeightLine")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.isFontWeightExtended")).toBool(), true);
    QVERIFY(!config.get(QStringLiteral("common.langId")).isValid()); // null
    QCOMPARE(config.get(QStringLiteral("player.isShowLyricTranslation")).toBool(), false);
    QCOMPARE(config.get(QStringLiteral("player.isShowLyricRoma")).toBool(), false);
    QCOMPARE(config.get(QStringLiteral("player.isSwapLyricTranslationAndRoma")).toBool(), false);
    QCOMPARE(config.get(QStringLiteral("player.isPlayLxlrc")).toBool(), true); // !isMac
    QCOMPARE(config.get(QStringLiteral("player.playbackRate")).toDouble(), 1.0);
}

void TestConfig::setFlushLoadRoundTrip()
{
    {
        DesktopLyricConfig a;
        QVERIFY(a.set(QStringLiteral("desktopLyric.isLock"), true));
        QVERIFY(a.set(QStringLiteral("desktopLyric.width"), 640));
        QVERIFY(a.set(QStringLiteral("desktopLyric.style.fontSize"), 33));
        QVERIFY(a.set(QStringLiteral("desktopLyric.style.lineGap"), 5));
        QVERIFY(a.set(QStringLiteral("common.langId"), QStringLiteral("zh-tw")));
        a.setPlayedColor(QColor(1, 2, 3, 128));
        QVERIFY(a.set(QStringLiteral("desktopLyric.x"), 100));
        QVERIFY(a.set(QStringLiteral("player.playbackRate"), 1.5));
        a.flush();
    } // a destroyed: its destructor flush must not corrupt the round-trip

    DesktopLyricConfig b;
    b.load();

    QCOMPARE(b.get(QStringLiteral("desktopLyric.isLock")).toBool(), true);
    QCOMPARE(b.get(QStringLiteral("desktopLyric.width")).toInt(), 640);
    QCOMPARE(b.get(QStringLiteral("desktopLyric.style.fontSize")).toInt(), 33);
    QCOMPARE(b.get(QStringLiteral("desktopLyric.style.lineGap")).toInt(), 5);
    // Regression: coerceToDefault must preserve strings for null-default keys.
    // common.langId is null by default, so a string must round-trip as a
    // string, not be coerced to an int/bool/0.
    QCOMPARE(b.get(QStringLiteral("common.langId")).toString(), QStringLiteral("zh-tw"));
    QCOMPARE(b.get(QStringLiteral("desktopLyric.x")).toInt(), 100);
    QCOMPARE(b.get(QStringLiteral("player.playbackRate")).toDouble(), 1.5);

    // The color setter stores "rgba(r, g, b, alpha)" with alpha in [0, 1];
    // 128/255 formats to "0.5" and parses back to 128, but allow ±1 for float
    // formatting drift (alphaF stringification is not lossless).
    const QColor expected(1, 2, 3, 128);
    const QColor actual = b.playedColor();
    QCOMPARE(actual.red(), expected.red());
    QCOMPARE(actual.green(), expected.green());
    QCOMPARE(actual.blue(), expected.blue());
    QVERIFY(qAbs(actual.alpha() - expected.alpha()) <= 1);
}

void TestConfig::unknownKeySetFailsFast()
{
    DesktopLyricConfig config;

    QVERIFY(!config.set(QStringLiteral("typo.key"), 1));
    QVERIFY(!config.get(QStringLiteral("typo.key")).isValid());
}

void TestConfig::unknownJsonKeysIgnoredOnLoad()
{
    const QString path = DesktopLyricConfig::configFilePath();
    QVERIFY(!path.isEmpty());
    QDir().mkpath(QFileInfo(path).absolutePath());

    // A forward-compatible config: known keys plus unknown "future.*" keys.
    const QJsonObject seeded{
        { QStringLiteral("desktopLyric.isLock"), true },
        { QStringLiteral("desktopLyric.width"), 777 },
        { QStringLiteral("desktopLyric.style.fontSize"), 42 },
        { QStringLiteral("future.unknownFeature"), QStringLiteral("must be ignored") },
        { QStringLiteral("future.other"), 123 },
    };
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(seeded).toJson(QJsonDocument::Indented));
    file.close();

    DesktopLyricConfig config;
    config.load();

    // Known keys are applied...
    QCOMPARE(config.get(QStringLiteral("desktopLyric.isLock")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.width")).toInt(), 777);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.fontSize")).toInt(), 42);
    // ...the unknown "future.*" keys are ignored, not stored, no crash.
    QVERIFY(!config.get(QStringLiteral("future.unknownFeature")).isValid());
    QVERIFY(!config.get(QStringLiteral("future.other")).isValid());
}

void TestConfig::resetRestoresDefault()
{
    DesktopLyricConfig config;

    // The tuned keys each reset from a non-default value back to the new
    // default in loadDefaults(): enable/isAlwaysOnTop/isAlwaysOnTopLoop/
    // isZoomActiveLrc -> true, fullscreenHide -> false, width -> 300,
    // fontSize -> 14, opacity -> 100.
    const QVector<QPair<QString, QVariant>> cases = {
        { QStringLiteral("desktopLyric.enable"), false },
        { QStringLiteral("desktopLyric.isAlwaysOnTop"), false },
        { QStringLiteral("desktopLyric.isAlwaysOnTopLoop"), false },
        { QStringLiteral("desktopLyric.fullscreenHide"), true },
        { QStringLiteral("desktopLyric.width"), 640 },
        { QStringLiteral("desktopLyric.style.fontSize"), 60 },
        { QStringLiteral("desktopLyric.style.opacity"), 50 },
        { QStringLiteral("desktopLyric.style.isZoomActiveLrc"), false },
    };
    for (const auto& c : cases)
        QVERIFY(config.set(c.first, c.second));
    for (const auto& c : cases)
        QVERIFY(config.reset(c.first));

    QCOMPARE(config.get(QStringLiteral("desktopLyric.enable")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.isAlwaysOnTop")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.isAlwaysOnTopLoop")).toBool(), true);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.fullscreenHide")).toBool(), false);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.width")).toInt(), 300);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.fontSize")).toInt(), 14);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.opacity")).toInt(), 100);
    QCOMPARE(config.get(QStringLiteral("desktopLyric.style.isZoomActiveLrc")).toBool(), true);
}

void TestConfig::debouncedSaveWritesFile()
{
    DesktopLyricConfig config;
    QVERIFY(config.set(QStringLiteral("desktopLyric.isLock"), true));

    // set() debounces the write by 500ms; the file must appear with the value.
    const QString path = DesktopLyricConfig::configFilePath();
    QTRY_VERIFY_WITH_TIMEOUT(fileContainsIsLockTrue(path), 3000);
}

QTEST_MAIN(TestConfig)

#include "tst_config.moc"
