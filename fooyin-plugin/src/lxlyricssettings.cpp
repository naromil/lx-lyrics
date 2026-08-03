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

LxLyricsSettingsPageWidget::LxLyricsSettingsPageWidget(Fooyin::SettingsManager* settings, QWidget* parent)
    : Fooyin::SettingsPageWidget()
    , m_settings(settings)
    , m_appPathEdit(new QLineEdit(this))
    , m_autoSpawnCheck(new QCheckBox(tr("Start desktop lyrics when fooyin starts"), this))
{
    if (parent != nullptr) {
        setParent(parent);
    }

    m_appPathEdit->setPlaceholderText(tr("Auto-detect (lyrics-app in PATH)"));

    auto* layout = new QFormLayout(this);
    layout->addRow(tr("Lyrics app path:"), m_appPathEdit);
    layout->addRow(m_autoSpawnCheck);
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
    setCategory({QStringLiteral("Lyrics")});
    setWidgetCreator([settings] {
        return new LxLyricsSettingsPageWidget(settings);
    });
}
