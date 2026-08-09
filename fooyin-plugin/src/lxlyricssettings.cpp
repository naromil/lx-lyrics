/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#include "lxlyricssettings.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>

#include <utility>

LxLyricsSettingsPageWidget::LxLyricsSettingsPageWidget(Fooyin::SettingsManager* settings,
                                                       QWidget* parent)
  : Fooyin::SettingsPageWidget()
  , m_settings(settings)
  , m_appPathEdit(new QLineEdit(this))
  , m_autoSpawnCheck(new QCheckBox(tr("Start desktop lyrics when fooyin starts"), this))
  , m_openSettingsButton(new QPushButton(tr("Open lyrics settings"), this))
{
  if (parent != nullptr) {
    setParent(parent);
  }

  m_appPathEdit->setPlaceholderText(tr("Auto-detect (lyrics-app in PATH)"));

  auto* layout = new QFormLayout(this);
  layout->addRow(tr("Lyrics app path:"), m_appPathEdit);
  layout->addRow(m_autoSpawnCheck);
  layout->addRow(m_openSettingsButton);

  connect(m_openSettingsButton, &QPushButton::clicked, this, [this] {
    // Guard null: the callback is only set while the plugin is alive; when
    // the app is not running the plugin's callback is itself a no-op.
    if (m_openSettingsCallback) {
      m_openSettingsCallback();
    }
  });
}

void LxLyricsSettingsPageWidget::setOpenSettingsCallback(std::function<void()> cb)
{
  m_openSettingsCallback = std::move(cb);
}

void LxLyricsSettingsPageWidget::load()
{
  m_appPathEdit->setText(m_settings->value(LxLyrics::appPathKey).toString());
  m_autoSpawnCheck->setChecked(m_settings->value(LxLyrics::autoSpawnKey).toBool());
}

void LxLyricsSettingsPageWidget::apply()
{
  m_settings->set(LxLyrics::appPathKey, m_appPathEdit->text().trimmed());
  m_settings->set(LxLyrics::autoSpawnKey, m_autoSpawnCheck->isChecked());
}

void LxLyricsSettingsPageWidget::reset()
{
  m_appPathEdit->clear();
  m_autoSpawnCheck->setChecked(false);
}

LxLyricsSettingsPage::LxLyricsSettingsPage(Fooyin::SettingsManager* settings, QObject* parent)
  : Fooyin::SettingsPage(settings->settingsDialog(), parent)
{
  setId(Fooyin::Id(QStringLiteral("Fooyin.Page.LxLyrics")));
  setName(tr("LX Lyrics"));
  setCategory({QStringLiteral("LX Lyrics")});
  // The dialog creates the widget lazily (WidgetCreator runs only when the
  // page is opened), so the open-settings callback is forwarded at creation
  // time instead of fetched via SettingsPage::widget().
  setWidgetCreator([settings, this] {
    auto* widget = new LxLyricsSettingsPageWidget(settings);
    widget->setOpenSettingsCallback(m_openSettingsCallback);
    return widget;
  });
}

void LxLyricsSettingsPage::setOpenSettingsCallback(std::function<void()> cb)
{
  m_openSettingsCallback = std::move(cb);
}
