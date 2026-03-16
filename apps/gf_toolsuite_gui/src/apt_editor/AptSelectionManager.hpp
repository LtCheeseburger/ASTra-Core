#pragma once

#include <QObject>

namespace gf::gui::apt_editor {

class AptSelectionManager final : public QObject {
  Q_OBJECT
public:
  explicit AptSelectionManager(QObject* parent = nullptr);

  [[nodiscard]] int selectedPlacementIndex() const noexcept { return m_selectedPlacementIndex; }
  void setSelectedPlacementIndex(int index);
  void clearSelection();

signals:
  void selectedPlacementChanged(int placementIndex);

private:
  int m_selectedPlacementIndex = -1;
};

} // namespace gf::gui::apt_editor
