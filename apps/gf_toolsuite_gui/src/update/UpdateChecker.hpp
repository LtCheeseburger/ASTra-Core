#pragma once
#include <QObject>
#include <QString>
#include <QUrl>

#include <gf/core/version_model.hpp>

class QNetworkAccessManager;
class QNetworkReply;

namespace gf::gui::update {

// Which tier of releases the user is willing to receive.
// Stored via QSettings key "update/channel" as an integer.
enum class UpdateChannel {
    Stable,   // Only Stable releases (default)
    Beta,     // Stable + ReleaseCandidate + Beta
    Nightly   // All releases including Nightly and Dev
};

// Returns true when a release with the given classification is visible under channel.
inline bool releaseVisibleOnChannel(gf::core::ReleaseClassification cls,
                                    UpdateChannel channel) noexcept {
    const int rank    = gf::core::classificationRank(cls);
    const int maxRank = (channel == UpdateChannel::Stable)  ? 0
                      : (channel == UpdateChannel::Beta)    ? 2
                      :                                       99;
    return rank <= maxRank;
}

// Describes a release fetched from the GitHub API.
struct ReleaseInfo {
    QString tagName;         // e.g. "v1.0.2"
    QString name;            // Human-readable release name
    QString body;            // Release notes (Markdown)
    QString downloadUrl;     // browser_download_url of the best matching asset
    QString assetName;       // Original asset filename

    // Parsed and classified version (populated by UpdateChecker).
    gf::core::ParsedVersion  parsedVersion;
    bool                     isDraft      = false;
    bool                     isPreRelease = false;
};

// UpdateChecker fetches the GitHub Releases list, filters by UpdateChannel,
// and emits signals indicating whether a newer release exists.
//
// Usage:
//   auto* checker = new UpdateChecker(owner, repo, this);
//   checker->setChannel(UpdateChannel::Stable);
//   connect(checker, &UpdateChecker::updateAvailable, ...);
//   connect(checker, &UpdateChecker::upToDate, ...);
//   connect(checker, &UpdateChecker::checkFailed, ...);
//   checker->checkForUpdates();
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(const QString& owner,
                           const QString& repo,
                           QObject* parent = nullptr);

    // Set the channel preference before calling checkForUpdates().
    void setChannel(UpdateChannel channel) { m_channel = channel; }
    UpdateChannel channel() const          { return m_channel; }

    // Kick off an async check.  Emits one of the three signals when complete.
    void checkForUpdates();

    // Compare two version strings, returning true when remoteTag is strictly
    // newer than localVersion (channel-aware; pre-release handling via ParsedVersion).
    static bool isNewer(const QString& localVersion, const QString& remoteTag);

signals:
    // A newer release that is eligible under the configured channel was found.
    void updateAvailable(const gf::gui::update::ReleaseInfo& info);

    // No newer release is available for the configured channel.
    void upToDate();

    // The check could not complete (network error, JSON parse failure, etc.).
    void checkFailed(const QString& errorMessage);

private slots:
    void onReplyFinished(QNetworkReply* reply);

private:
    QString                m_owner;
    QString                m_repo;
    UpdateChannel          m_channel = UpdateChannel::Stable;
    QNetworkAccessManager* m_nam;
};

} // namespace gf::gui::update
