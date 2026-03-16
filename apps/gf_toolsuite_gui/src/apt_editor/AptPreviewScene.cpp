#include "AptPreviewScene.hpp"

#include "AptPreviewItem.hpp"
#include "AptSelectionManager.hpp"

#include <gf/apt/apt_renderer.hpp>

#include <QCursor>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace gf::gui::apt_editor {
namespace {

QTransform toQtTransform(const gf::apt::AptTransform& t) {
  return QTransform(t.scale_x,
                    t.rotate_skew_0,
                    t.rotate_skew_1,
                    t.scale_y,
                    t.x,
                    t.y);
}

QRectF boundsFromApt(const std::optional<gf::apt::AptBounds>& bounds) {
  if (!bounds) return QRectF(0.0, 0.0, 120.0, 48.0);
  const auto& b = *bounds;
  const qreal w = static_cast<qreal>(b.right - b.left);
  const qreal h = static_cast<qreal>(b.bottom - b.top);
  if (w <= 0.0 || h <= 0.0) return QRectF(0.0, 0.0, 120.0, 48.0);
  return QRectF(static_cast<qreal>(b.left), static_cast<qreal>(b.top), w, h);
}

QRectF unionRect(const QRectF& lhs, const QRectF& rhs) {
  if (!lhs.isValid()) return rhs;
  if (!rhs.isValid()) return lhs;
  return lhs.united(rhs);
}

} // namespace

AptTransformHandle::AptTransformHandle(AptPreviewScene* scene, Mode mode, QGraphicsItem* parent)
    : QGraphicsObject(parent), m_editorScene(scene), m_mode(mode) {
  setAcceptedMouseButtons(Qt::LeftButton);
  setCursor(QCursor(mode == Mode::Move ? Qt::SizeAllCursor
                         : (mode == Mode::Scale ? Qt::SizeFDiagCursor : Qt::CrossCursor)));
  setZValue(5000.0);
}

QRectF AptTransformHandle::boundingRect() const {
  return QRectF(-6.0, -6.0, 12.0, 12.0);
}

void AptTransformHandle::paint(QPainter* painter,
                               const QStyleOptionGraphicsItem*,
                               QWidget*) {
  if (!painter) return;
  painter->setRenderHint(QPainter::Antialiasing, true);
  QColor outline(255, 220, 96);
  QColor fill(255, 220, 96, 110);
  painter->setPen(QPen(outline, 1.5));
  painter->setBrush(fill);
  if (m_mode == Mode::Rotate) {
    painter->drawEllipse(boundingRect());
  } else if (m_mode == Mode::Move) {
    QPolygonF diamond;
    diamond << QPointF(0.0, -6.0) << QPointF(6.0, 0.0) << QPointF(0.0, 6.0) << QPointF(-6.0, 0.0);
    painter->drawPolygon(diamond);
  } else {
    painter->drawRect(boundingRect());
  }
}

void AptTransformHandle::mousePressEvent(QGraphicsSceneMouseEvent* event) {
  if (!event) return;
  m_pressScenePos = event->scenePos();
  if (m_editorScene && m_editorScene->selectedPreviewItem() && m_editorScene->selectedPreviewItem()->placement()) {
    m_editorScene->m_handleDragStartTransform = m_editorScene->selectedPreviewItem()->placement()->transform;
    const QRectF sceneBounds = m_editorScene->selectedPreviewItem()->sceneBoundingRect();
    m_editorScene->m_handleDragStartBounds = sceneBounds;
  }
  QGraphicsObject::mousePressEvent(event);
}

void AptTransformHandle::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
  if (m_editorScene && event) {
    m_editorScene->applyHandleDrag(m_mode, m_pressScenePos, event->scenePos(), false);
  }
  QGraphicsObject::mouseMoveEvent(event);
}

void AptTransformHandle::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
  if (m_editorScene && event) {
    m_editorScene->applyHandleDrag(m_mode, m_pressScenePos, event->scenePos(), true);
  }
  QGraphicsObject::mouseReleaseEvent(event);
}

AptPreviewScene::AptPreviewScene(QObject* parent)
    : QGraphicsScene(parent) {}

AptPreviewScene::~AptPreviewScene() = default;

void AptPreviewScene::setSelectionManager(AptSelectionManager* selectionManager) {
  if (m_selectionManager == selectionManager) return;
  if (m_selectionManager) disconnect(m_selectionManager, nullptr, this, nullptr);
  m_selectionManager = selectionManager;
  if (m_selectionManager) {
    connect(m_selectionManager, &AptSelectionManager::selectedPlacementChanged,
            this, [this](int idx) { syncSelectionFromExternal(idx); });
  }
}

void AptPreviewScene::setContext(Context context) {
  m_context = context;
  rebuildEditorOverlay();
}

