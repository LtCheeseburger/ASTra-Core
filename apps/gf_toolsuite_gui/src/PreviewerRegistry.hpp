#pragma once
#include <QString>
#include <functional>
#include <vector>

namespace gf::gui {

// v0.6.20+: lightweight plugin boundary scaffold.
// Not wired into MainWindow yet (safe no-op until used).
struct PreviewerEntry {
  QString name;
  std::function<bool(const QString& path)> canPreview;
  std::function<void(const QString& path)> openPreview;
};

class PreviewerRegistry {
public:
  void add(const PreviewerEntry& e) { m_entries.push_back(e); }
  const std::vector<PreviewerEntry>& entries() const { return m_entries; }
private:
  std::vector<PreviewerEntry> m_entries;
};

} // namespace gf::gui
