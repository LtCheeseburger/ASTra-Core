#pragma once

#include <gf/apt/apt_reader.hpp>
#include <gf/apt/apt_renderer.hpp>

#include <QGraphicsEllipseItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QPointer>

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gf::gui::apt_editor {

class AptSelectionManager;
class AptPreviewItem;

class AptTransformHandle final : public QGraphicsObject {
  Q_OBJECT
public:
  enum class Mode {
    Move,
    Scale,
    Rotate,
  };

  AptTransformHandle(class AptPreviewScene* scene,
                     Mode mode,
                     QGraphicsItem* parent = nullptr);

  [[nodiscard]] QRectF boundingRect() const override;
  void paint(QPainter* painter,
             const QStyleOptionGraphicsItem* option,
             QWidget* widget) override;

  [[nodiscard]] Mode mode() const noexcept { return m_mode; }

protected:
  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
  void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
  void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
  class AptPreviewScene* m_editorScene = nullptr;
  Mode m_mode = Mode::Move;
  QPointF m_pressScenePos;
};

class AptPreviewScene final : public QGraphicsScene {
  Q_OBJECT
public:
  struct Context {
    gf::apt::AptFile* file = nullptr;
    std::vector<gf::apt::AptFrame>* timelineFrames = nullptr;
    std::vector<gf::apt::AptCharacter>* characterTable = nullptr;
    int ownerKind = 0;   // 0=root, 1=character
    int ownerIndex = -1; // character index when ownerKind==1
    int frameIndex = 0;
    bool debugOverlay = false;
  };

  explicit AptPreviewScene(QObject* parent = nullptr);
  ~AptPreviewScene() override;

  void setSelectionManager(AptSelectionManager* selectionManager);
  void setContext(Context context);
  [[nodiscard]] const Context& context() const noexcept { return m_context; }

  void rebuildEditorOverlay();
  void clearEditorOverlay();
  void syncSelectionFromExternal(int placementIndex);
  void requestSelectionFromScene(int placementIndex);

  [[nodiscard]] int selectedPlacementIndex() const noexcept;
  [[nodiscard]] AptPreviewItem* selectedPreviewItem() const noexcept;

  bool bringSelectionForward();
  bool sendSelectionBackward();
  bool removeSelection();
  bool duplicateSelection();
  bool addPlacement();

  void notifyPlacementChanged(AptPreviewItem* item, bool interactive);
  void applyHandleDrag(AptTransformHandle::Mode mode,
                       const QPointF& pressScenePos,
                       const QPointF& currentScenePos,
                       bool finished);

  std::function<void(int placementIndex)> onPlacementSelected;
  std::function<void(int placementIndex, bool interactive)> onPlacementEdited;

signals:
  void sceneSelectionChanged(int placementIndex);

private:
  friend class AptPreviewItem;
  friend class AptTransformHandle;

  struct HandlePack {
    AptTransformHandle* move = nullptr;
    AptTransformHandle* scale = nullptr;
    AptTransformHandle* rotate = nullptr;
  };

  [[nodiscard]] std::vector<gf::apt::AptPlacement>* editablePlacements() const;
  [[nodiscard]] gf::apt::AptPlacement* editablePlacementAt(int placementIndex) const;
  [[nodiscard]] std::vector<gf::apt::RenderNode> currentRenderNodes() const;
  [[nodiscard]] QRectF localBoundsForPlacement(int placementIndex,
                                               const std::vector<gf::apt::RenderNode>& nodes) const;
  void updateSelectionVisuals();
  void rebuildHandles();
  void clearHandles();
  void selectPlacementInternal(int placementIndex, bool fromExternal);
  void reindexPlacementItems();
  void syncEditedPlacementIntoFrameItems(int placementIndex);
  void swapPlacements(int lhs, int rhs);

  Context m_context;
  QPointer<AptSelectionManager> m_selectionManager;
  std::vector<AptPreviewItem*> m_previewItems;
  HandlePack m_handles;
  gf::apt::AptTransform m_handleDragStartTransform;
  QRectF m_handleDragStartBounds;
};

} // namespace gf::gui::apt_editor