void AptPreviewScene::clearEditorOverlay() {
  clearHandles();
  for (AptPreviewItem* item : m_previewItems) {
    removeItem(item);
    delete item;
  }
  m_previewItems.clear();
}

std::vector<gf::apt::AptPlacement>* AptPreviewScene::editablePlacements() const {
  if (!m_context.timelineFrames) return nullptr;
  if (m_context.frameIndex < 0 || m_context.frameIndex >= static_cast<int>(m_context.timelineFrames->size())) return nullptr;
  return &(*m_context.timelineFrames)[static_cast<std::size_t>(m_context.frameIndex)].placements;
}

gf::apt::AptPlacement* AptPreviewScene::editablePlacementAt(int placementIndex) const {
  auto* placements = editablePlacements();
  if (!placements) return nullptr;
  if (placementIndex < 0 || placementIndex >= static_cast<int>(placements->size())) return nullptr;
  return &(*placements)[static_cast<std::size_t>(placementIndex)];
}

std::vector<gf::apt::RenderNode> AptPreviewScene::currentRenderNodes() const {
  if (!m_context.file || !m_context.timelineFrames || !m_context.characterTable) return {};
  gf::apt::RenderOptions opts;
  opts.collectParentChain = m_context.debugOverlay;
  opts.maxRecursionDepth = 8;
  return gf::apt::renderAptTimelineFrame(*m_context.file,
                                         *m_context.timelineFrames,
                                         static_cast<std::size_t>(std::max(0, m_context.frameIndex)),
                                         *m_context.characterTable,
                                         gf::apt::Transform2D::identity(),
                                         opts);
}

QRectF AptPreviewScene::localBoundsForPlacement(int placementIndex,
                                                const std::vector<gf::apt::RenderNode>& nodes) const {
  gf::apt::AptPlacement* placement = editablePlacementAt(placementIndex);
  if (!placement) return QRectF(0.0, 0.0, 120.0, 48.0);

  QRectF local;
  bool found = false;
  const QTransform itemWorld = toQtTransform(placement->transform);
  const QTransform inv = itemWorld.inverted();
  for (const auto& node : nodes) {
    if (node.rootPlacementIndex != placementIndex) continue;
    QRectF nodeLocal = boundsFromApt(node.localBounds);
    const QTransform nodeWorld(node.worldTransform.a,
                               node.worldTransform.b,
                               node.worldTransform.c,
                               node.worldTransform.d,
                               node.worldTransform.tx,
                               node.worldTransform.ty);
    const QRectF nodeScene = nodeWorld.mapRect(nodeLocal);
    const QRectF backToItem = inv.mapRect(nodeScene);
    local = found ? unionRect(local, backToItem) : backToItem;
    found = true;
  }
  return found ? local : QRectF(0.0, 0.0, 120.0, 48.0);
}

void AptPreviewScene::rebuildEditorOverlay() {
  clearEditorOverlay();
  auto* placements = editablePlacements();
  if (!placements || !m_context.file) return;

  const auto nodes = currentRenderNodes();
  m_previewItems.reserve(placements->size());
  for (int i = 0; i < static_cast<int>(placements->size()); ++i) {
    auto* previewItem = new AptPreviewItem(this,
                                           &(*placements)[static_cast<std::size_t>(i)],
                                           i,
                                           localBoundsForPlacement(i, nodes));
    addItem(previewItem);
    m_previewItems.push_back(previewItem);
  }
  reindexPlacementItems();
  updateSelectionVisuals();
}

void AptPreviewScene::syncSelectionFromExternal(int placementIndex) {
  selectPlacementInternal(placementIndex, true);
}

void AptPreviewScene::requestSelectionFromScene(int placementIndex) {
  selectPlacementInternal(placementIndex, false);
}

int AptPreviewScene::selectedPlacementIndex() const noexcept {
  return m_selectionManager ? m_selectionManager->selectedPlacementIndex() : -1;
}

AptPreviewItem* AptPreviewScene::selectedPreviewItem() const noexcept {
  const int idx = selectedPlacementIndex();
  if (idx < 0 || idx >= static_cast<int>(m_previewItems.size())) return nullptr;
  return m_previewItems[static_cast<std::size_t>(idx)];
}

bool AptPreviewScene::bringSelectionForward() {
  const int idx = selectedPlacementIndex();
  auto* placements = editablePlacements();
  if (!placements || idx < 0 || idx + 1 >= static_cast<int>(placements->size())) return false;
  swapPlacements(idx, idx + 1);
  if (m_selectionManager) m_selectionManager->setSelectedPlacementIndex(idx + 1);
  rebuildEditorOverlay();
  if (onPlacementEdited) onPlacementEdited(idx + 1, false);
  return true;
}

