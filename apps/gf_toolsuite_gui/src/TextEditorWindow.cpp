#include "TextEditorWindow.hpp"

#include "PlatformUtils.hpp"
#include "gf/core/log.hpp"
#include "gf/core/safe_write.hpp"

#include <QFontDatabase>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QToolBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>

#include <QPlainTextEdit>
#include <QAction>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QKeySequence>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QInputDialog>
#include <QCloseEvent>
#include <QDir>
#include <QFont>

#include <filesystem>

namespace gf::gui {

TextEditorWindow::TextEditorWindow(QWidget* parent) : QMainWindow(parent) {
  m_doc.titleHint = "Text Editor";
  buildUi();
  updateUi();
}

void TextEditorWindow::buildUi() {
  setMinimumSize(860, 560);

  m_editor = new QPlainTextEdit(this);
  m_editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
  m_highlighter = new TextSyntaxHighlighter(m_editor->document());

  // Prefer a readable monospace if available.
  QFont mono("Menlo");
  mono.setStyleHint(QFont::Monospace);
  mono.setPointSize(11);
  m_editor->setFont(mono);

  // Central widget with editor + find bar
  auto* central = new QWidget(this);
  auto* v = new QVBoxLayout(central);
  v->setContentsMargins(0,0,0,0);
  v->setSpacing(0);
  v->addWidget(m_editor, 1);
  buildFindBar();
  v->addWidget(m_findBar, 0);
  setCentralWidget(central);

  // Actions
  m_actOpen = new QAction("Open…", this);
  m_actOpen->setShortcut(QKeySequence::Open);
  connect(m_actOpen, &QAction::triggered, this, &TextEditorWindow::onOpen);

  m_actSave = new QAction("Save", this);
  m_actSave->setShortcut(QKeySequence::Save);
  connect(m_actSave, &QAction::triggered, this, &TextEditorWindow::onSave);

  m_actSaveAs = new QAction("Save As…", this);
  m_actSaveAs->setShortcut(QKeySequence::SaveAs);
  connect(m_actSaveAs, &QAction::triggered, this, &TextEditorWindow::onSaveAs);

  m_actRevert = new QAction("Revert", this);
  connect(m_actRevert, &QAction::triggered, this, &TextEditorWindow::onRevert);
  m_actUndo = new QAction("Undo", this);
  m_actUndo->setShortcut(QKeySequence::Undo);
  connect(m_actUndo, &QAction::triggered, this, [this]() { if (m_editor) m_editor->undo(); });

  m_actRedo = new QAction("Redo", this);
  m_actRedo->setShortcut(QKeySequence::Redo);
  connect(m_actRedo, &QAction::triggered, this, [this]() { if (m_editor) m_editor->redo(); });

  m_actFind = new QAction("Find/Replace", this);
  m_actFind->setShortcut(QKeySequence::Find);
  connect(m_actFind, &QAction::triggered, this, &TextEditorWindow::onFind);

  m_actGoto = new QAction("Go to Line…", this);
  m_actGoto->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
  connect(m_actGoto, &QAction::triggered, this, &TextEditorWindow::onGoToLine);

  m_actWrap = new QAction("Word Wrap", this);
  m_actWrap->setCheckable(true);
  m_actWrap->setChecked(false);
  connect(m_actWrap, &QAction::triggered, this, &TextEditorWindow::onToggleWrap);


  // Menus
  auto* fileMenu = menuBar()->addMenu("File");
  fileMenu->addAction(m_actOpen);
  fileMenu->addSeparator();
  fileMenu->addAction(m_actSave);
  fileMenu->addAction(m_actSaveAs);
  fileMenu->addAction(m_actRevert);
  fileMenu->addSeparator();
  fileMenu->addAction("Close", this, [this]() { close(); }, QKeySequence::Close);

  auto* editMenu = menuBar()->addMenu("Edit");
  editMenu->addAction(m_actUndo);
  editMenu->addAction(m_actRedo);
  editMenu->addSeparator();
  editMenu->addAction(m_actFind);
  editMenu->addAction(m_actGoto);
  editMenu->addAction(m_actWrap);

  // Toolbar
  auto* tb = addToolBar("File");
  tb->addAction(m_actOpen);
  tb->addAction(m_actSave);
  tb->addAction(m_actSaveAs);
  tb->addAction(m_actRevert);
  tb->addSeparator();
  tb->addAction(m_actUndo);
  tb->addAction(m_actRedo);
  tb->addSeparator();
  tb->addAction(m_actFind);
  tb->addAction(m_actGoto);
  tb->addAction(m_actWrap);

  statusBar()->showMessage("Ready");

  m_cursorStatus = new QLabel(this);
  statusBar()->addPermanentWidget(m_cursorStatus);
  connect(m_editor, &QPlainTextEdit::cursorPositionChanged, this, &TextEditorWindow::updateCursorStatus);
  updateCursorStatus();

  // Dirty tracking
  connect(m_editor->document(), &QTextDocument::modificationChanged, this, [this](bool) {
    updateUi();
  });

  connect(m_editor, &QPlainTextEdit::undoAvailable, this, [this](bool ok) {
    if (m_actUndo) m_actUndo->setEnabled(ok);
  });
  connect(m_editor, &QPlainTextEdit::redoAvailable, this, [this](bool ok) {
    if (m_actRedo) m_actRedo->setEnabled(ok);
  });
}

void TextEditorWindow::updateUi() {
  const bool dirty = m_editor && m_editor->document()->isModified();

  m_doc.dirty = dirty;
  setWindowTitle(m_doc.makeWindowTitle("ASTra Core", "Text Editor"));

  if (m_actSave) m_actSave->setEnabled(dirty && !m_doc.path.isEmpty());
  if (m_actSaveAs) m_actSaveAs->setEnabled(true);
  if (m_actRevert) m_actRevert->setEnabled(dirty && !m_loadedText.isNull());

  if (m_actUndo && m_editor) m_actUndo->setEnabled(m_editor->document()->isUndoAvailable());
  if (m_actRedo && m_editor) m_actRedo->setEnabled(m_editor->document()->isRedoAvailable());
  updateCursorStatus();
}

void TextEditorWindow::updateCursorStatus() {
  if (!m_cursorStatus || !m_editor) return;
  const QTextCursor c = m_editor->textCursor();
  const int line = c.blockNumber() + 1;
  const int col = c.positionInBlock() + 1;
  m_cursorStatus->setText(QString("Ln %1, Col %2").arg(line).arg(col));
}

bool TextEditorWindow::openFile(const QString& path) {
  if (!DocumentLifecycle::maybePromptDiscard(this, m_doc.dirty)) return false;

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(this, "ASTra Core", QString("Failed to open file:\n%1").arg(path));
    return false;
  }

