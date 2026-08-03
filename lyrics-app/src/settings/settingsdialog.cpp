/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "settings/settingsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <utility>

#include "config/desktoplyricconfig.h"
#include "i18n/translationmanager.h"
#include "settings/colorpickerbutton.h"

namespace {

// --- Reference constants (ported verbatim) ----------------------------------
//
// Preset swatches and reset defaults come straight from
// references/src/renderer/views/Setting/components/SettingDesktopLyric.vue.

const QStringList kUnplayPresets = {
    QStringLiteral("rgba(255, 255, 255, 1)"),
    QStringLiteral("rgba(255, 236, 144, 1)"),
    QStringLiteral("rgba(144, 255, 206, 1)"),
    QStringLiteral("rgba(32, 255, 132, 1)"),
    QStringLiteral("rgba(255, 226, 32, 1)"),
    QStringLiteral("rgba(57, 203, 255, 1)"),
    QStringLiteral("rgba(217, 57, 255, 1)"),
    QStringLiteral("rgba(255, 57, 71, 1)"),
};

const QStringList kPlayedPresets = {
    QStringLiteral("rgba(255, 236, 144, 1)"),
    QStringLiteral("rgba(144, 255, 206, 1)"),
    QStringLiteral("rgba(32, 255, 132, 1)"),
    QStringLiteral("rgba(255, 226, 32, 1)"),
    QStringLiteral("rgba(57, 203, 255, 1)"),
    QStringLiteral("rgba(7, 197, 86, 1)"),
    QStringLiteral("rgba(25, 181, 254, 1)"),
    QStringLiteral("rgba(217, 57, 255, 1)"),
    QStringLiteral("rgba(255, 57, 71, 1)"),
};

const QStringList kShadowPresets = {
    QStringLiteral("rgba(0, 0, 0, 0.15)"),
};

const QString kDefaultUnplayColor = QStringLiteral("rgba(255, 255, 255, 1)");
const QString kDefaultPlayedColor = QStringLiteral("rgba(7, 197, 86, 1)");
const QString kDefaultShadowColor = QStringLiteral("rgba(0, 0, 0, 0.18)");

// --- General checkbox list (reference order; hover-hide skipped on Linux) ---
//
// Each option pairs a desktopLyric.* config key with the reference translation
// key (verbatim in the language packs), so labels follow the current language.

struct CheckboxSpec {
    const char* labelKey;   // reference i18n key, e.g. "setting__desktop_lyric_lock"
    const char* configKey;  // desktopLyric.* setting key
};

QVector<CheckboxSpec> generalCheckboxSpecs()
{
    QVector<CheckboxSpec> specs = {
        { "setting__desktop_lyric_enable", "desktopLyric.enable" },
        { "setting__desktop_lyric_lock", "desktopLyric.isLock" },
        { "setting__desktop_lyric_fullscreen_hide", "desktopLyric.fullscreenHide" },
        { "setting__desktop_lyric_pause_hide", "desktopLyric.pauseHide" },
        { "setting__desktop_lyric_audio_visualization", "desktopLyric.audioVisualization" },
        { "setting__desktop_lyric_delay_scroll", "desktopLyric.isDelayScroll" },
        { "setting__desktop_lyric_always_on_top", "desktopLyric.isAlwaysOnTop" },
        { "setting__desktop_lyric_show_taskbar", "desktopLyric.isShowTaskbar" },
        { "setting__desktop_lyric_always_on_top_loop", "desktopLyric.isAlwaysOnTopLoop" },
        { "setting__desktop_lyric_lock_screen", "desktopLyric.isLockScreen" },
        { "setting__desktop_lyric_ellipsis", "desktopLyric.style.ellipsis" },
        // The reference labels the zoom option with the control-bar tooltip key.
        { "desktop_lyric__lrc_active_zoom_on", "desktopLyric.style.isZoomActiveLrc" },
    };
    // QOperatingSystemVersion dropped AnyLinux in Qt 6.11, so detect Linux the
    // same compile-time way DesktopLyricConfig detects its platforms.
#if !defined(Q_OS_LINUX)
    specs.append(CheckboxSpec{
        "setting__desktop_lyric_hover_hide",
        "desktopLyric.isHoverHide",
    });
#endif
    return specs;
}

struct RadioSpec {
    const char* label;
    const char* key;
    const char* value;
};

// Column of one color label above its picker button, mirroring the reference
// color chip + label layout.
QWidget* makeColorColumn(QWidget* parent, const QString& label, ColorPickerButton* button)
{
    auto* column = new QWidget(parent);
    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* labelWidget = new QLabel(label, column);
    labelWidget->setAlignment(Qt::AlignHCenter);
    layout->addWidget(labelWidget);
    layout->addWidget(button, 0, Qt::AlignHCenter);
    return column;
}

} // namespace

