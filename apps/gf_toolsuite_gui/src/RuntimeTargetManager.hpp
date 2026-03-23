#pragma once
#include "RuntimeTargetConfig.hpp"
#include <QStringList>
#include <optional>

namespace gf::gui {

// Persists and validates per-game runtime target configuration.
//
// Storage: <appDataDir>/runtime_configs/<gameId>.json
// One JSON file per game.  Stateless — every call reads/writes directly.
//
// Validation is intentionally strict: a RuntimeTargetConfig is considered
// valid only when the emulator executable and AST directory both exist on disk
// and the AST directory contains a minimum number of qkl_*.AST files.
class RuntimeTargetManager {
public:
    // Returns true if a config file exists for gameId (does not validate contents).
    static bool hasConfig(const QString& gameId);

    // Load and return the config for gameId.  Returns nullopt if the file is
    // absent or unreadable; writes a description to *outErr on error.
    static std::optional<RuntimeTargetConfig> load(const QString& gameId,
                                                    QString*       outErr = nullptr);

    // Persist a config.  Creates the runtime_configs/ directory if needed.
    // Does NOT validate the config — call validate() first.
    static bool save(const RuntimeTargetConfig& config, QString* outErr = nullptr);

    // Validate a config against the filesystem.
    // Returns true and clears *outErrors on success.
    // Returns false and populates *outErrors with one or more human-readable
    // error strings on failure.
    static bool validate(const RuntimeTargetConfig& config,
                         QStringList*               outErrors);

    // Minimum number of qkl_*.AST files that must be present in the AST directory.
    static constexpr int kMinAstFiles = 3;

private:
    static QString configPath(const QString& gameId);
    static QString configDir();
};

} // namespace gf::gui