  // Keep it safe for now; we can add streaming / chunked loads later.
  static constexpr qint64 kMaxBytes = 32ll * 1024ll * 1024ll; // 32 MiB
  if (f.size() > kMaxBytes) {
    QMessageBox::warning(this, "ASTra Core", "File is too large for the text editor (32 MiB limit)." );
    return false;
  }

  const QByteArray bytes = f.readAll();
  // UTF-8 best effort; invalid sequences will be replaced.
  const QString text = QString::fromUtf8(bytes);

  m_editor->setPlainText(text);
  m_editor->document()->setModified(false);

  m_doc.path = QDir::toNativeSeparators(path);
  m_loadedText = text;

  statusBar()->showMessage(QString("Opened %1").arg(m_doc.path), 2500);
  updateUi();
  return true;
}


bool TextEditorWindow::openTextContent(const QString& title, const QString& text, const QString& suggestedFileName) {
  if (!DocumentLifecycle::maybePromptDiscard(this, m_doc.dirty)) return false;

  if (!m_editor) return false;
  m_editor->setPlainText(text);
  m_editor->document()->setModified(false);

  m_doc.path.clear(); // untitled (extracted content)
  m_loadedText = text;
  m_suggestedName = suggestedFileName;

  if (!suggestedFileName.isEmpty()) setSyntaxForPath(suggestedFileName);
  else setSyntaxForPath(title);

  setWindowTitle(QString("ASTra Core - Text Editor - %1").arg(title));
  statusBar()->showMessage("Loaded extracted text", 2500);
  updateUi();
  return true;
}

