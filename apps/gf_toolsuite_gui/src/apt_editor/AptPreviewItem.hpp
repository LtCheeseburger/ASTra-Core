#pragma once

#include <gf/apt/apt_reader.hpp>
#include <gf/apt/apt_transform.hpp>

#include <QGraphicsObject>
#include <QPointer>
#include <QRectF>

namespace gf::gui::apt_editor {

class AptPreviewScene;

class AptPreviewItem final : public QGraphicsObject {
  Q_OBJECT
public:
  AptPreviewItem(AptPreviewScene* scene,
                 gf::apt::AptPlacement* placement,
                 int placementIndex,
                 const QRectF& localBounds,
                 QGraphicsItem* parent = nullptr);

  [[nodiscard]] QRectF boundingRect() const override;
  void paint(QPainter* painter,
             const QStyleOptionGraphicsItem* option,
             QWidget* widget) override;

  [[nodiscard]] int placementIndex() const noexcept { return m_placementIndex; }
  [[nodiscard]] gf::apt::AptPlacement* placement() const noexcept { return m_placement; }
  [[nodiscard]] QRectF localBounds() const noexcept { return m_localBounds; }

  void syncFromPlacement();
  void setEditorSelected(bool selected);

protected:
  QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;
  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
  void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
  void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
  static QTransform toQtTransform(const gf::apt::AptTransform& t);

  QPointer<AptPreviewScene> m_editorScene;
  gf::apt::AptPlacement* m_placement = nullptr;
  int m_placementIndex = -1;
  QRectF m_localBounds;
  QPointF m_dragStartScenePos;
  gf::apt::AptTransform m_dragStartTransform;
  bool m_editorSelected = false;
};

} // namespace gf::gui::apt_editor
