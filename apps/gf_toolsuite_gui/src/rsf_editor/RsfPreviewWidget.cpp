#include "RsfPreviewWidget.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGraphicsEllipseItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtMath>

namespace gf::gui::rsf_editor {
using gf::models::rsf::preview_transform;

RsfGraphicsView::RsfGraphicsView(QWidget* parent) : QGraphicsView(parent) {
  setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
  setDragMode(QGraphicsView::ScrollHandDrag);
}

void RsfGraphicsView::wheelEvent(QWheelEvent* event) {
  const double factor = event->angleDelta().y() > 0 ? 1.15 : (1.0 / 1.15);
  scale(factor, factor);
  event->accept();
}

RsfPreviewWidget::RsfPreviewWidget(QWidget* parent) : QWidget(parent) {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(4);

  m_toolbar = new QToolBar(this);
  m_modeCombo = new QComboBox(m_toolbar);
  m_modeCombo->addItems({"Proxy", "Textured", "UV", "Wireframe"});
  m_toolbar->addWidget(new QLabel("Mode", m_toolbar));
  m_toolbar->addWidget(m_modeCombo);
  m_gridAction = m_toolbar->addAction("Grid");
  m_gridAction->setCheckable(true);
  m_gridAction->setChecked(true);
  m_boundsAction = m_toolbar->addAction("Bounds");
  m_boundsAction->setCheckable(true);
  m_boundsAction->setChecked(true);
  m_fitAction = m_toolbar->addAction("Fit");
  m_resetViewAction = m_toolbar->addAction("Reset View");
  outer->addWidget(m_toolbar);

  m_bannerLabel = new QLabel(this);
  m_bannerLabel->setWordWrap(true);
  outer->addWidget(m_bannerLabel);

  auto* split = new QHBoxLayout();
  split->setContentsMargins(0, 0, 0, 0);
  split->setSpacing(4);
  outer->addLayout(split, 1);

  m_scene = new QGraphicsScene(this);
  m_view = new RsfGraphicsView(this);
  m_view->setScene(m_scene);
  split->addWidget(m_view, 3);

  auto* side = new QVBoxLayout();
  split->addLayout(side, 1);

  side->addWidget(new QLabel("Objects", this));
  m_objectList = new QListWidget(this);
  side->addWidget(m_objectList, 1);

  auto* form = new QFormLayout();
  m_xSpin = new QDoubleSpinBox(this);
  m_ySpin = new QDoubleSpinBox(this);
  m_rotSpin = new QDoubleSpinBox(this);
  m_scaleXSpin = new QDoubleSpinBox(this);
  m_scaleYSpin = new QDoubleSpinBox(this);
  for (QDoubleSpinBox* spin : {m_xSpin, m_ySpin, m_rotSpin, m_scaleXSpin, m_scaleYSpin}) {
    spin->setRange(-99999.0, 99999.0);
    spin->setDecimals(3);
    spin->setSingleStep(0.25);
  }
  m_scaleXSpin->setValue(1.0);
  m_scaleYSpin->setValue(1.0);
  form->addRow("X", m_xSpin);
  form->addRow("Y", m_ySpin);
  form->addRow("Rotation", m_rotSpin);
  form->addRow("Scale X", m_scaleXSpin);
  form->addRow("Scale Y", m_scaleYSpin);
  side->addLayout(form);

  m_resetTransformButton = new QPushButton("Reset Transform", this);
  side->addWidget(m_resetTransformButton);

  m_textureStatusLabel = new QLabel(this);
  m_textureStatusLabel->setWordWrap(true);
  side->addWidget(m_textureStatusLabel);

  connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { rebuildScene(); });
  connect(m_gridAction, &QAction::toggled, this, [this](bool) { rebuildScene(); });
  connect(m_boundsAction, &QAction::toggled, this, [this](bool) { rebuildScene(); });
  connect(m_fitAction, &QAction::triggered, this, [this]() { fitView(); });
  connect(m_resetViewAction, &QAction::triggered, this, [this]() { resetView(); });
  connect(m_objectList, &QListWidget::currentRowChanged, this, [this](int row) { selectObject(row); });

