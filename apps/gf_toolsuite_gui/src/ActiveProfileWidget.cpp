#include "ActiveProfileWidget.hpp"
#include "ProfileContextService.hpp"

#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace gf::gui {

ActiveProfileWidget::ActiveProfileWidget(ProfileContextService* ctx, QWidget* parent)
    : QWidget(parent), m_ctx(ctx)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(4);

    m_label = new QLabel("Profile: (none)", this);
    m_label->setToolTip("Active mod profile for the current game");
    layout->addWidget(m_label);

    m_btn = new QPushButton("Profiles\u2026", this);
    m_btn->setFlat(true);
    m_btn->setFixedHeight(18);
    m_btn->setToolTip("Open Mod Profiles manager");
    layout->addWidget(m_btn);

    connect(m_btn, &QPushButton::clicked, this, &ActiveProfileWidget::manageProfilesRequested);

    connect(m_ctx, &ProfileContextService::activeProfileChanged,
            this, &ActiveProfileWidget::onContextChanged);
    connect(m_ctx, &ProfileContextService::activeGameChanged,
            this, [this](const QString&, const QString&) { refresh(); });

    refresh();
}

void ActiveProfileWidget::onContextChanged(const std::optional<ModProfile>& /*profile*/) {
    refresh();
}

void ActiveProfileWidget::refresh() {
    if (!m_ctx->hasActiveGame()) {
        m_label->setText("Profile: (none)");
        m_label->setToolTip("No game selected");
        m_btn->setEnabled(false);
        return;
    }

    m_btn->setEnabled(true);

    const auto profile = m_ctx->activeProfile();
    if (profile) {
        m_label->setText(QString("Profile: %1").arg(profile->name));
        m_label->setToolTip(
            QString("Active: %1\nWorkspace: %2")
                .arg(profile->name,
                     QDir::toNativeSeparators(profile->workspacePath)));
    } else {
        m_label->setText("Profile: (none)");
        m_label->setToolTip(
            "No active mod profile \u2014 click Profiles\u2026 to create one");
    }
}

} // namespace gf::gui
