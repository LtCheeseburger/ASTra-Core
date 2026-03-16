#include "AptSelectionManager.hpp"

namespace gf::gui::apt_editor {

AptSelectionManager::AptSelectionManager(QObject* parent)
    : QObject(parent) {}

void AptSelectionManager::setSelectedPlacementIndex(int index) {
  if (m_selectedPlacementIndex == index) return;
  m_selectedPlacementIndex = index;
  emit selectedPlacementChanged(m_selectedPlacementIndex);
}

void AptSelectionManager::clearSelection() {
  setSelectedPlacementIndex(-1);
}

} // namespace gf::gui::apt_editor
