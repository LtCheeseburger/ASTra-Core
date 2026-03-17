#include "UpdateChecker.hpp"
#include "UpdaterConfig.hpp"

#include <gf/core/log.hpp>

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>

namespace gf::gui::update {

// ── helpers ─────────────────────────────────────────────────────────────────

// Strip leading "v" and any pre-release suffix like "-beta", "-rc1".
// Returns a list of numeric components: "0.9.1" → {0, 9, 1}
static QList<int> parseVersionComponents(const QString& raw) {
    QString s = raw.trimmed();
    if (s.startsWith('v') || s.startsWith('V'))
        s = s.mid(1);
    // Drop pre-release portion (everything after the first '-')
    int dash = s.indexOf('-');
    if (dash != -1)
        s = s.left(dash);

    QList<int> parts;
    for (const QString& p : s.split('.'))
        parts.append(p.toInt());
    return parts;
}

// ── UpdateChecker ────────────────────────────────────────────────────────────

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
    gf::core::Log::get()->info("[Updater] Checking GitHub for updates");

    const QString url = QStringLiteral(
        "https://api.github.com/repos/%1/%2/releases/latest")
        .arg(m_owner, m_repo);

    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Accept",     "application/vnd.github+json");
    req.setRawHeader("User-Agent", "ASTra-Updater/1.0");
    m_nam->get(req);
}

// static
bool UpdateChecker::isNewer(const QString& localVersion,
                            const QString& remoteTag) {
    const auto local  = parseVersionComponents(localVersion);
    const auto remote = parseVersionComponents(remoteTag);

    const int len = qMax(local.size(), remote.size());
    for (int i = 0; i < len; ++i) {
        const int l = (i < local.size())  ? local[i]  : 0;
        const int r = (i < remote.size()) ? remote[i] : 0;
        if (r > l) return true;
        if (r < l) return false;
    }
    return false; // identical
}

void UpdateChecker::onReplyFinished(QNetworkReply* reply) {
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        const QString err = reply->errorString();
        gf::core::Log::get()->warn("[Updater] Network error: {}", err.toStdString());
        emit checkFailed(err);
        return;
    }

    const QByteArray body = reply->readAll();
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        gf::core::Log::get()->warn("[Updater] Failed to parse GitHub response");
        emit checkFailed(tr("Failed to parse server response."));
        return;
    }

    const QJsonObject obj = doc.object();
    const QString tagName = obj.value("tag_name").toString();

    if (tagName.isEmpty()) {
        emit checkFailed(tr("No release tag found in response."));
        return;
    }

    gf::core::Log::get()->info("[Updater] Latest version found: {}",
                               tagName.toStdString());

    // Pull the local version from the compiled-in constant.
    const QString localVer = ASTRA_CURRENT_VERSION_QSTRING;

    if (!isNewer(localVer, tagName)) {
        gf::core::Log::get()->info("[Updater] Already running latest version");
        emit upToDate();
        return;
    }

    // Find a suitable download asset (prefer .zip, then first asset).
    const QJsonArray assets = obj.value("assets").toArray();
    QString downloadUrl;
    QString assetName;
    for (const QJsonValue& v : assets) {
        const QJsonObject asset = v.toObject();
        const QString name = asset.value("name").toString();
        if (name.endsWith(".zip", Qt::CaseInsensitive) ||
            name.endsWith(".exe", Qt::CaseInsensitive)) {
            downloadUrl = asset.value("browser_download_url").toString();
            assetName   = name;
            break;
        }
    }
    // If nothing matched, fall back to the first asset.
    if (downloadUrl.isEmpty() && !assets.isEmpty()) {
        const QJsonObject first = assets.first().toObject();
        downloadUrl = first.value("browser_download_url").toString();
        assetName   = first.value("name").toString();
    }

    ReleaseInfo info;
    info.tagName    = tagName;
    info.name       = obj.value("name").toString();
    info.body       = obj.value("body").toString();
    info.downloadUrl = downloadUrl;
    info.assetName  = assetName;

    emit updateAvailable(info);
}

} // namespace gf::gui::update
