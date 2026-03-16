#include "AptPreviewItem.hpp"

#include "AptPreviewScene.hpp"

#include <QGraphicsSceneMouseEvent>
#include <QPainter>

namespace gf::gui::apt_editor {

AptPreviewItem::AptPreviewItem(AptPreviewScene* scene,
                               gf::apt::AptPlacement* placement,
                               int placementIndex,
                               const QRectF& localBounds,
                               QGraphicsItem* parent)
    : QGraphicsObject(parent),
      m_editorScene(scene),
      m_placement(placement),
      m_placementIndex(placementIndex),
      m_localBounds(localBounds) {
  setFlags(ItemIsSelectable | ItemSendsGeometryChanges);
  setAcceptedMouseButtons(Qt::LeftButton);
  setZValue(2000.0 + placementIndex);
  syncFromPlacement();
}

QRectF AptPreviewItem::boundingRect() const {
  return m_localBounds.adjusted(-8.0, -8.0, 8.0, 8.0);
}

void AptPreviewItem::paint(QPainter* painter,
                           const QStyleOptionGraphicsItem*,
                           QWidget*) {
  if (!painter) return;
  painter->setRenderHint(QPainter::Antialiasing, true);

  const QColor outline = m_editorSelected ? QColor(255, 220, 96) : QColor(90, 200, 255, 180);
  const QColor fill = m_editorSelected ? QColor(255, 220, 96, 30) : QColor(90, 200, 255, 16);
  painter->setPen(QPen(outline, m_editorSelected ? 2.0 : 1.25, Qt::DashLine));
  painter->setBrush(fill);
  painter->drawRect(m_localBounds);

  const QPointF origin(0.0, 0.0);
  painter->setPen(QPen(m_editorSelected ? QColor(255, 220, 96) : QColor(120, 255, 180), 1.5));
  painter->drawLine(QPointF(origin.x() - 6.0, origin.y()), QPointF(origin.x() + 6.0, origin.y()));
  painter->drawLine(QPointF(origin.x(), origin.y() - 6.0), QPointF(origin.x(), origin.y() + 6.0));
}

void AptPreviewItem::syncFromPlacement() {
  if (!m_placement) return;
  setTransform(toQtTransform(m_placement->transform));
  update();
}

void AptPreviewItem::setEditorSelected(bool selected) {
  if (m_editorSelected == selected) return;
  m_editorSelected = selected;
  update();
}

QVariant AptPreviewItem::itemChange(GraphicsItemChange change, const QVariant& value) {
  if (change == ItemSelectedHasChanged && m_editorScene) {
    m_editorScene->requestSelectionFromScene(value.toBool() ? m_placementIndex : -1);
  }
  return QGraphicsObject::itemChange(change, value);
}

void AptPreviewItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
  if (!event || !m_placement) return;
  m_dragStartScenePos = event->scenePos();
  m_dragStartTransform = m_placement->transform;
  if (m_editorScene) {
    m_editorScene->requestSelectionFromScene(m_placementIndex);
  }
  QGraphicsObject::mousePressEvent(event);
}

void AptPreviewItem::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
  if (!event || !m_placement || !m_editorScene) return;
  const QPointF delta = event->scenePos() - m_dragStartScenePos;
  m_placement->transform = m_dragStartTransform;
  m_placement->transform.x += delta.x();
  m_placement->transform.y += delta.y();
  syncFromPlacement();
  m_editorScene->notifyPlacementChanged(this, true);
  QGraphicsObject::mouseMoveEvent(event);
}

void AptPreviewItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
  if (m_editorScene) {
    m_editorScene->notifyPlacementChanged(this, false);
  }
  QGraphicsObject::mouseReleaseEvent(event);
}

QTransform AptPreviewItem::toQtTransform(const gf::apt::AptTransform& t) {
  return QTransform(t.scale_x,
                    t.rotate_skew_0,
                    t.rotate_skew_1,
                    t.scale_y,
                    t.x,
                    t.y);
}

} // namespace gf::gui::apt_editor
