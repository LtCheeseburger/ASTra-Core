#include "VersionBadgeWidget.hpp"
#include "UpdateChecker.hpp"
#include "UpdaterConfig.hpp"

#include <QLabel>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QCursor>

namespace gf::gui::update {

// Pill colours (background hex strings).
static constexpr const char* kColourChecking = "#888888";
static constexpr const char* kColourLatest   = "#27ae60";
static constexpr const char* kColourUpdate   = "#e67e22";
static constexpr const char* kColourBeta     = "#2980b9";
static constexpr const char* kColourNightly  = "#7f8c8d";
static constexpr const char* kColourError    = "#c0392b";

// Pill label stylesheet template.
static QString pillStyle(const QString& bg) {
    return QStringLiteral(
        "QLabel {"
        "  background-color: %1;"
        "  color: white;"
        "  border-radius: 8px;"
        "  padding: 1px 7px;"
        "  font-size: 11px;"
        "  font-weight: bold;"
        "}").arg(bg);
}

// ── VersionBadgeWidget ────────────────────────────────────────────────────────

VersionBadgeWidget::VersionBadgeWidget(const QString& currentVersion,
                                       QWidget* parent)
    : QWidget(parent)
    , m_currentVersion(currentVersion)
{
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Click to check for updates"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 6, 2);
    layout->setSpacing(6);

    m_versionLabel = new QLabel(currentVersion, this);
    m_versionLabel->setStyleSheet("QLabel { font-weight: bold; font-size: 12px; }");

    m_pillLabel = new QLabel(this);
    m_pillLabel->hide(); // hidden until the first check completes

    layout->addWidget(m_versionLabel);
    layout->addWidget(m_pillLabel);
    setLayout(layout);
}

void VersionBadgeWidget::applyPillStyle(const QString& text, const QString& bgColour) {
    m_pillLabel->setText(text);
    m_pillLabel->setStyleSheet(pillStyle(bgColour));
    m_pillLabel->show();
}

void VersionBadgeWidget::setStatusChecking() {
    m_status = BadgeStatus::Checking;
    applyPillStyle(tr("Checking\u2026"), kColourChecking);
    setToolTip(tr("Checking for updates\u2026"));
}

void VersionBadgeWidget::setStatusLatest() {
    m_status = BadgeStatus::Latest;
    applyPillStyle(tr("Latest"), kColourLatest);
    setToolTip(QStringLiteral("%1\n%2")
        .arg(tr("You are running the latest version."))
        .arg(tr("Click to check again.")));
}

void VersionBadgeWidget::setStatusUpdateAvailable(const ReleaseInfo& info) {
    m_latestTag = info.tagName;

    const auto cls = info.parsedVersion.classification;
    using RC = gf::core::ReleaseClassification;

    if (cls == RC::Stable || cls == RC::ReleaseCandidate) {
        m_status = BadgeStatus::UpdateAvailable;
        applyPillStyle(tr("Update Available"), kColourUpdate);
    } else if (cls == RC::Beta) {
        m_status = BadgeStatus::BetaAvailable;
        applyPillStyle(tr("Beta Available"), kColourBeta);
    } else {
        m_status = BadgeStatus::NightlyAvailable;
        applyPillStyle(tr("Nightly Available"), kColourNightly);
    }

    const QString releaseTitle = info.name.isEmpty() ? info.tagName : info.name;
    setToolTip(QStringLiteral(
        "%1\n\n"
        "%2: %3\n"
        "%4: %5\n\n"
        "%6")
        .arg(tr("A newer version is available."))
        .arg(tr("Installed")).arg(m_currentVersion)
        .arg(tr("Available")).arg(info.tagName)
        .arg(tr("Click to view and install the update.")));
}

void VersionBadgeWidget::setStatusError(const QString& reason) {
    m_status = BadgeStatus::Error;
    applyPillStyle(tr("!"), kColourError);
    setToolTip(QStringLiteral("%1\n\n%2\n\n%3")
        .arg(tr("Update check failed."))
        .arg(reason)
        .arg(tr("Click to try again.")));
}

void VersionBadgeWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        emit clicked();
    QWidget::mousePressEvent(event);
}

} // namespace gf::gui::update
