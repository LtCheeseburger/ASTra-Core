#include "ProfileContextService.hpp"
#include "ModProfileManager.hpp"
#include "ProfileWorkspaceBuilder.hpp"

namespace gf::gui {

ProfileContextService::ProfileContextService(ModProfileManager* mgr, QObject* parent)
    : QObject(parent), m_mgr(mgr)
{
    Q_ASSERT(mgr);
    connect(mgr, &ModProfileManager::activeProfileChanged,
            this, &ProfileContextService::onManagerActiveProfileChanged);
}

void ProfileContextService::setActiveGame(const QString& gameId,
                                          const QString& gameDisplayName) {
    if (m_gameId == gameId && m_gameDisplayName == gameDisplayName) return;
    m_gameId          = gameId;
    m_gameDisplayName = gameDisplayName;
    emit activeGameChanged(gameId, gameDisplayName);
    emit activeProfileChanged(activeProfile());
}

void ProfileContextService::clearActiveGame() {
    if (m_gameId.isEmpty()) return;
    m_gameId.clear();
    m_gameDisplayName.clear();
    emit activeGameChanged({}, {});
    emit activeProfileChanged(std::nullopt);
}

std::optional<ModProfile> ProfileContextService::activeProfile() const {
    if (m_gameId.isEmpty()) return std::nullopt;
    return m_mgr->activeProfile(m_gameId);
}

bool ProfileContextService::hasActiveProfile() const {
    return activeProfile().has_value();
}

QString ProfileContextService::editContentRoot() const {
    const auto profile = activeProfile();
    if (!profile) return {};
    if (!ProfileWorkspaceBuilder::isGameCopyPopulated(*profile)) return {};
    return ProfileWorkspaceBuilder::gameCopyPath(*profile);
}

bool ProfileContextService::isEditingProfileCopy() const {
    return !editContentRoot().isEmpty();
}

void ProfileContextService::onManagerActiveProfileChanged(const QString& gameId,
                                                           const QString& /*newProfileId*/) {
    // Only re-broadcast if it's the currently active game
    if (gameId != m_gameId) return;
    emit activeProfileChanged(activeProfile());
}

} // namespace gf::gui
