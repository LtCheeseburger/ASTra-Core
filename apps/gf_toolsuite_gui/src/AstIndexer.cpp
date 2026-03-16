#include "AstIndexer.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <algorithm>

namespace gf::gui {

static bool bucketLess(const AstIndexEntry& a, const AstIndexEntry& b) {
  const int cmp = QString::compare(a.bucketName, b.bucketName, Qt::CaseInsensitive);
  if (cmp != 0) return cmp < 0;
  return QString::compare(a.relativePath, b.relativePath, Qt::CaseInsensitive) < 0;
}

QVector<AstIndexEntry> AstIndexer::indexBuckets(const QVector<AstBucketSpec>& buckets) {
  QVector<AstIndexEntry> out;

  for (const auto& bucket : buckets) {
    const QString root = bucket.dirPath.trimmed();
    if (root.isEmpty()) continue;

    QDir rootDir(root);
    if (!rootDir.exists()) continue;

    // Match both common casings.
    const QStringList filters = { "*.ast", "*.AST" };

    QDirIterator it(root,
                    filters,
                    QDir::Files,
                    QDirIterator::Subdirectories);

    while (it.hasNext()) {
      const QString absPath = it.next();
      const QFileInfo fi(absPath);
      if (!fi.exists() || !fi.isFile()) continue;

      AstIndexEntry e;
      e.bucketName   = bucket.name;
      e.fileName     = fi.fileName();
      e.absolutePath = fi.absoluteFilePath();
      e.relativePath = rootDir.relativeFilePath(e.absolutePath);
      e.fileSize     = static_cast<quint64>(fi.size());
      e.modifiedTime = fi.lastModified();

      out.push_back(std::move(e));
    }
  }

  std::sort(out.begin(), out.end(), bucketLess);
  return out;
}

} // namespace gf::gui
