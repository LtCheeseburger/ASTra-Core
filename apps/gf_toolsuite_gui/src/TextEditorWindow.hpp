#pragma once

#include <QMainWindow>
#include "DocumentLifecycle.hpp"
#include "TextSyntaxHighlighter.hpp"

class QPlainTextEdit;
class QAction;
class QLineEdit;
class QWidget;
class QLabel;

namespace gf::gui {

// Lightweight plain-text editor for XML/config/text files.
// v0.6.2: first real "save"-capable module, built on gf::core::safe_write.
class TextEditorWindow final : public QMainWindow {
  Q_OBJECT
public:
  explicit TextEditorWindow(QWidget* parent = nullptr);

  // Convenience to open a file immediately.
  bool openFile(const QString& path);

  // Open provided text content (e.g., extracted from an embedded AST entry).
  // `suggestedFileName` is used as the default name for Save As.
  bool openTextContent(const QString& title, const QString& text, const QString& suggestedFileName = QString());

protected:
  void closeEvent(QCloseEvent* e) override;

private slots:
  void onOpen();
  void onSave();
  void onSaveAs();
  void onRevert();
  void onFind();
  void onGoToLine();
  void onToggleWrap();

private:
  void buildFindBar();
  void setSyntaxForPath(const QString& path);
  void showFindBar(bool show);
  void findNext(bool backwards);
  void replaceOne();
  void replaceAll();

  void buildUi();
  void updateUi();
  void updateCursorStatus();
    bool saveToPath(const QString& path);

  QPlainTextEdit* m_editor = nullptr;
  QAction* m_actOpen = nullptr;
  QAction* m_actSave = nullptr;
  QAction* m_actSaveAs = nullptr;
  QAction* m_actRevert = nullptr;
  QAction* m_actFind = nullptr;
  QAction* m_actUndo = nullptr;
  QAction* m_actRedo = nullptr;
  QAction* m_actWrap = nullptr;
  QAction* m_actGoto = nullptr;

  DocumentLifecycle m_doc;
  QString m_loadedText;    // last saved/loaded text snapshot
  QString m_suggestedName;

  // Find/Replace bar + lightweight syntax highlighting
  QWidget* m_findBar = nullptr;
  QLineEdit* m_findEdit = nullptr;
  QLineEdit* m_replaceEdit = nullptr;
  QLabel* m_findStatus = nullptr;
  QLabel* m_cursorStatus = nullptr;
  TextSyntaxHighlighter* m_highlighter = nullptr;
};

} // namespace gf::gui
