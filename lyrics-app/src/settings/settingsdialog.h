/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#pragma once

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QScrollArea;
class QSpinBox;
class QVBoxLayout;
class ColorPickerButton;
class DesktopLyricConfig;
class TranslationManager;

// Modeless settings dialog for the desktop lyric overlay.
//
// A faithful port of the reference SettingDesktopLyric.vue control set:
// DesktopLyricConfig is the single source of truth, so the dialog never keeps
// its own copy of values — it re-reads the config whenever it is shown and
// writes through config.set() on every control change. LyricWindow already
// observes settingChanged and applies changes live.
//
// Labels are looked up from the TranslationManager when the widget tree is
// built; the tree is rebuilt wholesale on a language change.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(DesktopLyricConfig& config, TranslationManager& i18n, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    struct CheckboxBinding {
        QCheckBox* box;
        QString key;
    };
    struct RadioBinding {
        QRadioButton* radio;
        QString key;
        QString value;
    };

    QCheckBox* addCheckbox(QWidget* parent, const QString& label, const QString& key);
    QRadioButton* addRadio(QWidget* parent, const QString& label, const QString& key, const QString& value);

    void buildUi();
    void updateLineGapLabel();

    QWidget* buildGeneralGroup(QWidget* parent);
    QWidget* buildFontWeightGroup(QWidget* parent);
    QWidget* buildDirectionGroup(QWidget* parent);
    QWidget* buildScrollAlignGroup(QWidget* parent);
    QWidget* buildAlignGroup(QWidget* parent);
    QWidget* buildStyleGroup(QWidget* parent);
    QWidget* buildColorGroup(QWidget* parent);
    QWidget* buildResetGroup(QWidget* parent);

    void populateFromConfig();

    DesktopLyricConfig& m_config;
    TranslationManager& m_i18n;

    QVBoxLayout* m_mainLayout = nullptr;
    QScrollArea* m_scrollArea = nullptr;
    QVector<CheckboxBinding> m_checkboxBindings;
    QVector<RadioBinding> m_radioBindings;

    QLabel* m_lineGapLabel = nullptr;
    QSpinBox* m_lineGapSpin = nullptr;
    QSpinBox* m_fontSizeSpin = nullptr;
    QSpinBox* m_opacitySpin = nullptr;
    QComboBox* m_fontCombo = nullptr;
    ColorPickerButton* m_unplayButton = nullptr;
    ColorPickerButton* m_playedButton = nullptr;
    ColorPickerButton* m_shadowButton = nullptr;
    QPushButton* m_resetWindowButton = nullptr;
};
