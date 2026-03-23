#pragma once
#include "ModProfile.hpp"
#include <QWidget>
#include <optional>

class QLabel;
class QPushButton;

namespace gf::gui {

class ProfileContextService;

// Compact status-bar widget showing the active mod profile for the current game.
// The "Profiles..." button opens the management dialog via a signal.
class ActiveProfileWidget : public QWidget {
    Q_OBJECT
public:
    explicit ActiveProfileWidget(ProfileContextService* ctx, QWidget* parent = nullptr);

    // Manually refresh display from current context state.
    void refresh();

signals:
    // Emitted when the user clicks the "Profiles..." button.
    void manageProfilesRequested();

private:
    void onContextChanged(const std::optional<ModProfile>& profile);

    ProfileContextService* m_ctx   = nullptr;
    QLabel*                m_label = nullptr;
    QPushButton*           m_btn   = nullptr;
};

} // namespace gf::gui
