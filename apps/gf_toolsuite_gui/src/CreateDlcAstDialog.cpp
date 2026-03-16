#include "CreateDlcAstDialog.hpp"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace gf::gui {

CreateDlcAstDialog::CreateDlcAstDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("Create DLC AST");
  resize(1080, 700);

  auto* root = new QVBoxLayout(this);
  auto* form = new QGridLayout();

  m_donorEdit = new QLineEdit(this);
  m_outputEdit = new QLineEdit(this);
  auto* donorBrowse = new QPushButton("Browse...", this);
  auto* outputBrowse = new QPushButton("Browse...", this);
  auto* howToUse = new QPushButton("How to Use", this);
  howToUse->setMaximumWidth(110);

  form->addWidget(new QLabel("Template / Donor AST", this), 0, 0);
  form->addWidget(m_donorEdit, 0, 1);
  form->addWidget(donorBrowse, 0, 2);
  form->addWidget(howToUse, 0, 3);
  form->addWidget(new QLabel("Output AST", this), 1, 0);
  form->addWidget(m_outputEdit, 1, 1);
  form->addWidget(outputBrowse, 1, 2);
  root->addLayout(form);

  auto* helper = new QLabel(
      "Template-driven builder only. Choose a donor AST, add files or a folder, review archive names, then build a DLC-ready AST.",
      this);
  helper->setWordWrap(true);
  root->addWidget(helper);

  m_donorSummaryLabel = new QLabel(this);
  m_donorSummaryLabel->setWordWrap(true);
  m_donorSummaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_donorSummaryLabel->setText("No donor profile loaded.");
  root->addWidget(m_donorSummaryLabel);

  m_entriesTable = new QTableWidget(this);
  m_entriesTable->setColumnCount(6);
  m_entriesTable->setHorizontalHeaderLabels({"Source", "Archive Name", "Type", "Compression", "Include", "Status"});
  m_entriesTable->horizontalHeader()->setStretchLastSection(true);
  m_entriesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_entriesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  root->addWidget(m_entriesTable, 1);

  auto* actions = new QHBoxLayout();
  auto* addFiles = new QPushButton("Add Files...", this);
  auto* addFolder = new QPushButton("Add Folder...", this);
  auto* removeFiles = new QPushButton("Remove Selected", this);
  auto* clearAll = new QPushButton("Clear All", this);
  m_statusLabel = new QLabel(this);
  m_statusLabel->setWordWrap(true);
  m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  actions->addWidget(addFiles);
  actions->addWidget(addFolder);
  actions->addWidget(removeFiles);
  actions->addWidget(clearAll);
  actions->addStretch(1);
  actions->addWidget(m_statusLabel, 1);
  root->addLayout(actions);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  m_buildButton = new QPushButton("Build AST", this);
  buttons->addButton(m_buildButton, QDialogButtonBox::AcceptRole);
  root->addWidget(buttons);

  connect(donorBrowse, &QPushButton::clicked, this, &CreateDlcAstDialog::onBrowseDonor);
  connect(outputBrowse, &QPushButton::clicked, this, &CreateDlcAstDialog::onBrowseOutput);
  connect(addFiles, &QPushButton::clicked, this, &CreateDlcAstDialog::onAddFiles);
  connect(addFolder, &QPushButton::clicked, this, &CreateDlcAstDialog::onAddFolder);
  connect(removeFiles, &QPushButton::clicked, this, &CreateDlcAstDialog::onRemoveSelected);
  connect(clearAll, &QPushButton::clicked, this, &CreateDlcAstDialog::onClearAll);
  connect(m_buildButton, &QPushButton::clicked, this, &CreateDlcAstDialog::onBuild);
  connect(howToUse, &QPushButton::clicked, this, &CreateDlcAstDialog::onHowToUse);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_entriesTable, &QTableWidget::itemChanged, this, &CreateDlcAstDialog::onTableItemChanged);

  refreshTable();
}

void CreateDlcAstDialog::appendLogLine(const QString& text) {
  const QString cur = m_statusLabel->text();
  m_statusLabel->setText(cur.isEmpty() ? text : (cur + "\n" + text));
}

void CreateDlcAstDialog::refreshDonorSummary() {
  if (!m_builder.profile().has_value()) {
    m_donorSummaryLabel->setText("No donor profile loaded.");
    return;
  }
  const auto& profile = *m_builder.profile();
  QString text = QString("Donor profile: %1\n%2")
      .arg(QString::fromStdString(profile.donor_display_name),
           QString::fromStdString(profile.donor_inspection_summary));
  m_donorSummaryLabel->setText(text);
}

