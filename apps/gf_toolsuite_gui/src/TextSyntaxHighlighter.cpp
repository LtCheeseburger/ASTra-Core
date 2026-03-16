#include "TextSyntaxHighlighter.hpp"

#include <QTextDocument>

namespace gf::gui {

TextSyntaxHighlighter::TextSyntaxHighlighter(QTextDocument* doc)
  : QSyntaxHighlighter(doc) {

  // NOTE: We intentionally keep formatting subtle and theme-agnostic.
  m_tag.setFontWeight(QFont::DemiBold);

  m_attr.setFontItalic(true);

  // strings / numbers / comments / keys - just font tweaks to avoid fighting dark themes
  m_string.setFontItalic(false);
  m_number.setFontItalic(false);
  m_comment.setFontItalic(true);
  m_key.setFontWeight(QFont::DemiBold);

  // XML
  m_xmlTag = QRegularExpression(R"(<\/?\s*[\w:\-\.]+)");
  m_xmlAttr = QRegularExpression(R"(\b[\w:\-\.]+\s*(?==))");
  m_xmlComment = QRegularExpression(R"(<!--.*?-->)");

  // JSON
  m_jsonString = QRegularExpression(R"("([^"\\]|\\.)*")");
  m_jsonNumber = QRegularExpression(R"(\b-?\d+(\.\d+)?([eE][+-]?\d+)?\b)");
  m_jsonKey = QRegularExpression(R"("([^"\\]|\\.)*"\s*:)");

  // INI / CFG
  m_iniSection = QRegularExpression(R"(^\s*\[[^\]]+\])");
  m_iniKey = QRegularExpression(R"(^\s*[\w\.\-]+\s*(?==))");
  m_iniComment = QRegularExpression(R"(^\s*[#;].*$)");
}

void TextSyntaxHighlighter::setMode(TextSyntaxMode mode) {
  if (m_mode == mode) return;
  m_mode = mode;
  rehighlight();
}

void TextSyntaxHighlighter::highlightBlock(const QString& text) {
  if (m_mode == TextSyntaxMode::Plain) return;

  auto applyAll = [&](const QRegularExpression& rx, const QTextCharFormat& fmt) {
    auto it = rx.globalMatch(text);
    while (it.hasNext()) {
      auto m = it.next();
      setFormat(m.capturedStart(), m.capturedLength(), fmt);
    }
  };

  switch (m_mode) {
    case TextSyntaxMode::Xml: {
      applyAll(m_xmlComment, m_comment);
      applyAll(m_xmlTag, m_tag);
      applyAll(m_xmlAttr, m_attr);
      // naive strings
      applyAll(QRegularExpression(R"("([^"\\]|\\.)*")"), m_string);
      break;
    }
    case TextSyntaxMode::Json: {
      applyAll(m_jsonKey, m_key);
      applyAll(m_jsonString, m_string);
      applyAll(m_jsonNumber, m_number);
      break;
    }
    case TextSyntaxMode::Ini: {
      applyAll(m_iniComment, m_comment);
      applyAll(m_iniSection, m_tag);
      applyAll(m_iniKey, m_key);
      applyAll(QRegularExpression(R"(=.+$)"), m_string);
      break;
    }
    case TextSyntaxMode::Plain:
    default:
      break;
  }
}

} // namespace gf::gui
