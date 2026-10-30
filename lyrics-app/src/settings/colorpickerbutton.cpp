/*
 * SPDX-License-Identifier: Apache-2.0
 * Portions derived from lx-music-desktop (https://github.com/lyswhut/lx-music-desktop),
 * Copyright (c) lyswhut, licensed under Apache-2.0.
 * Copyright (c) 2026 LX Lyrics contributors.
 */
#include "settings/colorpickerbutton.h"

#include <QAbstractButton>
#include <QColorDialog>
#include <QDialog>
#include <QGridLayout>
#include <QPainter>
#include <QVBoxLayout>
#include <utility>

#include "config/desktoplyricconfig.h"
#include "i18n/translationmanager.h"

namespace {

// --- Small preset color chip ------------------------------------------------

// Clickable square swatch inside the picker dialog's preset grid.
class ColorChip : public QAbstractButton {
public:
  explicit ColorChip(const QColor& color, QWidget* parent = nullptr)
    : QAbstractButton(parent)
    , m_color(color)
  {
    setCursor(Qt::PointingHandCursor);
    setFixedSize(28, 28);
    setToolTip(color.name(QColor::HexArgb));
  }

protected:
  void paintEvent(QPaintEvent*) override
  {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF chip = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    const QColor border =
      (underMouse() || isDown()) ? palette().color(QPalette::Highlight) : QColor(140, 140, 140);
    painter.setPen(QPen(border, 1.0));
    painter.setBrush(m_color);
    painter.drawRoundedRect(chip, 5, 5);
  }

private:
  QColor m_color;
};

// --- Preset picker dialog ---------------------------------------------------

// Modal dialog with the reference preset swatches, a per-color reset, and the
// Qt color dialog as the "More colors..." fallback. Accepts with a color; the
// caller writes it to the config. Labels are looked up from the
// TranslationManager at construction, so each open uses the current language.
class ColorPickerDialog : public QDialog {
public:
  ColorPickerDialog(const QColor& current, const QList<QColor>& presets, const QColor& defaultColor,
                    TranslationManager& i18n, QWidget* parent = nullptr)
    : QDialog(parent)
    , m_selected(current)
    , m_i18n(i18n)
  {
    setWindowTitle(m_i18n.tr(QStringLiteral("theme_edit_modal__pick_color")));
    setModal(true);

    auto* layout = new QVBoxLayout(this);

    auto* grid = new QGridLayout;
    constexpr int kColumns = 4;
    for (int i = 0; i < presets.size(); ++i) {
      auto* chip = new ColorChip(presets.at(i), this);
      grid->addWidget(chip, i / kColumns, i % kColumns);
      connect(chip, &QAbstractButton::clicked, this, [this, color = presets.at(i)] {
        m_selected = color;
        accept();
      });
    }
    layout->addLayout(grid);

    // No reference key for this label — kept as-is.
    auto* resetButton = new QPushButton(QStringLiteral("Reset to default"), this);
    connect(resetButton, &QPushButton::clicked, this, [this, defaultColor] {
      m_selected = defaultColor;
      accept();
    });
    layout->addWidget(resetButton);

    // No reference key for this label — kept as-is.
    auto* moreButton = new QPushButton(QStringLiteral("More colors..."), this);
    connect(moreButton, &QPushButton::clicked, this, [this, current] {
      const QColor picked = QColorDialog::getColor(current, this, QStringLiteral("Select color"),
                                                   QColorDialog::ShowAlphaChannel);
      if (picked.isValid()) {
        m_selected = picked;
        accept();
      }
    });
    layout->addWidget(moreButton);

    auto* cancelButton = new QPushButton(m_i18n.tr(QStringLiteral("btn_cancel")), this);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(cancelButton);
  }

  QColor selectedColor() const { return m_selected; }

private:
  QColor m_selected;
  TranslationManager& m_i18n;
};

} // namespace

ColorPickerButton::ColorPickerButton(QString configKey, DesktopLyricConfig& config,
                                     TranslationManager& i18n, const QStringList& presetRgbaStrings,
                                     const QString& defaultRgba, QWidget* parent)
  : QPushButton(parent)
  , m_configKey(std::move(configKey))
  , m_config(config)
  , m_i18n(i18n)
  , m_defaultColor(DesktopLyricConfig::parseRgbaString(defaultRgba))
{
  setFixedSize(72, 28);
  setCursor(Qt::PointingHandCursor);

  m_presets.reserve(presetRgbaStrings.size());
  for (const QString& rgba : presetRgbaStrings)
    m_presets.append(DesktopLyricConfig::parseRgbaString(rgba));

  connect(this, &QPushButton::clicked, this, &ColorPickerButton::openPicker);
  connect(&m_config, &DesktopLyricConfig::settingChanged, this,
          [this](const QString& key, const QVariant&) {
            if (key == m_configKey)
              refreshFromConfig();
          });

  refreshFromConfig();
}

void ColorPickerButton::refreshFromConfig()
{
  m_color = DesktopLyricConfig::parseRgbaString(m_config.get(m_configKey).toString());
  setToolTip(m_config.get(m_configKey).toString());
  update();
}

void ColorPickerButton::openPicker()
{
  ColorPickerDialog dialog(m_color, m_presets, m_defaultColor, m_i18n, this);
  if (dialog.exec() == QDialog::Accepted)
    applyColor(dialog.selectedColor());
}

void ColorPickerButton::applyColor(const QColor& color)
{
  m_config.set(m_configKey, DesktopLyricConfig::rgbaStringFromColor(color));
}

void ColorPickerButton::paintEvent(QPaintEvent*)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF swatch = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
  const QColor border = (isDown() || hasFocus()) ? palette().color(QPalette::Highlight)
                                                 : palette().color(QPalette::Mid);
  painter.setPen(QPen(border, 1.0));
  painter.setBrush(m_color);
  painter.drawRoundedRect(swatch, 5, 5);
}