  auto emitTransform = [this](bool interactive) {
    if (m_updatingUi || !m_doc) return;
    const int idx = currentObjectIndex();
    if (idx < 0 || idx >= static_cast<int>(m_doc->objects.size())) return;
    preview_transform tr;
    tr.x = static_cast<float>(m_xSpin->value());
    tr.y = static_cast<float>(m_ySpin->value());
    tr.rotation_deg = static_cast<float>(m_rotSpin->value());
    tr.scale_x = static_cast<float>(m_scaleXSpin->value());
    tr.scale_y = static_cast<float>(m_scaleYSpin->value());
    tr.scale_z = 1.0f;
    emit transformEdited(m_doc->objects[static_cast<std::size_t>(idx)].material_index, tr, interactive);
  };
  connect(m_xSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [emitTransform](double) { emitTransform(true); });
  connect(m_ySpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [emitTransform](double) { emitTransform(true); });
  connect(m_rotSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [emitTransform](double) { emitTransform(true); });
  connect(m_scaleXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [emitTransform](double) { emitTransform(true); });
  connect(m_scaleYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [emitTransform](double) { emitTransform(true); });
  connect(m_resetTransformButton, &QPushButton::clicked, this, [this]() {
    if (!m_doc) return;
    const int idx = currentObjectIndex();
    if (idx < 0 || idx >= static_cast<int>(m_doc->objects.size())) return;
    const auto tr = m_doc->objects[static_cast<std::size_t>(idx)].original_transform;
    pushSpinboxValues(tr);
    emit transformEdited(m_doc->objects[static_cast<std::size_t>(idx)].material_index, tr, false);
  });
}

void RsfPreviewWidget::setDocument(const gf::models::rsf::preview_document& doc) {
  m_doc = doc;
  rebuildScene();
}

void RsfPreviewWidget::clear() {
  m_doc.reset();
  m_texturePixmaps.clear();
  m_itemsByMaterialIndex.clear();
  m_scene->clear();
  m_objectList->clear();
  m_bannerLabel->clear();
  m_textureStatusLabel->clear();
}

void RsfPreviewWidget::setTexturePixmap(int textureIndex, const QPixmap& pixmap) {
  m_texturePixmaps.insert(textureIndex, pixmap);
  rebuildScene();
}

void RsfPreviewWidget::setTextureStatus(const QString& text) { m_textureStatusLabel->setText(text); }

void RsfPreviewWidget::setSelectionByMaterialIndex(int materialIndex) {
  if (!m_doc) return;
  for (int i = 0; i < static_cast<int>(m_doc->objects.size()); ++i) {
    if (m_doc->objects[static_cast<std::size_t>(i)].material_index == materialIndex) {
      QSignalBlocker blocker(m_objectList);
      m_objectList->setCurrentRow(i);
      blocker.unblock();
      selectObject(i);
      return;
    }
  }
}

void RsfPreviewWidget::updateObjectTransform(int materialIndex, const preview_transform& transform) {
  if (!m_doc) return;
  for (auto& obj : m_doc->objects) {
    if (obj.material_index == materialIndex) {
      obj.transform = transform;
      break;
    }
  }
  rebuildScene();
  setSelectionByMaterialIndex(materialIndex);
}

void RsfPreviewWidget::fitView() {
  if (!m_scene) return;
  const QRectF bounds = m_scene->itemsBoundingRect();
  if (!bounds.isValid() || bounds.isEmpty()) return;
  m_view->fitInView(bounds.adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
}

void RsfPreviewWidget::resetView() {
  m_view->resetTransform();
  fitView();
}

int RsfPreviewWidget::currentObjectIndex() const { return m_objectList ? m_objectList->currentRow() : -1; }

QPixmap RsfPreviewWidget::buildCheckerboard(const QSize& size) const {
  QPixmap pm(size);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  for (int y = 0; y < size.height(); y += 16) {
    for (int x = 0; x < size.width(); x += 16) {
      const bool dark = ((x / 16) + (y / 16)) % 2 == 0;
      p.fillRect(x, y, 16, 16, dark ? QColor(80, 80, 80) : QColor(120, 120, 120));
    }
  }
  return pm;
}

bool RsfPreviewWidget::shouldRenderMesh() const {
  if (!m_doc) return false;
  const int mode = m_modeCombo->currentIndex();
  return (mode == static_cast<int>(PreviewMode::Wireframe) || mode == static_cast<int>(PreviewMode::UV)) &&
         m_doc->geometry.selected_candidate >= 0 &&
         m_doc->geometry.selected_candidate < static_cast<int>(m_doc->geometry.candidates.size());
}

void RsfPreviewWidget::renderMeshCandidate(const gf::models::rsf::geometry_candidate& candidate) {
  const auto& mesh = candidate.mesh;
  if (mesh.positions.empty()) return;

  const bool uvMode = m_modeCombo->currentIndex() == static_cast<int>(PreviewMode::UV);
  const QRectF target(-256.0, -256.0, 512.0, 512.0);
  const float sx = std::max(0.0001f, mesh.bounds_max.x - mesh.bounds_min.x);
  const float sy = std::max(0.0001f, mesh.bounds_max.y - mesh.bounds_min.y);
  const float sz = std::max(0.0001f, mesh.bounds_max.z - mesh.bounds_min.z);
  const float scale = 460.0f / std::max({sx, sy, sz});

  auto project = [&](std::size_t i) {
    if (uvMode && mesh.has_uvs && i < mesh.uvs.size()) {
      const auto uv = mesh.uvs[i];
      return QPointF(target.left() + (uv.x * target.width()), target.top() + ((1.0 - uv.y) * target.height()));
    }
    const auto& p = mesh.positions[i];
    const double x = (double(p.x - mesh.bounds_min.x) * scale) - 230.0;
    const double y = 230.0 - (double(p.y - mesh.bounds_min.y) * scale);
    return QPointF(x, y);
  };

  const QPen linePen(QColor(0, 220, 255, 230), 1.25);
  const QPen pointPen(QColor(255, 200, 0, 230), 1.0);

  if (mesh.indexed && mesh.indices.size() >= 3) {
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
      const auto a = mesh.indices[i + 0];
      const auto b = mesh.indices[i + 1];
      const auto c = mesh.indices[i + 2];
      if (a >= mesh.positions.size() || b >= mesh.positions.size() || c >= mesh.positions.size()) continue;
      const QPointF pa = project(a);
      const QPointF pb = project(b);
      const QPointF pc = project(c);
      m_scene->addLine(QLineF(pa, pb), linePen);
      m_scene->addLine(QLineF(pb, pc), linePen);
      m_scene->addLine(QLineF(pc, pa), linePen);
    }
  } else {
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
      const QPointF p = project(i);
      m_scene->addEllipse(QRectF(p.x() - 1.25, p.y() - 1.25, 2.5, 2.5), pointPen, QBrush(QColor(255, 200, 0, 190)));
    }
  }

  if (m_boundsAction->isChecked()) {
    m_scene->addRect(target, QPen(QColor(0, 255, 255, 110), 1.0, Qt::DashLine));
  }
}

