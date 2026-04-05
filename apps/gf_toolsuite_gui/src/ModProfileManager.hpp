#pragma once
#include "ModProfile.hpp"
#include "ModProfileStore.hpp"

#include <QHash>
#include <QObject>
#include <QVector>
#include <optional>

namespace gf::gui {

// Core service: create, rename, delete, list, and select active mod profiles.
//
// Thread affinity: main thread only.  Signals fire on the main thread.
// Must call load() once at startup before any mutating method.
class ModProfileManager : public QObject {
    Q_OBJECT
public:
    explicit ModProfileManager(QObject* parent = nullptr);

    // Load the persisted index from disk.  Safe to call when the file is absent
    // (first run).  Returns false only on I/O or parse errors.
    bool load(QString* outErr);

    // ── Queries ──────────────────────────────────────────────────────────────

    QVector<ModProfile>       profilesForGame(const QString& gameId) const;
    std::optional<ModProfile> findById(const ModProfileId& id) const;

    std::optional<ModProfileId> activeProfileId(const QString& gameId) const;
    std::optional<ModProfile>   activeProfile(const QString& gameId) const;

    // ── Mutations ────────────────────────────────────────────────────────────

    // Create a new profile + workspace for the given game.
    // On success populates *outProfile (if non-null) and emits profilesChanged().
    //
    // gameDisplayName (Phase 5D): when non-empty, the workspace folder uses a
    // human-readable slug-based path instead of the raw gameId/profileId path.
    //   New:    workspaces/<gameSlug>/<profileSlug>__<shortId>/
    //   Legacy: workspaces/<gameId>/<profileId>/
    // Passing an empty gameDisplayName preserves legacy behavior for existing callers.
    bool createProfile(const QString& gameId,
                       const QString& name,
                       const QString& description,
                       ModProfile*    outProfile,
                       QString*       outErr,
                       const QString& gameDisplayName = {});

    // Rename.  Emits profilesChanged() on success.
    bool renameProfile(const ModProfileId& id,
                       const QString&      newName,
                       QString*            outErr);

    // Delete.  If deleteWorkspace is true the workspace directory tree is also
    // removed (guarded by ModWorkspaceManager's safety check).
    // The active-profile mapping for the game is cleared automatically if needed.
    // Emits profilesChanged() and activeProfileChanged() on success.
    bool deleteProfile(const ModProfileId& id,
                       bool                deleteWorkspace,
                       QString*            outErr);

    // Mark a profile as the active one for its game.  Emits activeProfileChanged().
    bool setActiveProfile(const QString&      gameId,
                          const ModProfileId& id,
                          QString*            outErr);

    // Clear the active-profile selection for a game.  Emits activeProfileChanged().
    void clearActiveProfile(const QString& gameId);

    // Mark or un-mark a profile as a baseline capture.  Updates isBaseline and
    // baselineType fields and persists.  Does NOT emit profilesChanged (the list
    // content is unchanged; callers that care may re-query).
    bool markAsBaseline(const ModProfileId& id,
                        bool                isBaseline,
                        BaselineType        type,
                        QString*            outErr);

    // Phase 7: update mutable fields of an existing profile (name, description,
    // sourcePath).  Immutable fields (id, gameId, workspacePath, createdAt) are
    // ignored even if different in the supplied struct.
    // Does NOT emit profilesChanged — callers that show the profile name should
    // re-query if they need the change reflected immediately.
    bool updateProfile(const ModProfile& updated, QString* outErr);

signals:
    // Fired whenever the profile list for a game changes (create / rename / delete).
    void profilesChanged(const QString& gameId);

    // Fired when the active profile for a game changes.
    // newProfileId is empty when the selection is cleared.
    void activeProfileChanged(const QString& gameId, const QString& newProfileId);

private:
    bool persist(QString* outErr);

    ModProfileStore              m_store;
    QVector<ModProfile>          m_profiles;
    QHash<QString, ModProfileId> m_activeByGame; // gameId -> profileId
    bool                         m_loaded = false;
};

} // namespace gf::gui