bool AptPreviewScene::sendSelectionBackward() {
  const int idx = selectedPlacementIndex();
  auto* placements = editablePlacements();
  if (!placements || idx <= 0 || idx >= static_cast<int>(placements->size())) return false;
  swapPlacements(idx, idx - 1);
  if (m_selectionManager) m_selectionManager->setSelectedPlacementIndex(idx - 1);
  rebuildEditorOverlay();
  if (onPlacementEdited) onPlacementEdited(idx - 1, false);
  return true;
}

bool AptPreviewScene::removeSelection() {
  const int idx = selectedPlacementIndex();
  auto* placements = editablePlacements();
  if (!placements || idx < 0 || idx >= static_cast<int>(placements->size())) return false;
  placements->erase(placements->begin() + idx);
  if (m_context.frameIndex >= 0 && m_context.frameIndex < static_cast<int>(m_context.timelineFrames->size())) {
    auto& frame = (*m_context.timelineFrames)[static_cast<std::size_t>(m_context.frameIndex)];
    if (idx < static_cast<int>(frame.items.size())) {
      auto it = std::find_if(frame.items.begin(), frame.items.end(), [idx, count = 0](const gf::apt::AptFrameItem& item) mutable {
        if (item.kind != gf::apt::AptFrameItemKind::PlaceObject) return false;
        const bool match = count == idx;
        ++count;
        return match;
      });
      if (it != frame.items.end()) frame.items.erase(it);
    }
  }
  if (m_selectionManager) m_selectionManager->clearSelection();
  rebuildEditorOverlay();
  if (onPlacementEdited) onPlacementEdited(-1, false);
  return true;
}

bool AptPreviewScene::duplicateSelection() {
  const int idx = selectedPlacementIndex();
  auto* placements = editablePlacements();
  if (!placements || idx < 0 || idx >= static_cast<int>(placements->size())) return false;
  gf::apt::AptPlacement copy = (*placements)[static_cast<std::size_t>(idx)];
  copy.depth += 1;
  copy.transform.x += 12.0;
  copy.transform.y += 12.0;
  placements->insert(placements->begin() + idx + 1, copy);
  syncEditedPlacementIntoFrameItems(idx + 1);
  rebuildEditorOverlay();
  if (m_selectionManager) m_selectionManager->setSelectedPlacementIndex(idx + 1);
  if (onPlacementEdited) onPlacementEdited(idx + 1, false);
  return true;
}

bool AptPreviewScene::addPlacement() {
  auto* placements = editablePlacements();
  if (!placements) return false;
  gf::apt::AptPlacement placement;
  placement.depth = placements->empty() ? 0u : ((*placements).back().depth + 1u);
  placements->push_back(placement);
  syncEditedPlacementIntoFrameItems(static_cast<int>(placements->size()) - 1);
  rebuildEditorOverlay();
  if (m_selectionManager) m_selectionManager->setSelectedPlacementIndex(static_cast<int>(placements->size()) - 1);
  if (onPlacementEdited) onPlacementEdited(static_cast<int>(placements->size()) - 1, false);
  return true;
}

void AptPreviewScene::notifyPlacementChanged(AptPreviewItem* item, bool interactive) {
  if (!item) return;
  syncEditedPlacementIntoFrameItems(item->placementIndex());
  rebuildHandles();
  if (onPlacementEdited) onPlacementEdited(item->placementIndex(), interactive);
}

void AptPreviewScene::applyHandleDrag(AptTransformHandle::Mode mode,
                                      const QPointF& pressScenePos,
                                      const QPointF& currentScenePos,
                                      bool finished) {
  auto* item = selectedPreviewItem();
  if (!item || !item->placement()) return;
  auto& tr = item->placement()->transform;
  tr = m_handleDragStartTransform;

  if (mode == AptTransformHandle::Mode::Move) {
    const QPointF delta = currentScenePos - pressScenePos;
    tr.x += delta.x();
    tr.y += delta.y();
  } else if (mode == AptTransformHandle::Mode::Scale) {
    const QPointF startVec = pressScenePos - m_handleDragStartBounds.center();
    const QPointF nowVec = currentScenePos - m_handleDragStartBounds.center();
    const double startLenX = std::max(1.0, std::abs(startVec.x()));
    const double startLenY = std::max(1.0, std::abs(startVec.y()));
    tr.scale_x *= nowVec.x() / startLenX;
    tr.scale_y *= nowVec.y() / startLenY;
    if (std::abs(tr.scale_x) < 0.01) tr.scale_x = (tr.scale_x < 0.0 ? -0.01 : 0.01);
    if (std::abs(tr.scale_y) < 0.01) tr.scale_y = (tr.scale_y < 0.0 ? -0.01 : 0.01);
  } else {
    const QPointF center = m_handleDragStartBounds.center();
    const double startAngle = std::atan2(pressScenePos.y() - center.y(), pressScenePos.x() - center.x());
    const double currentAngle = std::atan2(currentScenePos.y() - center.y(), currentScenePos.x() - center.x());
    const double delta = currentAngle - startAngle;
    const double cosv = std::cos(delta);
    const double sinv = std::sin(delta);
    tr.scale_x = m_handleDragStartTransform.scale_x * cosv;
    tr.rotate_skew_0 = m_handleDragStartTransform.scale_x * sinv;
    tr.rotate_skew_1 = -m_handleDragStartTransform.scale_y * sinv;
    tr.scale_y = m_handleDragStartTransform.scale_y * cosv;
  }

  item->syncFromPlacement();
  notifyPlacementChanged(item, !finished);
}