void RsfPreviewWidget::rebuildScene() {
  m_scene->clear();
  m_itemsByMaterialIndex.clear();
  m_objectList->clear();
  if (!m_doc) return;

  QStringList bannerLines;
  if (m_doc->partial_decode) bannerLines << "Partial decode mode: rendering proxy objects from RSF materials/texture bindings.";
  for (const auto& w : m_doc->warnings) bannerLines << QString::fromStdString(w);
  for (const auto& w : m_doc->geometry.warnings) bannerLines << QString::fromStdString(w.message);
  if (m_doc->geometry.selected_candidate >= 0 && m_doc->geometry.selected_candidate < static_cast<int>(m_doc->geometry.candidates.size())) {
    const auto& c = m_doc->geometry.candidates[static_cast<std::size_t>(m_doc->geometry.selected_candidate)];
    bannerLines << QString("Using candidate 0x%1 stride=%2 verts=%3 idx=%4 uv=%5 confidence=%6")
                    .arg(static_cast<qulonglong>(c.vertex_offset), 0, 16)
                    .arg(c.stride_guess)
                    .arg(static_cast<qulonglong>(c.emitted_vertices))
                    .arg(static_cast<qulonglong>(c.emitted_indices))
                    .arg(c.mesh.has_uvs ? "yes" : "no")
                    .arg(c.confidence, 0, 'f', 2);
  }
  m_bannerLabel->setText(bannerLines.join("\n"));

  if (m_gridAction->isChecked()) {
    QPen gridPen(QColor(60, 60, 60, 90));
    for (int x = -2000; x <= 2000; x += 64) m_scene->addLine(x, -2000, x, 2000, gridPen);
    for (int y = -2000; y <= 2000; y += 64) m_scene->addLine(-2000, y, 2000, y, gridPen);
  }

  for (const auto& obj : m_doc->objects) {
    m_objectList->addItem(QString::fromStdString(obj.label));
  }

  if (shouldRenderMesh()) {
    const auto& c = m_doc->geometry.candidates[static_cast<std::size_t>(m_doc->geometry.selected_candidate)];
    renderMeshCandidate(c);
  } else {
    const bool textured = m_modeCombo->currentIndex() == static_cast<int>(PreviewMode::Textured) ||
                          m_modeCombo->currentIndex() == static_cast<int>(PreviewMode::UV);
    for (const auto& obj : m_doc->objects) {
      QRectF r(0.0, 0.0,
               qMax(32.0, static_cast<double>(obj.proxy_width * std::abs(obj.transform.scale_x))),
               qMax(32.0, static_cast<double>(obj.proxy_height * std::abs(obj.transform.scale_y))));
      auto* rect = m_scene->addRect(r,
                                    QPen(QColor(220, 220, 220, 180), 2.0),
                                    QBrush(QColor::fromRgbF(obj.material.color_r, obj.material.color_g, obj.material.color_b, obj.material.alpha)));
      rect->setPos(obj.transform.x, obj.transform.y);
      rect->setRotation(obj.transform.rotation_deg);
      rect->setData(0, obj.material_index);
      rect->setFlag(QGraphicsItem::ItemIsSelectable, true);
      m_itemsByMaterialIndex.insert(obj.material_index, rect);

      if (textured) {
        QPixmap pm;
        if (obj.material.texture_index && m_texturePixmaps.contains(static_cast<int>(*obj.material.texture_index))) {
          pm = m_texturePixmaps.value(static_cast<int>(*obj.material.texture_index));
        } else {
          pm = buildCheckerboard(QSize(static_cast<int>(r.width()), static_cast<int>(r.height())));
        }
        auto* pix = m_scene->addPixmap(pm.scaled(r.size().toSize(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        pix->setPos(obj.transform.x, obj.transform.y);
        pix->setRotation(obj.transform.rotation_deg);
        pix->setOpacity(obj.material.alpha);
      }

      auto* text = m_scene->addSimpleText(QString::fromStdString(obj.label));
      text->setPos(obj.transform.x + 6.0, obj.transform.y + 6.0);
      text->setBrush(QBrush(QColor(230, 230, 230)));

      if (m_boundsAction->isChecked()) {
        const QRectF bounds = rect->sceneBoundingRect();
        m_scene->addRect(bounds, QPen(QColor(0, 255, 255, 110), 1.0, Qt::DashLine));
      }
    }
  }

  if (!m_doc->objects.empty()) {
    if (m_objectList->currentRow() < 0) m_objectList->setCurrentRow(0);
    syncSelectionToList();
  }
  fitView();
}

void RsfPreviewWidget::refreshInspector() {
  if (!m_doc) return;
  const int idx = currentObjectIndex();
  if (idx < 0 || idx >= static_cast<int>(m_doc->objects.size())) return;
  pushSpinboxValues(m_doc->objects[static_cast<std::size_t>(idx)].transform);

  QString tex;
  const auto& mat = m_doc->objects[static_cast<std::size_t>(idx)].material;
  if (mat.texture_index) {
    tex = QString("Texture #%1\n%2\n%3").arg(*mat.texture_index).arg(QString::fromStdString(mat.texture_name), QString::fromStdString(mat.texture_filename));
  } else {
    tex = "No resolved texture binding.";
  }
  if (m_doc->geometry.selected_candidate >= 0 && m_doc->geometry.selected_candidate < static_cast<int>(m_doc->geometry.candidates.size())) {
    const auto& c = m_doc->geometry.candidates[static_cast<std::size_t>(m_doc->geometry.selected_candidate)];
    tex += QString("\n\nMesh candidate\nsource=%1\nstride=%2\nverts=%3\nindices=%4\nconfidence=%5")
             .arg(QString::fromStdString(c.source_kind))
             .arg(c.stride_guess)
             .arg(static_cast<qulonglong>(c.emitted_vertices))
             .arg(static_cast<qulonglong>(c.emitted_indices))
             .arg(c.confidence, 0, 'f', 2);
  }
  m_textureStatusLabel->setText(tex);
}

void RsfPreviewWidget::pushSpinboxValues(const preview_transform& tr) {
  m_updatingUi = true;
  m_xSpin->setValue(tr.x);
  m_ySpin->setValue(tr.y);
  m_rotSpin->setValue(tr.rotation_deg);
  m_scaleXSpin->setValue(tr.scale_x);
  m_scaleYSpin->setValue(tr.scale_y);
  m_updatingUi = false;
}

void RsfPreviewWidget::syncSelectionToList() {
  refreshInspector();
  const int idx = currentObjectIndex();
  if (!m_doc || idx < 0 || idx >= static_cast<int>(m_doc->objects.size())) return;
  const int materialIndex = m_doc->objects[static_cast<std::size_t>(idx)].material_index;
  for (auto it = m_itemsByMaterialIndex.begin(); it != m_itemsByMaterialIndex.end(); ++it) {
    if (it.value()) {
      const bool selected = it.key() == materialIndex;
      it.value()->setPen(QPen(selected ? QColor(255, 196, 0) : QColor(220, 220, 220, 180), selected ? 4.0 : 2.0));
      it.value()->setBrush(selected ? QBrush(QColor(255, 255, 255, 40)) : it.value()->brush());
    }
  }
  emit materialSelectionChanged(materialIndex);
}

void RsfPreviewWidget::selectObject(int index) {
  if (!m_doc || index < 0 || index >= static_cast<int>(m_doc->objects.size())) return;
  syncSelectionToList();
}

} // namespace gf::gui::rsf_editor
