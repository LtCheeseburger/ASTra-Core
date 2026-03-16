#pragma once

#include <QDialog>
#include <gf/core/AstContainerBuilder.hpp>

class QLineEdit;
class QLabel;
class QTableWidget;
class QTableWidgetItem;
class QPushButton;

namespace gf::gui {

class CreateDlcAstDialog final : public QDialog {
  Q_OBJECT
public:
  explicit CreateDlcAstDialog(QWidget* parent = nullptr);

private slots:
  void onBrowseDonor();
  void onBrowseOutput();
  void onAddFiles();
  void onAddFolder();
  void onRemoveSelected();
  void onClearAll();
  void onBuild();
  void onHowToUse();
  void refreshTable();
  void onTableItemChanged(QTableWidgetItem* item);

private:
  void appendLogLine(const QString& text);
  bool ensureProfileLoaded();
  void refreshDonorSummary();

  gf::core::AstContainerBuilder m_builder;
  QLineEdit* m_donorEdit = nullptr;
  QLineEdit* m_outputEdit = nullptr;
  QTableWidget* m_entriesTable = nullptr;
  QLabel* m_donorSummaryLabel = nullptr;
  QLabel* m_statusLabel = nullptr;
  QPushButton* m_buildButton = nullptr;
};

} // namespace gf::gui