bool TextEditorWindow::saveToPath(const QString& path) {
  if (!m_editor) return false;
  const QString text = m_editor->toPlainText();

  gf::core::SafeWriteOptions opt;
  opt.make_backup = true;
  opt.max_bytes = 128ull * 1024ull * 1024ull; // 128 MiB

  const auto res = gf::core::safe_write_text(std::filesystem::path(path.toStdString()), text.toStdString(), opt);
  if (!res.ok) {
    QMessageBox::warning(this, "ASTra Core", QString("Save failed:\n%1").arg(QString::fromStdString(res.message)));
    return false;
  }

  m_doc.path = QDir::toNativeSeparators(path);
  m_loadedText = text;
  m_editor->document()->setModified(false);
  QString msg = QString("Saved %1").arg(m_doc.path);
  if (res.backup_path) {
    const QString bp = QDir::toNativeSeparators(QString::fromStdString(res.backup_path->string()));
    msg += QString(" (backup: %1)").arg(QFileInfo(bp).fileName());
  }
  statusBar()->showMessage(msg, 3500);
  updateUi();
  return true;
}

void TextEditorWindow::onOpen() {
  const QString startDir = !m_doc.path.isEmpty() ? QFileInfo(m_doc.path).absolutePath() : QDir::homePath();
  const QString path = QFileDialog::getOpenFileName(
      this,
      "Open Text File",
      startDir,
      "Text/Config Files (*.txt *.xml *.cfg *.conf *.ini *.json *.yaml *.yml *.lua *.js *.css *.html *.htm);;All Files (*)");
  if (path.isEmpty()) return;
  (void)openFile(path);
}

void TextEditorWindow::onSave() {
  if (m_doc.path.isEmpty()) {
    onSaveAs();
    return;
  }
  (void)saveToPath(m_doc.path);
}

void TextEditorWindow::onSaveAs() {
  const QString startDir = !m_doc.path.isEmpty() ? QFileInfo(m_doc.path).absolutePath() : QDir::homePath();
  QString suggested = startDir;
  if (m_doc.path.isEmpty() && !m_suggestedName.isEmpty()) {
    suggested = QDir(startDir).filePath(m_suggestedName);
  }
  const QString path = QFileDialog::getSaveFileName(this, "Save As", suggested, "All Files (*)");
  if (path.isEmpty()) return;
  (void)saveToPath(path);
}

