#pragma once
#include "RuntimeContentRoot.hpp"
#include <QString>
#include <QVector>

namespace gf::gui {

// Supported emulator/runtime targets.
// Extend this enum when additional platforms are supported.
enum class RuntimePlatform {
    RPCS3 = 0   // PlayStation 3 via RPCS3 emulator (the only supported target for now)
};

// Returns a human-readable label for a RuntimePlatform.
inline QString runtimePlatformLabel(RuntimePlatform p) {
    switch (p) {
    case RuntimePlatform::RPCS3: return "RPCS3";
    }
    return "Unknown";
}

// Per-game runtime environment configuration.
// Stored at: <appDataDir>/runtime_configs/<gameId>.json
//
// This is separate from mod profiles — it describes the emulator/runtime
// environment ASTra will target for this game, not the mod workspace.
struct RuntimeTargetConfig {
    QString         gameId;         // SHA-1 cache ID of the game
    RuntimePlatform platform = RuntimePlatform::RPCS3;
    QString         rpcs3ExePath;   // Absolute path to rpcs3.exe
    QString         astDirPath;     // Absolute path to base content root directory (flat *.ast / *.AST)
    QString         configuredAt;   // ISO-8601 UTC timestamp of last save

    // Phase 5A/6A: optional additional content roots.
    // Phase 6A: only one entry with kind==Update is used by the new UI.
    // Old configs may still contain Dlc/CustomDlc entries (backward compat).
    // Empty = legacy single-root mode; astDirPath is the only live directory.
    QVector<RuntimeContentRoot> contentRoots;
};

// Phase 6A: convenience accessor — returns the update content root path, or
// empty if no Update root is configured.
inline QString updateRootPath(const RuntimeTargetConfig& cfg)
{
    for (const RuntimeContentRoot& cr : cfg.contentRoots)
        if (cr.kind == RuntimeContentKind::Update) return cr.path;
    return {};
}

} // namespace gf::gui
