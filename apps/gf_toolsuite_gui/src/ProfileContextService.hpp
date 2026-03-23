#pragma once
#include "ModProfile.hpp"
#include <QObject>
#include <optional>

namespace gf::gui {

class ModProfileManager;

// Single source of truth for "what game and what mod profile is active right now."
//
// MainWindow calls setActiveGame() / clearActiveGame() as games are opened.
// Other subsystems (dialogs, widgets, future save-routing) query activeProfile()
// or connect to activeProfileChanged() without coupling to ModProfileManager directly.
class ProfileContextService : public QObject {
    Q_OBJECT
public:
    explicit ProfileContextService(ModProfileManager* mgr, QObject* parent = nullptr);

    // Called by MainWindow when a game is opened / closed.
    void setActiveGame(const QString& gameId, const QString& gameDisplayName);
    void clearActiveGame();

    // Current game context
    QString activeGameId()          const { return m_gameId; }
    QString activeGameDisplayName() const { return m_gameDisplayName; }
    bool    hasActiveGame()         const { return !m_gameId.isEmpty(); }

    // Current profile context
    std::optional<ModProfile> activeProfile()  const;
    bool                      hasActiveProfile() const;

signals:
    // Fired when the active game changes (gameId/displayName are empty when cleared).
    void activeGameChanged(const QString& gameId, const QString& displayName);

    // Fired when the active profile changes for the current game.
    // profile is nullopt when there is no active profile.
    void activeProfileChanged(const std::optional<ModProfile>& profile);

private slots:
    void onManagerActiveProfileChanged(const QString& gameId, const QString& newProfileId);

private:
    ModProfileManager* m_mgr          = nullptr;
    QString            m_gameId;
    QString            m_gameDisplayName;
};

} // namespace gf::gui
