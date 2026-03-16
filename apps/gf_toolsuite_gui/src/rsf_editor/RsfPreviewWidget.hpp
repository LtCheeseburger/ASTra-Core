#pragma once

#include <QGraphicsView>
#include <QHash>
#include <QPixmap>
#include <QWidget>
#include <QWheelEvent>

#include <optional>

#include "gf/models/rsf_preview.hpp"

class QAction;
class QComboBox;
class QDoubleSpinBox;
class QGraphicsRectItem;
class QGraphicsScene;
class QLabel;
class QListWidget;
class QPushButton;
class QToolBar;

namespace gf::gui::rsf_editor {

class RsfGraphicsView final : public QGraphicsView {
public:
  explicit RsfGraphicsView(QWidget* parent = nullptr);
protected:
  void wheelEvent(QWheelEvent* event) override;
};

class RsfPreviewWidget final : public QWidget {
  Q_OBJECT
public:
  explicit RsfPreviewWidget(QWidget* parent = nullptr);

  void setDocument(const gf::models::rsf::preview_document& doc);
  void clear();
  void setTexturePixmap(int textureIndex, const QPixmap& pixmap);
  void setTextureStatus(const QString& text);
  void setSelectionByMaterialIndex(int materialIndex);
  void updateObjectTransform(int materialIndex, const gf::models::rsf::preview_transform& transform);
  void fitView();
  void resetView();

signals:
  void materialSelectionChanged(int materialIndex);
  void transformEdited(int materialIndex, const gf::models::rsf::preview_transform& transform, bool interactive);

private:
  enum class PreviewMode { Proxy = 0, Textured = 1, UV = 2, Wireframe = 3 };

  void rebuildScene();
  void refreshInspector();
  void syncSelectionToList();
  void pushSpinboxValues(const gf::models::rsf::preview_transform& tr);
  int currentObjectIndex() const;
  void selectObject(int index);
  QPixmap buildCheckerboard(const QSize& size) const;
  bool shouldRenderMesh() const;
  void renderMeshCandidate(const gf::models::rsf::geometry_candidate& candidate);

  QToolBar* m_toolbar = nullptr;
  QComboBox* m_modeCombo = nullptr;
  QAction* m_gridAction = nullptr;
  QAction* m_boundsAction = nullptr;
  QAction* m_fitAction = nullptr;
  QAction* m_resetViewAction = nullptr;
  QLabel* m_bannerLabel = nullptr;
  QLabel* m_textureStatusLabel = nullptr;
  RsfGraphicsView* m_view = nullptr;
  QGraphicsScene* m_scene = nullptr;
  QListWidget* m_objectList = nullptr;
  QDoubleSpinBox* m_xSpin = nullptr;
  QDoubleSpinBox* m_ySpin = nullptr;
  QDoubleSpinBox* m_rotSpin = nullptr;
  QDoubleSpinBox* m_scaleXSpin = nullptr;
  QDoubleSpinBox* m_scaleYSpin = nullptr;
  QPushButton* m_resetTransformButton = nullptr;

  std::optional<gf::models::rsf::preview_document> m_doc;
  QHash<int, QGraphicsRectItem*> m_itemsByMaterialIndex;
  QHash<int, QPixmap> m_texturePixmaps;
  bool m_updatingUi = false;
};

} // namespace gf::gui::rsf_editor
