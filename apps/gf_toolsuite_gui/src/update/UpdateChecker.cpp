#include "UpdateChecker.hpp"
#include "UpdaterConfig.hpp"

#include <gf/core/log.hpp>

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace gf::gui::update {

// ── helpers ───────────────────────────────────────────────────────────────────

// Pick the best download asset from the asset list: prefer .zip first,
// then .exe, then fall back to the first asset.
static void selectAsset(const QJsonArray& assets,
                         QString& outUrl,
                         QString& outName) {
    outUrl.clear();
    outName.clear();
    for (const QJsonValue& v : assets) {
        const QJsonObject a = v.toObject();
        const QString name  = a.value("name").toString();
        if (name.endsWith(".zip", Qt::CaseInsensitive)) {
            outUrl  = a.value("browser_download_url").toString();
            outName = name;
            return; // zip is preferred
        }
    }
    // Second pass: .exe installer
    for (const QJsonValue& v : assets) {
        const QJsonObject a = v.toObject();
        const QString name  = a.value("name").toString();
        if (name.endsWith(".exe", Qt::CaseInsensitive)) {
            outUrl  = a.value("browser_download_url").toString();
            outName = name;
            return;
        }
    }
    // Fallback: first asset
    if (!assets.isEmpty()) {
        const QJsonObject first = assets.first().toObject();
        outUrl  = first.value("browser_download_url").toString();
        outName = first.value("name").toString();
    }
}

// ── UpdateChecker ─────────────────────────────────────────────────────────────

UpdateChecker::UpdateChecker(const QString& owner,
                             const QString& repo,
                             QObject* parent)
    : QObject(parent)
    , m_owner(owner)
    , m_repo(repo)
    , m_nam(new QNetworkAccessManager(this))
{
    connect(m_nam, &QNetworkAccessManager::finished,
            this,  &UpdateChecker::onReplyFinished);
}

void UpdateChecker::checkForUpdates() {
    gf::core::logInfo(gf::core::LogCategory::Update,
                      "Checking GitHub Releases for updates");

    // Fetch the most recent releases (up to 30) so we can filter by channel.
    const QString url = QStringLiteral(
        "https://api.github.com/repos/%1/%2/releases?per_page=30")
        .arg(m_owner, m_repo);

    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Accept",     "application/vnd.github+json");
    req.setRawHeader("User-Agent", "ASTra-Updater/1.0");
    m_nam->get(req);
}

// static
bool UpdateChecker::isNewer(const QString& localVersion,
                             const QString& remoteTag) {
    const auto local  = gf::core::ParsedVersion::parse(localVersion.toStdString());
    const auto remote = gf::core::ParsedVersion::parse(remoteTag.toStdString());
    return remote > local;
}

void UpdateChecker::onReplyFinished(QNetworkReply* reply) {
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        const QString err = reply->errorString();
        gf::core::logWarn(gf::core::LogCategory::Update,
                          "Network error during update check",
                          err.toStdString());
        emit checkFailed(err);
        return;
    }

    const QByteArray body = reply->readAll();
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isArray()) {
        gf::core::logWarn(gf::core::LogCategory::Update,
                          "Failed to parse GitHub releases response");
        emit checkFailed(tr("Failed to parse server response."));
        return;
    }

    const QJsonArray releases = doc.array();
    if (releases.isEmpty()) {
        gf::core::logInfo(gf::core::LogCategory::Update,
                          "GitHub returned zero releases");
        emit upToDate();
        return;
    }

    // Local version for comparison.
    const QString localVerStr  = ASTRA_CURRENT_VERSION_QSTRING;
    const auto    localVersion = gf::core::ParsedVersion::parse(localVerStr.toStdString());

    gf::core::logInfo(gf::core::LogCategory::Update,
                      "Scanning releases for channel",
                      (m_channel == UpdateChannel::Stable  ? "Stable"
                     : m_channel == UpdateChannel::Beta    ? "Beta"
                     :                                       "Nightly"));

    // Walk releases (GitHub returns them newest-first) and find the best
    // candidate that is both eligible on the configured channel AND newer than
    // the installed version.
    ReleaseInfo bestCandidate;
    bool        found = false;

    for (const QJsonValue& val : releases) {
        const QJsonObject obj = val.toObject();

        // Skip drafts always.
        if (obj.value("draft").toBool()) continue;

        const QString tagName = obj.value("tag_name").toString();
        if (tagName.isEmpty()) continue;

        const bool ghPreRelease = obj.value("prerelease").toBool();
        const auto parsed       = gf::core::ParsedVersion::parse(tagName.toStdString());

        // Channel filter: pre-release classification must be within channel.
        if (!releaseVisibleOnChannel(parsed.classification, m_channel)) {
            gf::core::logInfo(gf::core::LogCategory::Update,
                              "Skipping (channel filtered)",
                              tagName.toStdString());
            continue;
        }

        if (parsed > localVersion) {
            // This is a newer, channel-eligible release.
            gf::core::logInfo(gf::core::LogCategory::Update,
                              "Update candidate found",
                              tagName.toStdString());

            ReleaseInfo info;
            info.tagName      = tagName;
            info.name         = obj.value("name").toString();
            info.body         = obj.value("body").toString();
            info.parsedVersion = parsed;
            info.isDraft      = false;
            info.isPreRelease = ghPreRelease;

            selectAsset(obj.value("assets").toArray(),
                        info.downloadUrl, info.assetName);

            bestCandidate = std::move(info);
            found = true;
            break; // releases are newest-first; first match is the best
        }
    }

    if (found) {
        gf::core::logInfo(gf::core::LogCategory::Update,
                          "Update available",
                          bestCandidate.tagName.toStdString());
        emit updateAvailable(bestCandidate);
    } else {
        gf::core::logInfo(gf::core::LogCategory::Update,
                          "Already running the latest eligible version");
        emit upToDate();
    }
}

} // namespace gf::gui::update
