#include "ModProfileManager.hpp"
#include "ModWorkspaceManager.hpp"
#include "ProfileWorkspaceNaming.hpp"

#include "gf/core/log.hpp"

#include <QDateTime>
#include <QUuid>

namespace gf::gui {

static QString nowUtcIso() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

static ModProfileId newProfileId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

ModProfileManager::ModProfileManager(QObject* parent)
    : QObject(parent) {}

bool ModProfileManager::load(QString* outErr) {
    const bool ok = m_store.loadIndex(m_profiles, m_activeByGame, outErr);
    m_loaded = ok;
    return ok;
}

// ── Queries ───────────────────────────────────────────────────────────────────

QVector<ModProfile> ModProfileManager::profilesForGame(const QString& gameId) const {
    QVector<ModProfile> result;
    for (const auto& p : m_profiles) {
        if (p.gameId == gameId) result.append(p);
    }
    return result;
}

std::optional<ModProfile> ModProfileManager::findById(const ModProfileId& id) const {
    for (const auto& p : m_profiles) {
        if (p.id == id) return p;
    }
    return std::nullopt;
}

std::optional<ModProfileId> ModProfileManager::activeProfileId(const QString& gameId) const {
    const auto it = m_activeByGame.constFind(gameId);
    if (it == m_activeByGame.cend()) return std::nullopt;
    return it.value();
}

std::optional<ModProfile> ModProfileManager::activeProfile(const QString& gameId) const {
    const auto id = activeProfileId(gameId);
    if (!id) return std::nullopt;
    return findById(*id);
}

// ── Mutations ─────────────────────────────────────────────────────────────────

bool ModProfileManager::createProfile(const QString& gameId,
                                      const QString& name,
                                      const QString& description,
                                      ModProfile*    outProfile,
                                      QString*       outErr,
                                      const QString& gameDisplayName) {
    if (gameId.isEmpty()) {
        if (outErr) *outErr = "Game ID must not be empty";
        return false;
    }
    const QString trimmedName = name.trimmed();
    if (trimmedName.isEmpty()) {
        if (outErr) *outErr = "Profile name must not be empty";
        return false;
    }

    // Reject duplicate names within the same game (case-insensitive)
    for (const auto& p : m_profiles) {
        if (p.gameId == gameId &&
            p.name.compare(trimmedName, Qt::CaseInsensitive) == 0) {
            if (outErr)
                *outErr = QString("A profile named \"%1\" already exists for this game")
                              .arg(trimmedName);
            return false;
        }
    }

    const ModProfileId id  = newProfileId();
    const QString      now = nowUtcIso();

    // Phase 5D: use a readable slug-based path when a display name is available.
    // Falls back to the legacy <gameId>/<profileId> format when not provided.
    const QString workspace = gameDisplayName.isEmpty()
        ? ModProfileStore::workspacePathFor(gameId, id)
        : ProfileWorkspaceNaming::namedWorkspacePath(
              ModProfileStore::defaultWorkspaceRoot(),
              gameDisplayName, trimmedName, id);

    ModProfile profile;
    profile.id            = id;
    profile.gameId        = gameId;
    profile.name          = trimmedName;
    profile.description   = description.trimmed();
    profile.workspacePath = workspace;
    profile.createdAt     = now;
    profile.updatedAt     = now;

    // 1. Create workspace directory tree
    const WorkspaceLayout layout = WorkspaceLayout::from(workspace);
    if (!ModWorkspaceManager::createWorkspace(layout, outErr)) return false;

    // 2. Write profile.json into workspace (self-describing marker)
    if (!m_store.writeProfileJson(profile, outErr)) return false;

    // 3. Add to in-memory index and persist
    m_profiles.append(profile);
    if (!persist(outErr)) {
        m_profiles.removeLast(); // rollback in-memory addition
        return false;
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModProfileManager: profile created",
                      (trimmedName + " | " + id).toStdString());

    if (outProfile) *outProfile = profile;
    emit profilesChanged(gameId);
    return true;
}

bool ModProfileManager::renameProfile(const ModProfileId& id,
                                      const QString&      newName,
                                      QString*            outErr) {
    const QString trimmed = newName.trimmed();
    if (trimmed.isEmpty()) {
        if (outErr) *outErr = "Profile name must not be empty";
        return false;
    }

    int idx = -1;
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == id) { idx = i; break; }
    }
    if (idx < 0) {
        if (outErr) *outErr = "Profile not found";
        return false;
    }

    const QString gameId = m_profiles[idx].gameId;

    // Reject duplicate names within the same game (excluding self)
    for (const auto& p : m_profiles) {
        if (p.id != id && p.gameId == gameId &&
            p.name.compare(trimmed, Qt::CaseInsensitive) == 0) {
            if (outErr)
                *outErr = QString("A profile named \"%1\" already exists for this game")
                              .arg(trimmed);
            return false;
        }
    }

    const QString oldName = m_profiles[idx].name;
    m_profiles[idx].name      = trimmed;
    m_profiles[idx].updatedAt = nowUtcIso();

    // Best-effort update of workspace profile.json
    m_store.writeProfileJson(m_profiles[idx], nullptr);

    if (!persist(outErr)) {
        m_profiles[idx].name = oldName; // rollback
        return false;
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModProfileManager: profile renamed",
                      (oldName + " -> " + trimmed).toStdString());

    emit profilesChanged(gameId);
    return true;
}

