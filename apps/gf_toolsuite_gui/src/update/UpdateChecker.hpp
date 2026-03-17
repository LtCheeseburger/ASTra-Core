#pragma once
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace gf::gui::update {

// Describes a release fetched from the GitHub API.
struct ReleaseInfo {
    QString tagName;        // e.g. "v0.9.1"
    QString name;           // Human-readable release name
    QString body;           // Release notes (Markdown)
    QString downloadUrl;    // browser_download_url of the first matching asset
    QString assetName;      // Original asset filename
};

// UpdateChecker queries the GitHub Releases API and emits signals when done.
// Usage:
//   auto* checker = new UpdateChecker(this);
//   connect(checker, &UpdateChecker::updateAvailable, ...);
//   connect(checker, &UpdateChecker::upToDate, ...);
//   connect(checker, &UpdateChecker::checkFailed, ...);
//   checker->checkForUpdates();
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    // owner   – GitHub user or org (e.g. "MyOrg")
    // repo    – GitHub repository name (e.g. "ASTra")
    explicit UpdateChecker(const QString& owner,
                           const QString& repo,
                           QObject* parent = nullptr);

    // Kick off an async check.  Emits one of the three signals when complete.
    void checkForUpdates();

    // Compare two version strings of the form "v0.9.0-beta" / "0.9.1".
    // Returns true when remoteTag is strictly newer than localVersion.
    static bool isNewer(const QString& localVersion, const QString& remoteTag);

signals:
    void updateAvailable(const ReleaseInfo& info);
    void upToDate();
    void checkFailed(const QString& errorMessage);

private slots:
    void onReplyFinished(QNetworkReply* reply);

private:
    QString m_owner;
    QString m_repo;
    QNetworkAccessManager* m_nam;
};

} // namespace gf::gui::update
