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
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include "config/desktoplyricconfig.h"
#include "i18n/translationmanager.h"

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
  return doc.isObject() && doc.object().value(QStringLiteral("desktopLyric.isLock")).toBool();
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
  void strictJsonTypesFallBackToDefaults();
  void wrongTypedSetLeavesValueUntouched();
  void resetRestoresDefault();
  void debouncedSaveWritesFile();
  void parseRgbaStringEdgeCases_data();
  void parseRgbaStringEdgeCases();
  void rgbaStringFromColorRoundTrips();

  void translationLocaleMapping_data();
  void translationLocaleMapping();
  void translationUnsupportedLangIdFallsBackToEnglish();
  void translationLiveSwitchEmitsLanguageChanged();
  void translationPacksShareKeySet();

  // moc requires an explicit access specifier to terminate the private slots region;
  // without it m_configRoot is parsed as a slot declaration and moc fails.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
private:
  QString m_configRoot;
};

void TestConfig::initTestCase()
{
  // Never touch the user's real config: test mode redirects ConfigLocation to
  // a temp dir, and XDG_CONFIG_HOME is overridden as a second safety net.
  QStandardPaths::setTestModeEnabled(true);
  m_configRoot = QDir::tempPath() + QStringLiteral("/lxlyrics-config-test-") +
                 QString::number(QCoreApplication::applicationPid());
  qputenv("XDG_CONFIG_HOME", m_configRoot.toUtf8());

  // A previous (possibly aborted) run must not leak into the assertions.
  const QString configPath = DesktopLyricConfig::configFilePath();
  QDir(QFileInfo(configPath).absolutePath()).removeRecursively();
}

