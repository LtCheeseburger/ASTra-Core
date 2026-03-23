#pragma once
#include <QString>

namespace gf::gui {

// Identifies the kind of content a root directory contains.
enum class RuntimeContentKind {
    BaseGame  = 0,  // Primary game disc root (contains base qkl_*.AST files)
    Update    = 1,  // Game update/patch root (overlays base game)
    Dlc       = 2,  // Official DLC root
    CustomDlc = 3,  // User-added extra content root (e.g. modded expansions)
};

inline QString runtimeContentKindString(RuntimeContentKind k) {
    switch (k) {
    case RuntimeContentKind::BaseGame:  return QStringLiteral("base_game");
    case RuntimeContentKind::Update:    return QStringLiteral("update");
    case RuntimeContentKind::Dlc:       return QStringLiteral("dlc");
    case RuntimeContentKind::CustomDlc: return QStringLiteral("custom_dlc");
    }
    return QStringLiteral("base_game");
}

inline RuntimeContentKind runtimeContentKindFromString(const QString& s) {
    if (s == QLatin1String("update"))     return RuntimeContentKind::Update;
    if (s == QLatin1String("dlc"))        return RuntimeContentKind::Dlc;
    if (s == QLatin1String("custom_dlc")) return RuntimeContentKind::CustomDlc;
    return RuntimeContentKind::BaseGame;
}

inline QString runtimeContentKindLabel(RuntimeContentKind k) {
    switch (k) {
    case RuntimeContentKind::BaseGame:  return QStringLiteral("Base Game");
    case RuntimeContentKind::Update:    return QStringLiteral("Update");
    case RuntimeContentKind::Dlc:       return QStringLiteral("DLC");
    case RuntimeContentKind::CustomDlc: return QStringLiteral("Custom DLC");
    }
    return QStringLiteral("Base Game");
}

// One live root directory that may contain qkl_*.AST files.
// Phase 5A: layered content roots for multi-install layouts (base + update + DLC).
struct RuntimeContentRoot {
    RuntimeContentKind kind        = RuntimeContentKind::BaseGame;
    QString            path;        // Absolute path to the live root directory
    QString            displayName; // Human-readable label (e.g. "Update 1.03")
};

} // namespace gf::gui