SettingsDialog::SettingsDialog(DesktopLyricConfig& config, TranslationManager& i18n, QWidget* parent)
    : QDialog(parent)
    , m_config(config)
    , m_i18n(i18n)
{
    setWindowTitle(QStringLiteral("Desktop Lyric Settings"));

    m_mainLayout = new QVBoxLayout(this);
    buildUi();

    // Rebuild the widget tree when the language changes so every label is
    // re-looked-up from the new pack.
    connect(&m_i18n, &TranslationManager::languageChanged, this, &SettingsDialog::buildUi);

    resize(550, 600);
}

void SettingsDialog::buildUi()
{
    // Deleting the scroll area also destroys every child widget and removes it
    // from the layout; the member pointers are reset so the build methods can
    // recreate fresh widgets with current translations.
    delete m_scrollArea;
    m_scrollArea = nullptr;

    m_checkboxBindings.clear();
    m_radioBindings.clear();
    m_lineGapLabel = nullptr;
    m_lineGapSpin = nullptr;
    m_fontSizeSpin = nullptr;
    m_opacitySpin = nullptr;
    m_fontCombo = nullptr;
    m_unplayButton = nullptr;
    m_playedButton = nullptr;
    m_shadowButton = nullptr;
    m_resetWindowButton = nullptr;

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);

    auto* container = new QWidget;
    auto* layout = new QVBoxLayout(container);
    layout->addWidget(buildGeneralGroup(container));
    layout->addWidget(buildFontWeightGroup(container));
    layout->addWidget(buildDirectionGroup(container));
    layout->addWidget(buildScrollAlignGroup(container));
    layout->addWidget(buildAlignGroup(container));
    layout->addWidget(buildStyleGroup(container));
    layout->addWidget(buildColorGroup(container));
    layout->addWidget(buildResetGroup(container));
    layout->addStretch();

    scroll->setWidget(container);
    m_mainLayout->addWidget(scroll);
    m_scrollArea = scroll;

    populateFromConfig();
}

void SettingsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    populateFromConfig(); // Re-read the config: values may have changed since last open.
}

QCheckBox* SettingsDialog::addCheckbox(QWidget* parent, const QString& label, const QString& key)
{
    auto* box = new QCheckBox(label, parent);
    connect(box, &QCheckBox::toggled, this, [this, key](bool on) { m_config.set(key, on); });
    m_checkboxBindings.append({ box, key });
    return box;
}

QRadioButton* SettingsDialog::addRadio(QWidget* parent, const QString& label, const QString& key, const QString& value)
{
    auto* radio = new QRadioButton(label, parent);
    connect(radio, &QRadioButton::toggled, this, [this, key, value](bool on) {
        if (on)
            m_config.set(key, value);
    });
    m_radioBindings.append({ radio, key, value });
    return radio;
}

QWidget* SettingsDialog::buildGeneralGroup(QWidget* parent)
{
    auto* group = new QGroupBox(m_i18n.tr(QStringLiteral("setting__desktop_lyric")), parent);
    auto* layout = new QVBoxLayout(group);
    for (const CheckboxSpec& spec : generalCheckboxSpecs()) {
        layout->addWidget(addCheckbox(group,
                                      m_i18n.tr(QString::fromUtf8(spec.labelKey)),
                                      QString::fromUtf8(spec.configKey)));
    }
    return group;
}

