#include "ModAssetValidator.hpp"

#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>

namespace gf::gui {

// static
bool ModAssetValidator::isSafeRelPath(const QString& relPath) {
    if (relPath.isEmpty()) return false;
    if (relPath.startsWith('/') || relPath.startsWith('\\')) return false;
    if (relPath.contains("..")) return false;
    if (relPath.contains('\0')) return false;
    // Reject Windows reserved device names
    static const QRegularExpression kDevName(
        QStringLiteral(R"((?:^|[/\\])(?:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.[^/\\]*)?$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (kDevName.match(relPath).hasMatch()) return false;
    return true;
}

// static
bool ModAssetValidator::isSupportedImageExtension(const QString& absPath) {
    const QString ext = QFileInfo(absPath).suffix().toLower();
    return ext == QLatin1String("png")  ||
           ext == QLatin1String("jpg")  ||
           ext == QLatin1String("jpeg") ||
           ext == QLatin1String("webp");
}

// static
bool ModAssetValidator::validateImageFile(const QString& absPath, QString* outErr) {
    if (!QFileInfo::exists(absPath)) {
        if (outErr) *outErr = QString("File not found: %1").arg(absPath);
        return false;
    }
    if (!isSupportedImageExtension(absPath)) {
        if (outErr)
            *outErr = QString("'%1' is not a supported image format. Use PNG, JPG/JPEG, or WebP.")
                          .arg(QFileInfo(absPath).fileName());
        return false;
    }
    QImageReader reader(absPath);
    if (!reader.canRead()) {
        if (outErr)
            *outErr = QString("Cannot decode image '%1': %2")
                          .arg(QFileInfo(absPath).fileName(), reader.errorString());
        return false;
    }
    return true;
}

} // namespace gf::gui