void AptPreviewScene::updateSelectionVisuals() {
  const int selectedIdx = selectedPlacementIndex();
  for (std::size_t i = 0; i < m_previewItems.size(); ++i) {
    auto* item = m_previewItems[i];
    const bool selected = static_cast<int>(i) == selectedIdx;
    item->setSelected(selected);
    item->setEditorSelected(selected);
  }
  rebuildHandles();
}

void AptPreviewScene::rebuildHandles() {
  clearHandles();
  AptPreviewItem* item = selectedPreviewItem();
  if (!item) return;

  const QRectF sceneBounds = item->sceneBoundingRect();
  m_handles.move = new AptTransformHandle(this, AptTransformHandle::Mode::Move);
  m_handles.scale = new AptTransformHandle(this, AptTransformHandle::Mode::Scale);
  m_handles.rotate = new AptTransformHandle(this, AptTransformHandle::Mode::Rotate);
  addItem(m_handles.move);
  addItem(m_handles.scale);
  addItem(m_handles.rotate);
  m_handles.move->setPos(sceneBounds.center());
  m_handles.scale->setPos(sceneBounds.bottomRight());
  m_handles.rotate->setPos(QPointF(sceneBounds.center().x(), sceneBounds.top() - 18.0));
}

void AptPreviewScene::clearHandles() {
  for (QGraphicsObject* handle : { static_cast<QGraphicsObject*>(m_handles.move),
                                   static_cast<QGraphicsObject*>(m_handles.scale),
                                   static_cast<QGraphicsObject*>(m_handles.rotate) }) {
    if (!handle) continue;
    removeItem(handle);
    delete handle;
  }
  m_handles = {};
}

void AptPreviewScene::selectPlacementInternal(int placementIndex, bool fromExternal) {
  if (m_selectionManager && m_selectionManager->selectedPlacementIndex() != placementIndex) {
    m_selectionManager->setSelectedPlacementIndex(placementIndex);
  }
  updateSelectionVisuals();
  emit sceneSelectionChanged(placementIndex);
  if (!fromExternal && onPlacementSelected) onPlacementSelected(placementIndex);
}

void AptPreviewScene::reindexPlacementItems() {
  for (std::size_t i = 0; i < m_previewItems.size(); ++i) {
    if (m_previewItems[i]) m_previewItems[i]->setZValue(2000.0 + static_cast<qreal>(i));
  }
}

void AptPreviewScene::syncEditedPlacementIntoFrameItems(int placementIndex) {
  auto* placements = editablePlacements();
  if (!placements || !m_context.timelineFrames) return;
  if (m_context.frameIndex < 0 || m_context.frameIndex >= static_cast<int>(m_context.timelineFrames->size())) return;
  auto& frame = (*m_context.timelineFrames)[static_cast<std::size_t>(m_context.frameIndex)];
  int seenPlaceObjects = -1;
  for (auto& item : frame.items) {
    if (item.kind != gf::apt::AptFrameItemKind::PlaceObject) continue;
    ++seenPlaceObjects;
    if (seenPlaceObjects == placementIndex) {
      item.placement = (*placements)[static_cast<std::size_t>(placementIndex)];
      return;
    }
  }
  gf::apt::AptFrameItem newItem;
  newItem.kind = gf::apt::AptFrameItemKind::PlaceObject;
  newItem.placement = (*placements)[static_cast<std::size_t>(placementIndex)];
  frame.items.push_back(newItem);
  frame.frameitemcount = static_cast<std::uint32_t>(frame.items.size());
}

void AptPreviewScene::swapPlacements(int lhs, int rhs) {
  auto* placements = editablePlacements();
  if (!placements) return;
  std::swap((*placements)[static_cast<std::size_t>(lhs)], (*placements)[static_cast<std::size_t>(rhs)]);
  (*placements)[static_cast<std::size_t>(lhs)].depth = static_cast<std::uint32_t>(lhs);
  (*placements)[static_cast<std::size_t>(rhs)].depth = static_cast<std::uint32_t>(rhs);
  syncEditedPlacementIntoFrameItems(lhs);
  syncEditedPlacementIntoFrameItems(rhs);
}

} // namespace gf::gui::apt_editor
