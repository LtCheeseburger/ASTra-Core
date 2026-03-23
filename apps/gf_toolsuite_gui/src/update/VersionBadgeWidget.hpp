#pragma once
#include <QWidget>
#include <QString>

class QLabel;

namespace gf::gui::update {

struct ReleaseInfo;

// Current state of the version badge — drives pill colour and text.
enum class BadgeStatus {
    Idle,              // no check has run yet (shows version only)
    Checking,          // check in progress
    Latest,            // up to date — matches the latest channel release
    UpdateAvailable,   // newer stable release found
    BetaAvailable,     // newer beta/RC release found
    NightlyAvailable,  // newer nightly/dev release found
    PreReleaseBuild,   // local version is AHEAD of the latest GitHub release
    Error              // network or parse failure
};

// VersionBadgeWidget — compact inline widget displayed in the main toolbar.
//
// Shows the installed version string on the left and a coloured status pill
// on the right.  Clicking anywhere on the widget emits clicked().
//
// Typical usage:
//   1. Construct and add to toolbar.
//   2. Connect clicked() → onCheckForUpdates() in MainWindow.
//   3. After each update-check cycle, call the appropriate setStatus*() method.
class VersionBadgeWidget : public QWidget {
    Q_OBJECT
public:
    explicit VersionBadgeWidget(const QString& currentVersion,
                                QWidget* parent = nullptr);

    // Update the badge to reflect a running check.
    void setStatusChecking();

    // Update the badge when the installed version is the newest.
    void setStatusLatest();

    // Update the badge when a newer release was found.
    void setStatusUpdateAvailable(const ReleaseInfo& info);

    // Update the badge when this build is ahead of the latest GitHub release.
    void setStatusPreReleaseBuild(const ReleaseInfo& latestRelease);

    // Update the badge when the check failed (network / parse error).
    void setStatusError(const QString& reason);

    BadgeStatus status()    const { return m_status; }
    QString     latestTag() const { return m_latestTag; }

signals:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void applyPillStyle(const QString& text, const QString& bgColour);

    QString      m_currentVersion;
    QString      m_latestTag;
    BadgeStatus  m_status = BadgeStatus::Idle;

    QLabel* m_versionLabel = nullptr;
    QLabel* m_pillLabel    = nullptr;
};

} // namespace gf::gui::update
