/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 LX Lyrics contributors.
 * This plugin drives the standalone lx-lyrics display; lyrics rendering logic
 * is ported from lx-music-desktop (Apache-2.0) and lives in the lyrics-app project.
 */

#include "lxlyricssettings.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include <utility>

LxLyricsSettingsDialog::LxLyricsSettingsDialog(Fooyin::SettingsManager* settings,
                                               std::function<void()> openSettings, QWidget* parent)
  : QDialog(parent)
  , m_settings(settings)
  , m_appPathEdit(new QLineEdit(this))
  , m_rememberStateCheck(
      new QCheckBox(tr("Remember the desktop lyrics state from the last session"), this))
  , m_openSettingsButton(new QPushButton(tr("Open lyrics settings"), this))
{
  setWindowTitle(tr("LX Lyrics"));

  m_appPathEdit->setPlaceholderText(tr("Auto-detect (lyrics-app in PATH)"));

  auto* buttons = new QDialogButtonBox(
    QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &LxLyricsSettingsDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &LxLyricsSettingsDialog::reject);
  // The settings dialog's per-page reset, kept: back to the untouched
  // defaults, written only if the dialog is then accepted.
  connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QAbstractButton::clicked, this,
          [this] {
            m_appPathEdit->clear();
            m_rememberStateCheck->setChecked(true); // Default: remember the state.
          });
  connect(m_openSettingsButton, &QPushButton::clicked, this,
          [openSettings = std::move(openSettings)] {
            // Guard null: the app is not running when the callback is a no-op.
            if (openSettings) {
              openSettings();
            }
          });

  auto* layout = new QGridLayout(this);
  layout->setSizeConstraint(QLayout::SetFixedSize);

  int row{0};
  layout->addWidget(new QLabel(tr("Lyrics app path:"), this), row, 0);
  layout->addWidget(m_appPathEdit, row++, 1);
  layout->addWidget(m_rememberStateCheck, row++, 0, 1, 2);
  layout->addWidget(m_openSettingsButton, row++, 0, 1, 2, Qt::AlignLeft);
  layout->addWidget(buttons, row++, 0, 1, 2, Qt::AlignBottom);

  m_appPathEdit->setText(m_settings->value(LxLyrics::appPathKey).toString());
  m_rememberStateCheck->setChecked(m_settings->value(LxLyrics::rememberStateKey).toBool());
}

void LxLyricsSettingsDialog::accept()
{
  // set() notifies the plugin's subscribers: the app path reaches the feed
  // writer at once, and RememberState is read on the next startup restore.
  m_settings->set(LxLyrics::appPathKey, m_appPathEdit->text().trimmed());
  m_settings->set(LxLyrics::rememberStateKey, m_rememberStateCheck->isChecked());

  done(Accepted);
}

LxLyricsPluginSettingsProvider::LxLyricsPluginSettingsProvider(Fooyin::SettingsManager* settings,
                                                               std::function<void()> openSettings)
  : m_settings(settings)
  , m_openSettings(std::move(openSettings))
{
}

QDialog* LxLyricsPluginSettingsProvider::createSettings(QWidget* parent)
{
  return new LxLyricsSettingsDialog(m_settings, m_openSettings, parent);
}
