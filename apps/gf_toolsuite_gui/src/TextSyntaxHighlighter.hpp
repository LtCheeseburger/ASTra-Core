#pragma once
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QRegularExpression>

namespace gf::gui {

enum class TextSyntaxMode {
  Plain,
  Xml,
  Json,
  Ini,
};

class TextSyntaxHighlighter final : public QSyntaxHighlighter {
  Q_OBJECT
public:
  explicit TextSyntaxHighlighter(QTextDocument* doc);
  void setMode(TextSyntaxMode mode);

protected:
  void highlightBlock(const QString& text) override;

private:
  TextSyntaxMode m_mode = TextSyntaxMode::Plain;

  // Formats
  QTextCharFormat m_tag;
  QTextCharFormat m_attr;
  QTextCharFormat m_string;
  QTextCharFormat m_number;
  QTextCharFormat m_comment;
  QTextCharFormat m_key;

  // Regex
  QRegularExpression m_xmlTag;
  QRegularExpression m_xmlAttr;
  QRegularExpression m_xmlComment;
  QRegularExpression m_jsonString;
  QRegularExpression m_jsonNumber;
  QRegularExpression m_jsonKey;
  QRegularExpression m_iniSection;
  QRegularExpression m_iniKey;
  QRegularExpression m_iniComment;
};

} // namespace gf::gui
