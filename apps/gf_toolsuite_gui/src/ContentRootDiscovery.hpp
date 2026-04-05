#pragma once
#include <QDir>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QPair>

namespace gf::gui {

// Phase 6A: Shared helpers for discovering AST/EDAT content files on disk.
//
// Used by BaselineCaptureService and ProfileResolverService to avoid
// duplicating file-discovery logic between capture and resolve paths.

// Recursively discovers all AST and EDAT content files under scanRoot.
//
// Returns (relPath, absPath) pairs where relPath is relative to scanRoot
// using forward slashes.
//
// Patterns matched: *.ast, *.AST, *.ast.edat, *.AST.EDAT
//
// Depth is unbounded — callers targeting the update content root may have
// DLC subfolders nested arbitrarily deep (e.g. DLC/CFBR_DLC_V19/*.ast).
inline void discoverAstFilesRecursive(const QDir&                       scanRoot,
                                       const QString&                    relBase,
                                       QVector<QPair<QString,QString>>& out)
{
    static const QStringList kFilters = {
        QStringLiteral("*.ast"),
        QStringLiteral("*.AST"),
        QStringLiteral("*.ast.edat"),
        QStringLiteral("*.AST.EDAT")
    };

    // Files at this level
    const QStringList files = scanRoot.entryList(kFilters, QDir::Files, QDir::Name);
    for (const QString& f : files) {
        const QString rel = relBase.isEmpty() ? f : relBase + QLatin1Char('/') + f;
        out.append({rel, scanRoot.filePath(f)});
    }

    // Recurse into subdirectories
    const QStringList subdirs = scanRoot.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& sub : subdirs) {
        const QString newBase = relBase.isEmpty() ? sub : relBase + QLatin1Char('/') + sub;
        discoverAstFilesRecursive(QDir(scanRoot.filePath(sub)), newBase, out);
    }
}

// Flat (non-recursive) discovery of AST files in the base content root.
//
// Patterns matched: *.ast, *.AST
// Does NOT include .edat — base root files are unencrypted by convention.
inline QStringList discoverBaseAstFiles(const QDir& root)
{
    return root.entryList(
        {QStringLiteral("*.ast"), QStringLiteral("*.AST")},
        QDir::Files, QDir::Name);
}

} // namespace gf::gui