bool CreateDlcAstDialog::ensureProfileLoaded() {
  std::string err;
  if (m_builder.profile().has_value() && m_builder.profile()->donor_path == m_donorEdit->text().toStdString()) {
    refreshDonorSummary();
    return true;
  }
  if (!m_builder.load_profile_from_donor(m_donorEdit->text().toStdString(), &err)) {
    QMessageBox::warning(this, "ASTra Core", QString("Failed to load donor AST:\n%1").arg(QString::fromStdString(err)));
    refreshDonorSummary();
    return false;
  }
  refreshDonorSummary();
  appendLogLine("Loaded donor profile from: " + m_donorEdit->text());
  return true;
}

void CreateDlcAstDialog::onBrowseDonor() {
  const QString path = QFileDialog::getOpenFileName(this, "Select Donor AST", {}, "AST Files (*.ast *.bgfa);;All Files (*.*)");
  if (path.isEmpty()) return;
  m_donorEdit->setText(path);
  m_builder.clear_entries();
  refreshDonorSummary();
  refreshTable();
}

void CreateDlcAstDialog::onBrowseOutput() {
  const QString path = QFileDialog::getSaveFileName(this, "Save DLC AST", {}, "AST Files (*.ast);;All Files (*.*)");
  if (!path.isEmpty()) m_outputEdit->setText(path);
  refreshTable();
}

void CreateDlcAstDialog::onAddFiles() {
  if (m_donorEdit->text().trimmed().isEmpty()) {
    QMessageBox::information(this, "ASTra Core", "Choose a donor/template AST first.");
    return;
  }
  if (!ensureProfileLoaded()) return;
  const QStringList paths = QFileDialog::getOpenFileNames(this, "Add Files To DLC AST", {}, "All Supported Files (*.*)");
  if (paths.isEmpty()) return;

  for (const QString& path : paths) {
    std::string err;
    auto entry = m_builder.make_entry_from_file(path.toStdString(), {}, gf::core::AstBuildEntry::CompressionMode::None, &err);
    if (!entry.has_value()) {
      appendLogLine(QString("Failed to import %1: %2").arg(path, QString::fromStdString(err)));
      continue;
    }
    m_builder.add_entry(std::move(*entry), &err);
    if (!err.empty()) appendLogLine(QString::fromStdString(err));
  }
  refreshTable();
}

void CreateDlcAstDialog::onAddFolder() {
  if (m_donorEdit->text().trimmed().isEmpty()) {
    QMessageBox::information(this, "ASTra Core", "Choose a donor/template AST first.");
    return;
  }
  if (!ensureProfileLoaded()) return;
  const QString folder = QFileDialog::getExistingDirectory(this, "Add Folder To DLC AST");
  if (folder.isEmpty()) return;

  std::string err;
  std::vector<std::string> warnings;
  const auto added = m_builder.add_folder_entries(folder.toStdString(), &err, &warnings);
  if (!err.empty()) appendLogLine(QString::fromStdString(err));
  appendLogLine(QString("Imported %1 file(s) from folder: %2").arg(added).arg(folder));
  for (const auto& warning : warnings) appendLogLine(QString::fromStdString(warning));
  refreshTable();
}

void CreateDlcAstDialog::onRemoveSelected() {
  QList<int> rows;
  for (const auto& idx : m_entriesTable->selectionModel()->selectedRows()) rows.push_back(idx.row());
  std::sort(rows.begin(), rows.end(), std::greater<int>());
  for (int row : rows) m_builder.remove_entry(static_cast<std::size_t>(row));
  refreshTable();
}

void CreateDlcAstDialog::onClearAll() {
  m_builder.clear_entries();
  refreshTable();
}

void CreateDlcAstDialog::onBuild() {
  if (m_donorEdit->text().trimmed().isEmpty() || m_outputEdit->text().trimmed().isEmpty()) {
    QMessageBox::information(this, "ASTra Core", "Select both the donor AST and the output AST path.");
    return;
  }
  if (!ensureProfileLoaded()) return;

  const auto validation = m_builder.validate();
  if (!validation.ok) {
    QString msg;
    for (const auto& err : validation.errors) {
      if (!msg.isEmpty()) msg += "\n";
      msg += QString::fromStdString(err);
    }
    QMessageBox::warning(this, "ASTra Core", "Build blocked because validation failed:\n" + msg);
    return;
  }

  std::string err;
  if (!m_builder.save_to_disk(m_outputEdit->text().toStdString(), &err)) {
    QMessageBox::warning(this,
                         "ASTra Core",
                         QString("Build failed.\n\nUser-facing summary:\nThe donor structure could not be reproduced exactly enough for a valid AST output, or the generated archive could not be reopened.\n\nDetails:\n%1")
                             .arg(QString::fromStdString(err)));
    return;
  }
  QMessageBox::information(this, "ASTra Core", "DLC AST built successfully and reopened successfully in ASTra Core validation.");
  accept();
}