void TestConfig::cleanupTestCase()
{
  QDir(m_configRoot).removeRecursively();
  // Test mode puts the config under <temp>/qttest/<appname>/; remove it too.
  QDir(QDir::tempPath() + QStringLiteral("/qttest/") + QCoreApplication::applicationName())
    .removeRecursively();
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
  QCOMPARE(config.get(QStringLiteral("desktopLyric.direction")).toString(),
           QStringLiteral("horizontal"));
  QCOMPARE(config.get(QStringLiteral("desktopLyric.scrollAlign")).toString(),
           QStringLiteral("center"));
  QCOMPARE(config.get(QStringLiteral("desktopLyric.width")).toInt(), 300);
  QCOMPARE(config.get(QStringLiteral("desktopLyric.height")).toInt(), 300);
  QVERIFY(!config.get(QStringLiteral("desktopLyric.x")).isValid()); // null
  QVERIFY(!config.get(QStringLiteral("desktopLyric.y")).isValid()); // null
  QCOMPARE(config.get(QStringLiteral("desktopLyric.style.align")).toString(),
           QStringLiteral("center"));
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
    {QStringLiteral("desktopLyric.isLock"), true},
    {QStringLiteral("desktopLyric.width"), 777},
    {QStringLiteral("desktopLyric.style.fontSize"), 42},
    {QStringLiteral("future.unknownFeature"), QStringLiteral("must be ignored")},
    {QStringLiteral("future.other"), 123},
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

void TestConfig::strictJsonTypesFallBackToDefaults()
{
  // REGRESSION: QVariant::toBool() reads the STRING "false" as true and
  // QVariant::toInt() turns "18"/true into 18/1, so a hand-edited or
  // malformed config silently changed a setting. A wrong-typed JSON value
  // must fall back to the key's default instead. A JSON number is still a
  // valid Int when integral and in range (width 400.0 -> 400); Qt 6's
  // QJsonValue::toVariant() delivers it as a qlonglong.
  const QString path = DesktopLyricConfig::configFilePath();
  QVERIFY(!path.isEmpty());
  QDir().mkpath(QFileInfo(path).absolutePath());

  const QJsonObject seeded{
    {QStringLiteral("desktopLyric.isLock"), QStringLiteral("false")},
    {QStringLiteral("desktopLyric.style.fontSize"), QStringLiteral("18")},
    {QStringLiteral("desktopLyric.width"), 400.0},
    {QStringLiteral("desktopLyric.style.opacity"), true},
    {QStringLiteral("desktopLyric.isShowTaskbar"), true},
    {QStringLiteral("desktopLyric.direction"), QStringLiteral("vertical")},
    {QStringLiteral("player.playbackRate"), 1.5},
  };
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write(QJsonDocument(seeded).toJson(QJsonDocument::Indented));
  file.close();

  DesktopLyricConfig config;
  config.load();

  QCOMPARE(config.isLock(), false); // "false" is not a boolean.
  QCOMPARE(config.fontSize(), 14);  // "18" is not a number.
  QCOMPARE(config.get(QStringLiteral("desktopLyric.style.opacity")).toInt(),
           100); // true is not an int.
  QCOMPARE(config.get(QStringLiteral("desktopLyric.width")).toInt(), 400);
  QCOMPARE(config.get(QStringLiteral("desktopLyric.width")).typeId(),
           static_cast<int>(QMetaType::Int));

  // Valid values are untouched by the strict check.
  QCOMPARE(config.isShowTaskbar(), true);
  QCOMPARE(config.direction(), QStringLiteral("vertical"));
  QCOMPARE(config.get(QStringLiteral("player.playbackRate")).toDouble(), 1.5);

  // The fallback defaults survive a save+load round-trip.
  config.flush();
  DesktopLyricConfig reloaded;
  reloaded.load();
  QCOMPARE(reloaded.isLock(), false);
  QCOMPARE(reloaded.fontSize(), 14);
  QCOMPARE(reloaded.get(QStringLiteral("desktopLyric.style.opacity")).toInt(), 100);
  QCOMPARE(reloaded.get(QStringLiteral("desktopLyric.width")).toInt(), 400);
  QCOMPARE(reloaded.isShowTaskbar(), true);
  QCOMPARE(reloaded.direction(), QStringLiteral("vertical"));
}

void TestConfig::wrongTypedSetLeavesValueUntouched()
{
  // set() is strict too: a wrong-typed value is REJECTED (false), emits no
  // signal and leaves the stored value untouched — no silent toBool()/toInt()
  // coercion.
  DesktopLyricConfig config;
  QSignalSpy changed(&config, &DesktopLyricConfig::settingChanged);
  QSignalSpy anyChanged(&config, &DesktopLyricConfig::anySettingChanged);

  // Baseline: a valid write lands and signals.
  QVERIFY(config.set(QStringLiteral("desktopLyric.isLock"), true));
  QCOMPARE(config.isLock(), true);
  QCOMPARE(changed.count(), 1);
  changed.clear();
  anyChanged.clear();

  QVERIFY(!config.set(QStringLiteral("desktopLyric.isLock"), QStringLiteral("false")));
  QVERIFY(!config.set(QStringLiteral("desktopLyric.style.fontSize"), QStringLiteral("18")));
  QVERIFY(!config.set(QStringLiteral("desktopLyric.style.fontSize"), 18.5));
  QVERIFY(!config.set(QStringLiteral("desktopLyric.style.opacity"), true));
  QVERIFY(!config.set(QStringLiteral("desktopLyric.width"), 400.5));
  QVERIFY(!config.set(QStringLiteral("desktopLyric.direction"), 2));
  QVERIFY(!config.set(QStringLiteral("player.playbackRate"), QStringLiteral("1.5")));

  QCOMPARE(config.isLock(), true);
  QCOMPARE(config.fontSize(), 14);
  QCOMPARE(config.opacity(), 100);
  QCOMPARE(config.get(QStringLiteral("desktopLyric.width")).toInt(), 300);
  QCOMPARE(config.direction(), QStringLiteral("horizontal"));
  QCOMPARE(config.get(QStringLiteral("player.playbackRate")).toDouble(), 1.0);
  QCOMPARE(changed.count(), 0);
  QCOMPARE(anyChanged.count(), 0);

  // Valid coercions still work: a JSON-style integral double for an Int key
  // and an Int for the Double key.
  QVERIFY(config.set(QStringLiteral("desktopLyric.width"), 500.0));
  QCOMPARE(config.get(QStringLiteral("desktopLyric.width")).toInt(), 500);
  QCOMPARE(config.get(QStringLiteral("desktopLyric.width")).typeId(),
           static_cast<int>(QMetaType::Int));
  QVERIFY(config.set(QStringLiteral("player.playbackRate"), 2));
  QCOMPARE(config.get(QStringLiteral("player.playbackRate")).toDouble(), 2.0);
}

void TestConfig::resetRestoresDefault()
{
  DesktopLyricConfig config;

  // The tuned keys each reset from a non-default value back to the new
  // default in loadDefaults(): enable/isAlwaysOnTop/isAlwaysOnTopLoop/
  // isZoomActiveLrc -> true, fullscreenHide -> false, width -> 300,
  // fontSize -> 14, opacity -> 100.
  const QVector<QPair<QString, QVariant>> cases = {
    {QStringLiteral("desktopLyric.enable"), false},
    {QStringLiteral("desktopLyric.isAlwaysOnTop"), false},
    {QStringLiteral("desktopLyric.isAlwaysOnTopLoop"), false},
    {QStringLiteral("desktopLyric.fullscreenHide"), true},
    {QStringLiteral("desktopLyric.width"), 640},
    {QStringLiteral("desktopLyric.style.fontSize"), 60},
    {QStringLiteral("desktopLyric.style.opacity"), 50},
    {QStringLiteral("desktopLyric.style.isZoomActiveLrc"), false},
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

// parseRgbaString/rgbaStringFromColor are the single colour-string format
// shared by the config accessors and the settings dialog's ColorPickerButton.
void TestConfig::parseRgbaStringEdgeCases_data()
{
  QTest::addColumn<QString>("input");
  QTest::addColumn<bool>("valid");
  QTest::addColumn<QColor>("expected");

  // Well-formed lx-music values: alpha is a [0, 1] fraction, not 0-255.
  QTest::newRow("opaque") << QStringLiteral("rgba(7, 197, 86, 1)") << true
                          << QColor(7, 197, 86, 255);
  QTest::newRow("fractional alpha")
    << QStringLiteral("rgba(0, 0, 0, 0.18)") << true << QColor(0, 0, 0, qRound(0.18 * 255.0));
  QTest::newRow("transparent") << QStringLiteral("rgba(1, 2, 3, 0)") << true << QColor(1, 2, 3, 0);
  QTest::newRow("loose whitespace")
    << QStringLiteral("  rgba( 10 ,20,  30 , 0.5 )  ") << true << QColor(10, 20, 30, 128);

  // Out-of-range components clamp instead of wrapping or failing.
  QTest::newRow("channel above 255")
    << QStringLiteral("rgba(300, 256, 999, 1)") << true << QColor(255, 255, 255, 255);
  QTest::newRow("alpha above 1") << QStringLiteral("rgba(10, 20, 30, 2.5)") << true
                                 << QColor(10, 20, 30, 255);
  QTest::newRow("alpha as 0-255") << QStringLiteral("rgba(10, 20, 30, 128)") << true
                                  << QColor(10, 20, 30, 255);

  // Anything else falls back to QColor's own parser: named and hex colours work.
  QTest::newRow("named") << QStringLiteral("red") << true << QColor(255, 0, 0, 255);
  QTest::newRow("hex rgb") << QStringLiteral("#00ff00") << true << QColor(0, 255, 0, 255);
  QTest::newRow("hex argb") << QStringLiteral("#8000ff00") << true << QColor(0, 255, 0, 128);

  // Malformed strings yield an invalid colour, never a silent black.
  QTest::newRow("empty") << QString() << false << QColor();
  QTest::newRow("too few components") << QStringLiteral("rgba(1, 2, 3)") << false << QColor();
  QTest::newRow("negative channel") << QStringLiteral("rgba(-1, 2, 3, 1)") << false << QColor();
  QTest::newRow("non-numeric") << QStringLiteral("rgba(a, b, c, d)") << false << QColor();
  QTest::newRow("trailing garbage") << QStringLiteral("rgba(1, 2, 3, 1) x") << false << QColor();
  QTest::newRow("unknown name") << QStringLiteral("notacolour") << false << QColor();
}

void TestConfig::parseRgbaStringEdgeCases()
{
  QFETCH(QString, input);
  QFETCH(bool, valid);
  QFETCH(QColor, expected);

  const QColor actual = DesktopLyricConfig::parseRgbaString(input);
  QCOMPARE(actual.isValid(), valid);
  if (valid)
    QCOMPARE(actual.rgba(), expected.rgba());
}

void TestConfig::rgbaStringFromColorRoundTrips()
{
  // The writer emits exactly the reference format the defaults use.
  QCOMPARE(DesktopLyricConfig::rgbaStringFromColor(QColor(7, 197, 86, 255)),
           QStringLiteral("rgba(7, 197, 86, 1)"));
  QCOMPARE(DesktopLyricConfig::rgbaStringFromColor(QColor(1, 2, 3, 0)),
           QStringLiteral("rgba(1, 2, 3, 0)"));
  QCOMPARE(DesktopLyricConfig::rgbaStringFromColor(QColor(0, 0, 0, qRound(0.18 * 255.0))),
           QStringLiteral("rgba(0, 0, 0, 0.18)"));

  // Every default colour string (see firstRunSeedsDefaults) survives parse -> format unchanged, so a
  // picker that re-saves an untouched colour writes no drift into the config.
  for (const QString& stored :
       {QStringLiteral("rgba(255, 255, 255, 1)"), QStringLiteral("rgba(7, 197, 86, 1)"),
        QStringLiteral("rgba(0, 0, 0, 0.18)")}) {
    QCOMPARE(DesktopLyricConfig::rgbaStringFromColor(DesktopLyricConfig::parseRgbaString(stored)),
             stored);
  }

  // Any 8-bit alpha survives format -> parse within one step (two decimals).
  for (int alpha = 0; alpha <= 255; ++alpha) {
    const QColor color(12, 34, 56, alpha);
    const QColor back =
      DesktopLyricConfig::parseRgbaString(DesktopLyricConfig::rgbaStringFromColor(color));
    QVERIFY(back.isValid());
    QCOMPARE(back.rgb(), color.rgb());
    QVERIFY2(qAbs(back.alpha() - alpha) <= 2, qPrintable(QString::number(alpha)));
  }
}

void TestConfig::translationLocaleMapping_data()
{
  QTest::addColumn<QString>("locale");
  QTest::addColumn<QString>("expected");

  QTest::newRow("zh-CN") << QStringLiteral("zh_CN") << QStringLiteral("zh-cn");
  QTest::newRow("zh-SG") << QStringLiteral("zh_SG") << QStringLiteral("zh-cn");
  QTest::newRow("zh-TW") << QStringLiteral("zh_TW") << QStringLiteral("zh-tw");
  QTest::newRow("zh-HK") << QStringLiteral("zh_HK") << QStringLiteral("zh-tw");
  QTest::newRow("zh-MO") << QStringLiteral("zh_MO") << QStringLiteral("zh-tw");
  QTest::newRow("en-US") << QStringLiteral("en_US") << QStringLiteral("en-us");
  QTest::newRow("en-GB") << QStringLiteral("en_GB") << QStringLiteral("en-us");
  QTest::newRow("ja-JP") << QStringLiteral("ja_JP") << QStringLiteral("en-us");
  QTest::newRow("C") << QStringLiteral("C") << QStringLiteral("en-us");
}

void TestConfig::translationLocaleMapping()
{
  // The null-langId auto-detect: only Chinese locales pick a Chinese pack,
  // split by territory; everything else (including the C locale) is en-us.
  QFETCH(QString, locale);
  QFETCH(QString, expected);
  QCOMPARE(TranslationManager::languageForLocale(QLocale(locale)), expected);
}

void TestConfig::translationUnsupportedLangIdFallsBackToEnglish()
{
  // A hand-edited, unsupported common.langId resolves to en-us with one
  // warning; lookups then use the en-us pack and unknown keys echo the key.
  DesktopLyricConfig config;
  config.loadDefaults();
  QVERIFY(config.set(QStringLiteral("common.langId"), QStringLiteral("fr-fr")));

  QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("unsupported language id")));
  const TranslationManager i18n(config);
  QCOMPARE(i18n.currentLanguage(), QStringLiteral("en-us"));
  QCOMPARE(i18n.tr(QStringLiteral("setting__desktop_lyric_lock")),
           QStringLiteral("Lock lyric window"));
  QCOMPARE(i18n.tr(QStringLiteral("no_such_key")), QStringLiteral("no_such_key"));
  QCOMPARE(i18n.tr(QString()), QString());
}

void TestConfig::translationLiveSwitchEmitsLanguageChanged()
{
  // A common.langId change swaps the active pack live and emits
  // languageChanged once per switch; other keys never reload the pack.
  DesktopLyricConfig config;
  config.loadDefaults();
  QVERIFY(config.set(QStringLiteral("common.langId"), QStringLiteral("en-us")));
  TranslationManager i18n(config);
  QSignalSpy spy(&i18n, &TranslationManager::languageChanged);
  QCOMPARE(i18n.tr(QStringLiteral("setting__desktop_lyric_lock")),
           QStringLiteral("Lock lyric window"));

  QVERIFY(config.set(QStringLiteral("common.langId"), QStringLiteral("zh-cn")));
  QCOMPARE(spy.count(), 1);
  QCOMPARE(i18n.currentLanguage(), QStringLiteral("zh-cn"));
  QCOMPARE(i18n.tr(QStringLiteral("setting__desktop_lyric_lock")), QStringLiteral("锁定歌词"));

  QVERIFY(config.set(QStringLiteral("common.langId"), QStringLiteral("zh-tw")));
  QCOMPARE(spy.count(), 2);
  QCOMPARE(i18n.currentLanguage(), QStringLiteral("zh-tw"));
  QCOMPARE(i18n.tr(QStringLiteral("setting__desktop_lyric_lock")), QStringLiteral("鎖定歌詞視窗"));

  QVERIFY(config.set(QStringLiteral("desktopLyric.isLock"), true));
  QCOMPARE(spy.count(), 2); // Unrelated key: no reload.

  // Back to null: the system-locale auto-detect applies (whatever it is).
  QVERIFY(config.set(QStringLiteral("common.langId"), QVariant()));
  QCOMPARE(spy.count(), 3);
  QCOMPARE(i18n.currentLanguage(), TranslationManager::languageForLocale(QLocale::system()));
}

void TestConfig::translationPacksShareKeySet()
{
  // Every key must exist in all three packs (a key missing from one pack
  // silently shows English there), and no value may be empty.
  QSet<QString> reference;
  for (const QString& langId :
       {QStringLiteral("en-us"), QStringLiteral("zh-cn"), QStringLiteral("zh-tw")}) {
    QFile file(QStringLiteral(":/translations/%1.json").arg(langId));
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(langId));
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    QVERIFY2(error.error == QJsonParseError::NoError && doc.isObject(), qPrintable(langId));
    const QJsonObject pack = doc.object();
    QSet<QString> keys;
    for (auto it = pack.constBegin(); it != pack.constEnd(); ++it) {
      QVERIFY2(it.value().isString() && !it.value().toString().isEmpty(),
               qPrintable(langId + QStringLiteral(": ") + it.key()));
      keys.insert(it.key());
    }
    if (reference.isEmpty()) {
      reference = keys;
    } else {
      // Sorted lists, so a failure prints the differing keys.
      QStringList actual(keys.cbegin(), keys.cend());
      QStringList expected(reference.cbegin(), reference.cend());
      actual.sort();
      expected.sort();
      QCOMPARE(actual, expected);
    }
  }
  QVERIFY(!reference.isEmpty());
}

QTEST_MAIN(TestConfig)

#include "tst_config.moc"