QWidget* SettingsDialog::buildFontWeightGroup(QWidget* parent)
{
    auto* group = new QGroupBox(m_i18n.tr(QStringLiteral("setting__desktop_lyric_font_weight")), parent);
    auto* layout = new QHBoxLayout(group);
    layout->addWidget(addCheckbox(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_font_weight_font")), QStringLiteral("desktopLyric.style.isFontWeightFont")));
    layout->addWidget(addCheckbox(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_font_weight_line")), QStringLiteral("desktopLyric.style.isFontWeightLine")));
    layout->addWidget(addCheckbox(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_font_weight_extended")), QStringLiteral("desktopLyric.style.isFontWeightExtended")));
    layout->addStretch();
    return group;
}

QWidget* SettingsDialog::buildDirectionGroup(QWidget* parent)
{
    auto* group = new QGroupBox(m_i18n.tr(QStringLiteral("setting__desktop_lyric_direction")), parent);
    auto* layout = new QHBoxLayout(group);
    layout->addWidget(addRadio(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_direction_horizontal")), QStringLiteral("desktopLyric.direction"), QStringLiteral("horizontal")));
    layout->addWidget(addRadio(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_direction_vertical")), QStringLiteral("desktopLyric.direction"), QStringLiteral("vertical")));
    layout->addStretch();
    return group;
}

QWidget* SettingsDialog::buildScrollAlignGroup(QWidget* parent)
{
    auto* group = new QGroupBox(m_i18n.tr(QStringLiteral("setting__desktop_lyric_scroll_align")), parent);
    auto* layout = new QHBoxLayout(group);
    layout->addWidget(addRadio(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_scroll_align_top")), QStringLiteral("desktopLyric.scrollAlign"), QStringLiteral("top")));
    layout->addWidget(addRadio(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_scroll_align_center")), QStringLiteral("desktopLyric.scrollAlign"), QStringLiteral("center")));
    layout->addStretch();
    return group;
}

QWidget* SettingsDialog::buildAlignGroup(QWidget* parent)
{
    auto* group = new QGroupBox(m_i18n.tr(QStringLiteral("setting__desktop_lyric_align")), parent);
    auto* layout = new QHBoxLayout(group);
    layout->addWidget(addRadio(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_align_left")), QStringLiteral("desktopLyric.style.align"), QStringLiteral("left")));
    layout->addWidget(addRadio(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_align_center")), QStringLiteral("desktopLyric.style.align"), QStringLiteral("center")));
    layout->addWidget(addRadio(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_align_right")), QStringLiteral("desktopLyric.style.align"), QStringLiteral("right")));
    layout->addStretch();
    return group;
}

QWidget* SettingsDialog::buildStyleGroup(QWidget* parent)
{
    // "Lyric Style" has no reference key in the language packs — kept as-is.
    auto* group = new QGroupBox(QStringLiteral("Lyric Style"), parent);
    auto* form = new QFormLayout(group);

    m_lineGapSpin = new QSpinBox(group);
    m_lineGapSpin->setRange(0, 25);
    m_lineGapSpin->setSuffix(QStringLiteral(" px"));

    auto* resetSpacingButton = new QPushButton(m_i18n.tr(QStringLiteral("setting__desktop_lyric_reset")), group);
    // The tooltip has no reference key in the language packs — kept as-is.
    resetSpacingButton->setToolTip(QStringLiteral("Restore the default spacing (15 px)"));
    connect(resetSpacingButton, &QPushButton::clicked, this, [this] { m_lineGapSpin->setValue(15); });

    auto* spacingRow = new QWidget(group);
    auto* spacingLayout = new QHBoxLayout(spacingRow);
    spacingLayout->setContentsMargins(0, 0, 0, 0);
    spacingLayout->addWidget(m_lineGapSpin);
    spacingLayout->addWidget(resetSpacingButton);
    spacingLayout->addStretch();

    // The reference shows the live spacing value in the label
    // ("Lyric Spacing (15)"); the pack stores it as a {num} placeholder.
    m_lineGapLabel = new QLabel(group);
    updateLineGapLabel();
    connect(m_lineGapSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        m_config.set(QStringLiteral("desktopLyric.style.lineGap"), value);
        updateLineGapLabel();
    });
    form->addRow(m_lineGapLabel, spacingRow);

    m_fontSizeSpin = new QSpinBox(group);
    m_fontSizeSpin->setRange(10, 80);
    m_fontSizeSpin->setSuffix(QStringLiteral(" px"));
    connect(m_fontSizeSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        m_config.set(QStringLiteral("desktopLyric.style.fontSize"), value);
    });
    form->addRow(m_i18n.tr(QStringLiteral("setting__basic_font_size")), m_fontSizeSpin);

    m_opacitySpin = new QSpinBox(group);
    m_opacitySpin->setRange(6, 100);
    m_opacitySpin->setSuffix(QStringLiteral("%"));
    connect(m_opacitySpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        m_config.set(QStringLiteral("desktopLyric.style.opacity"), value);
    });
    // "Opacity" has no reference key in the language packs — kept as-is.
    form->addRow(QStringLiteral("Opacity"), m_opacitySpin);

    m_fontCombo = new QComboBox(group);
    m_fontCombo->addItem(m_i18n.tr(QStringLiteral("setting__desktop_lyric_font_default")), QString()); // Empty family = default font.
    QStringList families = QFontDatabase::families();
    families.removeDuplicates();
    for (const QString& family : std::as_const(families))
        m_fontCombo->addItem(family, family);
    connect(m_fontCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_config.set(QStringLiteral("desktopLyric.style.font"), m_fontCombo->itemData(index).toString());
    });
    form->addRow(m_i18n.tr(QStringLiteral("setting__basic_font")), m_fontCombo);

    return group;
}

void SettingsDialog::updateLineGapLabel()
{
    if (!m_lineGapLabel)
        return; // Widget tree not built yet.
    // The pack stores the spacing label with a named {num} placeholder; Qt's
    // QString::arg only fills %N markers, so substitute the placeholder with
    // the live config value directly.
    m_lineGapLabel->setText(m_i18n.tr(QStringLiteral("setting__desktop_lyric_line_gap"))
                                .replace(QStringLiteral("{num}"),
                                         QString::number(m_config.get(QStringLiteral("desktopLyric.style.lineGap")).toInt())));
}

QWidget* SettingsDialog::buildColorGroup(QWidget* parent)
{
    auto* group = new QGroupBox(m_i18n.tr(QStringLiteral("setting__desktop_lyric_color")), parent);
    auto* layout = new QHBoxLayout(group);

    m_unplayButton = new ColorPickerButton(QStringLiteral("desktopLyric.style.lyricUnplayColor"),
                                           m_config, m_i18n, kUnplayPresets, kDefaultUnplayColor, group);
    layout->addWidget(makeColorColumn(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_unplay_color")), m_unplayButton));

    m_playedButton = new ColorPickerButton(QStringLiteral("desktopLyric.style.lyricPlayedColor"),
                                           m_config, m_i18n, kPlayedPresets, kDefaultPlayedColor, group);
    layout->addWidget(makeColorColumn(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_played_color")), m_playedButton));

    m_shadowButton = new ColorPickerButton(QStringLiteral("desktopLyric.style.lyricShadowColor"),
                                           m_config, m_i18n, kShadowPresets, kDefaultShadowColor, group);
    layout->addWidget(makeColorColumn(group, m_i18n.tr(QStringLiteral("setting__desktop_lyric_shadow_color")), m_shadowButton));

    layout->addStretch();
    return group;
}

QWidget* SettingsDialog::buildResetGroup(QWidget* parent)
{
    auto* group = new QGroupBox(m_i18n.tr(QStringLiteral("setting__desktop_lyric_reset")), parent);
    auto* layout = new QVBoxLayout(group);

    m_resetWindowButton = new QPushButton(m_i18n.tr(QStringLiteral("setting__desktop_lyric_reset_window")), group);
    connect(m_resetWindowButton, &QPushButton::clicked, this, [this] {
        // Width/height mirror the config defaults in loadDefaults() (300/300).
        m_config.set(QStringLiteral("desktopLyric.width"), 300);
        m_config.set(QStringLiteral("desktopLyric.height"), 300);
        m_config.set(QStringLiteral("desktopLyric.x"), QVariant());
        m_config.set(QStringLiteral("desktopLyric.y"), QVariant());
    });
    layout->addWidget(m_resetWindowButton);

    return group;
}

void SettingsDialog::populateFromConfig()
{
    for (const CheckboxBinding& binding : m_checkboxBindings) {
        const QSignalBlocker blocker(binding.box);
        binding.box->setChecked(m_config.get(binding.key).toBool());
    }
    for (const RadioBinding& binding : m_radioBindings) {
        const QSignalBlocker blocker(binding.radio);
        binding.radio->setChecked(m_config.get(binding.key).toString() == binding.value);
    }
    {
        const QSignalBlocker blocker(m_lineGapSpin);
        m_lineGapSpin->setValue(m_config.get(QStringLiteral("desktopLyric.style.lineGap")).toInt());
    }
    {
        const QSignalBlocker blocker(m_fontSizeSpin);
        m_fontSizeSpin->setValue(m_config.get(QStringLiteral("desktopLyric.style.fontSize")).toInt());
    }
    {
        const QSignalBlocker blocker(m_opacitySpin);
        m_opacitySpin->setValue(m_config.get(QStringLiteral("desktopLyric.style.opacity")).toInt());
    }
    {
        const QSignalBlocker blocker(m_fontCombo);
        const int index = m_fontCombo->findData(m_config.get(QStringLiteral("desktopLyric.style.font")).toString());
        m_fontCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    m_unplayButton->refreshFromConfig();
    m_playedButton->refreshFromConfig();
    m_shadowButton->refreshFromConfig();
    updateLineGapLabel();
}