void CreateDlcAstDialog::onHowToUse() {
  QString text;
  text += "1. Choose a donor/template AST from the same game/platform/family.\n";
  text += "2. Choose the output AST path.\n";
  text += "3. Add files individually or add a folder recursively.\n";
  text += "4. Review and edit archive/internal names in the table.\n";
  text += "5. Resolve any warnings or errors.\n";
  text += "6. Build the AST.\n";
  text += "7. Reopen and test the generated AST in ASTra Core and in-game.\n\n";
  text += "Notes:\n";
  text += "• This is donor/template-driven, not blank AST creation.\n";
  text += "• Using a donor from the correct game/platform matters.\n";
  text += "• Duplicate archive names are not allowed.\n";
  text += "• Build output is validated by reopening when possible.";
  QMessageBox::information(this, "How to Use - Create DLC AST", text);
}

void CreateDlcAstDialog::onTableItemChanged(QTableWidgetItem* item) {
  if (!item) return;
  const int row = item->row();
  if (row < 0 || row >= static_cast<int>(m_builder.mutable_entries().size())) return;
  auto& entry = m_builder.mutable_entries()[static_cast<std::size_t>(row)];
  if (item->column() == 1) {
    entry.archive_name = item->text().trimmed().toStdString();
  } else if (item->column() == 4) {
    const QString lowered = item->text().trimmed().toLower();
    entry.include = (lowered == "yes" || lowered == "true" || lowered == "1" || lowered == "y");
  }
  refreshTable();
}

void CreateDlcAstDialog::refreshTable() {
  const auto validation = m_builder.validate();
  const auto& entries = m_builder.entries();

  m_entriesTable->blockSignals(true);
  m_entriesTable->setRowCount(static_cast<int>(entries.size()));
  for (int row = 0; row < static_cast<int>(entries.size()); ++row) {
    const auto& e = entries[static_cast<std::size_t>(row)];
    auto set = [&](int col, const QString& text, bool editable) {
      auto* item = m_entriesTable->item(row, col);
      if (!item) {
        item = new QTableWidgetItem();
        m_entriesTable->setItem(row, col, item);
      }
      item->setText(text);
      auto flags = item->flags();
      if (editable) flags |= Qt::ItemIsEditable;
      else flags &= ~Qt::ItemIsEditable;
      item->setFlags(flags);
    };
    set(0, QString::fromStdString(e.source_path.string()), false);
    set(1, QString::fromStdString(e.archive_name), true);
    QString ty = "Raw";
    if (e.detected_type == gf::core::AstBuildEntry::Type::Texture) ty = "Texture";
    else if (e.detected_type == gf::core::AstBuildEntry::Type::Text) ty = "Text";
    set(2, ty, false);
    set(3, e.compression == gf::core::AstBuildEntry::CompressionMode::Zlib ? "Zlib" : "None", false);
    set(4, e.include ? "Yes" : "No", true);
    QStringList status;
    if (e.valid) status << "Ready";
    for (const auto& w : e.warnings) status << QString::fromStdString(w);
    for (const auto& er : e.errors) status << QString::fromStdString(er);
    set(5, status.join(" | "), false);
  }
  m_entriesTable->blockSignals(false);

  m_buildButton->setEnabled(validation.ok && !entries.empty() && !m_outputEdit->text().trimmed().isEmpty());
  if (validation.ok) {
    int included = 0;
    for (const auto& e : entries) if (e.include) ++included;
    m_statusLabel->setText(QString("Entries: %1   Included: %2   Warnings: %3   Errors: 0")
                               .arg(entries.size())
                               .arg(included)
                               .arg(validation.warnings.size()));
  } else {
    QStringList errs;
    for (const auto& err : validation.errors) errs << QString::fromStdString(err);
    m_statusLabel->setText(QString("Entries: %1   Errors: %2\n%3")
                               .arg(entries.size())
                               .arg(validation.errors.size())
                               .arg(errs.join("\n")));
  }
  refreshDonorSummary();
}

} // namespace gf::gui
