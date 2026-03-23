#include "ProfileWorkspaceNaming.hpp"

#include <QDir>
#include <QRegularExpression>

namespace gf::gui {

// static
QString ProfileWorkspaceNaming::makeSlug(const QString& displayName,
                                          const QString& fallback) {
    QString s = displayName.trimmed().toLower();

    // Replace any run of non-[a-z0-9] characters with a single hyphen.
    // This covers spaces, punctuation, emoji (multi-byte), and anything else.
    static const QRegularExpression kNonAlnum(QStringLiteral("[^a-z0-9]+"));
    s = s.replace(kNonAlnum, QStringLiteral("-"));

    // Strip leading/trailing hyphens produced by the replacement above.
    while (s.startsWith(QLatin1Char('-'))) s.remove(0, 1);
    while (s.endsWith(QLatin1Char('-')))   s.chop(1);

    // Truncate and re-strip any trailing hyphen created by the cut point.
    if (s.length() > kMaxSlugLength) {
        s = s.left(kMaxSlugLength);
        while (s.endsWith(QLatin1Char('-'))) s.chop(1);
    }

    return s.isEmpty() ? fallback : s;
}

// static
QString ProfileWorkspaceNaming::shortId(const ModProfileId& profileId) {
    const int dash = profileId.indexOf(QLatin1Char('-'));
    return dash > 0 ? profileId.left(dash) : profileId.left(8);
}

// static
QString ProfileWorkspaceNaming::namedWorkspacePath(const QString&      workspacesRoot,
                                                    const QString&      gameDisplayName,
                                                    const QString&      profileName,
                                                    const ModProfileId& profileId) {
    const QString gameSlug   = makeSlug(gameDisplayName, QStringLiteral("game"));
    const QString profileSlug = makeSlug(profileName);
    const QString sid        = shortId(profileId);
    const QString folder     = profileSlug + QStringLiteral("__") + sid;
    return QDir(workspacesRoot).filePath(gameSlug + QLatin1Char('/') + folder);
}

} // namespace gf::gui