bool ModProfileManager::deleteProfile(const ModProfileId& id,
                                      bool                deleteWorkspace,
                                      QString*            outErr) {
    int idx = -1;
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == id) { idx = i; break; }
    }
    if (idx < 0) {
        if (outErr) *outErr = "Profile not found";
        return false;
    }

    const ModProfile saved = m_profiles[idx];

    // Clear active-profile mapping if this profile was active
    const bool wasActive = (m_activeByGame.value(saved.gameId) == id);
    if (wasActive) m_activeByGame.remove(saved.gameId);

    m_profiles.removeAt(idx);

    if (!persist(outErr)) {
        // Rollback
        m_profiles.insert(idx, saved);
        if (wasActive) m_activeByGame.insert(saved.gameId, id);
        return false;
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModProfileManager: profile deleted",
                      (saved.name + " | " + id).toStdString());

    if (deleteWorkspace) {
        QString wsErr;
        if (!ModWorkspaceManager::deleteWorkspace(WorkspaceLayout::from(saved.workspacePath), &wsErr)) {
            // Non-fatal: profile is already removed from the index.  Log and continue.
            gf::core::logWarn(gf::core::LogCategory::General,
                              "ModProfileManager: workspace deletion failed (profile removed from index)",
                              wsErr.toStdString());
        }
    }

    emit profilesChanged(saved.gameId);
    if (wasActive) emit activeProfileChanged(saved.gameId, {});
    return true;
}

bool ModProfileManager::setActiveProfile(const QString&      gameId,
                                         const ModProfileId& id,
                                         QString*            outErr) {
    const auto opt = findById(id);
    if (!opt || opt->gameId != gameId) {
        if (outErr) *outErr = "Profile not found for this game";
        return false;
    }

    const auto prev = m_activeByGame.value(gameId);
    m_activeByGame.insert(gameId, id);

    if (!persist(outErr)) {
        // Restore previous state
        if (prev.isEmpty()) m_activeByGame.remove(gameId);
        else m_activeByGame.insert(gameId, prev);
        return false;
    }

    gf::core::logInfo(gf::core::LogCategory::General,
                      "ModProfileManager: active profile set",
                      (gameId + " -> " + id).toStdString());

    emit activeProfileChanged(gameId, id);
    return true;
}

void ModProfileManager::clearActiveProfile(const QString& gameId) {
    if (!m_activeByGame.contains(gameId)) return;
    m_activeByGame.remove(gameId);
    persist(nullptr); // best-effort
    emit activeProfileChanged(gameId, {});
}

bool ModProfileManager::markAsBaseline(const ModProfileId& id,
                                        bool                isBaseline,
                                        BaselineType        type,
                                        QString*            outErr) {
    if (!m_loaded) {
        if (outErr) *outErr = "Profile manager not loaded.";
        return false;
    }

    // Find the profile
    for (auto& p : m_profiles) {
        if (p.id == id) {
            p.isBaseline   = isBaseline;
            p.baselineType = type;
            p.updatedAt    = nowUtcIso();

            QString persistErr;
            if (!persist(&persistErr)) {
                // Rollback in-memory change
                p.isBaseline   = !isBaseline;
                p.baselineType = BaselineType::Custom;
                if (outErr) *outErr = "Failed to persist baseline metadata: " + persistErr;
                return false;
            }

            // Also update per-workspace profile.json
            m_store.writeProfileJson(p, nullptr); // best-effort

            gf::core::logInfo(gf::core::LogCategory::General,
                              "ModProfileManager: baseline metadata updated",
                              id.toStdString());
            return true;
        }
    }

    if (outErr) *outErr = QString("Profile not found: %1").arg(id);
    return false;
}

// ── Private ───────────────────────────────────────────────────────────────────

bool ModProfileManager::persist(QString* outErr) {
    return m_store.saveIndex(m_profiles, m_activeByGame, outErr);
}

} // namespace gf::gui
