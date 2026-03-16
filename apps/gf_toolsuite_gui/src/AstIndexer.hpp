#pragma once
#include <QDateTime>
#include <QVector>
#include <QString>

namespace gf::gui {

// Describes a filesystem "bucket" that should be indexed (Base / Patch / Update).
struct AstBucketSpec {
  QString name;    // e.g. "Base", "Update"
  QString dirPath; // absolute path to scan
};

// One AST file discovered during indexing (metadata only; no parsing).
struct AstIndexEntry {
  QString bucketName;   // bucket that produced this entry
  QString fileName;     // filename only (e.g. QKL_MISC.AST)
  QString absolutePath; // full path on disk
  QString relativePath; // path relative to bucket root (e.g. "subdir/QKL_MISC.AST")
  quint64 fileSize = 0;
  QDateTime modifiedTime;
};

class AstIndexer final {
public:
  // Index all buckets. This performs filesystem scanning only (no AST parsing).
  static QVector<AstIndexEntry> indexBuckets(const QVector<AstBucketSpec>& buckets);
};

} // namespace gf::gui