void TextEditorWindow::onRevert() {
  if (!m_editor) return;
  if (!m_editor->document()->isModified()) return;
  const auto rc = QMessageBox::question(this, "ASTra Core", "Revert to last saved state?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (rc != QMessageBox::Yes) return;
  m_editor->setPlainText(m_loadedText);
  m_editor->document()->setModified(false);
  statusBar()->showMessage("Reverted", 2000);
  updateUi();
}

void TextEditorWindow::onFind() {
  showFindBar(true);
  if (m_findEdit) {
    m_findEdit->setFocus();
    m_findEdit->selectAll();
  }
}

void TextEditorWindow::onGoToLine() {
  if (!m_editor) return;
  bool ok = false;
  const int maxLine = std::max(1, m_editor->document()->blockCount());
  const int line = QInputDialog::getInt(this, "Go to Line", "Line number:", 1, 1, maxLine, 1, &ok);
  if (!ok) return;

  QTextBlock blk = m_editor->document()->findBlockByNumber(line - 1);
  if (!blk.isValid()) return;
  QTextCursor c(blk);
  m_editor->setTextCursor(c);
  m_editor->centerCursor();
  updateCursorStatus();
}

void TextEditorWindow::onToggleWrap() {
  if (!m_editor) return;
  const bool on = m_actWrap && m_actWrap->isChecked();
  m_editor->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
}

void TextEditorWindow::closeEvent(QCloseEvent* e) {
  if (!DocumentLifecycle::maybePromptDiscard(this, m_doc.dirty)) {
    e->ignore();
    return;
  }
  e->accept();
}

void TextEditorWindow::buildFindBar() {
  m_findBar = new QWidget(this);
  auto* h = new QHBoxLayout(m_findBar);
  h->setContentsMargins(6, 4, 6, 4);

  auto* lblFind = new QLabel("Find:", m_findBar);
  m_findEdit = new QLineEdit(m_findBar);

  auto* lblRep = new QLabel("Replace:", m_findBar);
  m_replaceEdit = new QLineEdit(m_findBar);

  auto* btnPrev = new QPushButton("Prev", m_findBar);
  auto* btnNext = new QPushButton("Next", m_findBar);
  auto* btnRep = new QPushButton("Replace", m_findBar);
  auto* btnAll = new QPushButton("All", m_findBar);
  auto* btnClose = new QPushButton("X", m_findBar);
  btnClose->setFixedWidth(24);

  m_findStatus = new QLabel("", m_findBar);
  m_findStatus->setMinimumWidth(160);

  connect(btnNext, &QPushButton::clicked, this, [this]() { findNext(false); });
  connect(btnPrev, &QPushButton::clicked, this, [this]() { findNext(true); });
  connect(btnRep, &QPushButton::clicked, this, [this]() { replaceOne(); });
  connect(btnAll, &QPushButton::clicked, this, [this]() { replaceAll(); });
  connect(btnClose, &QPushButton::clicked, this, [this]() { showFindBar(false); });

  connect(m_findEdit, &QLineEdit::returnPressed, this, [this]() { findNext(false); });

  h->addWidget(lblFind);
  h->addWidget(m_findEdit, 2);
  h->addWidget(lblRep);
  h->addWidget(m_replaceEdit, 2);
  h->addWidget(btnPrev);
  h->addWidget(btnNext);
  h->addWidget(btnRep);
  h->addWidget(btnAll);
  h->addWidget(m_findStatus, 1);
  h->addWidget(btnClose);

  m_findBar->setVisible(false);
}

void TextEditorWindow::showFindBar(bool show) {
  if (!m_findBar) return;
  m_findBar->setVisible(show);
  if (!show) {
    m_findStatus->setText("");
    m_editor->setFocus();
  }
}

void TextEditorWindow::findNext(bool backwards) {
  const QString needle = m_findEdit ? m_findEdit->text() : QString();
  if (needle.isEmpty()) {
    m_findStatus->setText("Enter search text.");
    return;
  }

  QTextDocument::FindFlags flags;
  if (backwards) flags |= QTextDocument::FindBackward;

  const bool ok = m_editor->find(needle, flags);
  if (!ok) {
    // wrap
    QTextCursor c = m_editor->textCursor();
    c.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
    m_editor->setTextCursor(c);
    const bool ok2 = m_editor->find(needle, flags);
    m_findStatus->setText(ok2 ? "Wrapped." : "Not found.");
  } else {
    m_findStatus->setText("");
  }
}

void TextEditorWindow::replaceOne() {
  const QString needle = m_findEdit->text();
  if (needle.isEmpty()) return;

  QTextCursor c = m_editor->textCursor();
  if (c.hasSelection() && c.selectedText() == needle) {
    c.insertText(m_replaceEdit->text());
    m_doc.setDirty(true);
    findNext(false);
  } else {
    findNext(false);
  }
}

void TextEditorWindow::replaceAll() {
  const QString needle = m_findEdit->text();
  if (needle.isEmpty()) return;

  const QString repl = m_replaceEdit->text();

  QTextCursor c = m_editor->textCursor();
  c.beginEditBlock();
  m_editor->moveCursor(QTextCursor::Start);

  int count = 0;
  while (m_editor->find(needle)) {
    QTextCursor cur = m_editor->textCursor();
    cur.insertText(repl);
    ++count;
  }
  c.endEditBlock();

  if (count > 0) m_doc.setDirty(true);
  m_findStatus->setText(QString("Replaced %1.").arg(count));
}

void TextEditorWindow::setSyntaxForPath(const QString& path) {
  if (!m_highlighter) return;
  const QString p = path.toLower();

  if (p.endsWith(".xml") || p.endsWith(".xml2") || p.endsWith(".wdproj") || p.endsWith(".html") || p.endsWith(".htm")) {
    m_highlighter->setMode(TextSyntaxMode::Xml);
  } else if (p.endsWith(".json")) {
    m_highlighter->setMode(TextSyntaxMode::Json);
  } else if (p.endsWith(".ini") || p.endsWith(".cfg") || p.endsWith(".conf")) {
    m_highlighter->setMode(TextSyntaxMode::Ini);
  } else {
    // XML sniff: common header
    const QString head = m_editor->toPlainText().left(64).trimmed().toLower();
    if (head.startsWith("<?xml") || head.startsWith("<")) m_highlighter->setMode(TextSyntaxMode::Xml);
    else if (head.startsWith("{") || head.startsWith("[")) m_highlighter->setMode(TextSyntaxMode::Json);
    else m_highlighter->setMode(TextSyntaxMode::Plain);
  }
}

} // namespace gf::gui
