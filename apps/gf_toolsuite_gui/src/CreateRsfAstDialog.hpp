#pragma once

#include <QDialog>
#include <QObject>
#include <QString>

#include <filesystem>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QDialogButtonBox;

namespace gf::gui {

// ============================================================================
//  Backend types  (header-only declarations; implementations in .cpp)
// ============================================================================

/// One texture descriptor from the EASE-equivalent RDF option table.
struct RdfTextureSpec {
    std::string key;    ///< Lookup key — uppercase fragment (e.g. "JERSEY_COL")
    std::string format; ///< "D3DFMT_DXT1" or "D3DFMT_DXT5"
    std::uint32_t width   = 0;
    std::uint32_t height  = 0;
    std::uint32_t mips    = 0;
};

/// Input to the RSF-based AST build pipeline.
struct RsfAstBuildRequest {
    std::filesystem::path input_folder;
    std::filesystem::path output_folder;  ///< Defaults to input_folder when empty
    std::filesystem::path rsf_path;       ///< Validated single RSF file in input_folder
    std::vector<std::filesystem::path> tga_files; ///< All .tga files found in input_folder
    std::vector<std::filesystem::path> payload_files; ///< Non-TGA/RSF/junk files to package
};

/// Detailed result from a completed (or failed) build.
struct RsfAstBuildResult {
    bool success = false;
    std::filesystem::path xbox_output;
    std::filesystem::path ps3_output;
    std::vector<std::string> log;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

// ============================================================================
//  Dialog
// ============================================================================

/// Modal dialog that drives the "Create RSF-Based AST" feature.
///
/// Workflow mirrors EASE AST Editor 2.0:
///   1. User picks a prepared folder (RSF + TGA textures + companion data files).
///   2. Tool validates the folder content.
///   3. TGA files are converted → DDS (PS3 path) and → XPR2 (Xbox path).
///   4. Two BGFA/AST containers are packaged from the converted assets + RSF.
///   5. Outputs are written as  X-<name>_BIG  and  P-<name>_BIG.
///
/// Important constraints faithfully reproduced from EASE:
///   - Source textures MUST be TGA.  DDS as source input is refused.
///   - Both PS3 and Xbox outputs are always produced together.
///   - FileNameLength = 64 (matches EASE BGFA config).
///   - Tag IDs match EASE's hardcoded tag table (RSF/P3R/XPR/NAME).
///
class CreateRsfAstDialog final : public QDialog {
    Q_OBJECT

public:
    explicit CreateRsfAstDialog(QWidget* parent = nullptr);

private slots:
    void onBrowseInput();
    void onBrowseOutput();
    void onBuild();
    void onBuildFinished(bool success, const QString& summary);

private:
    // UI helpers
    void appendLog(const QString& line);
    void setUiEnabled(bool enabled);
    void showValidationErrors(const std::vector<std::string>& errors,
                              const std::vector<std::string>& warnings);

    // Backend (runs on worker thread)
    static RsfAstBuildResult run_pipeline(RsfAstBuildRequest req);

    // Widgets
    QLineEdit*      m_inputEdit   = nullptr;
    QLineEdit*      m_outputEdit  = nullptr;
    QPlainTextEdit* m_logView     = nullptr;
    QProgressBar*   m_progress    = nullptr;
    QPushButton*    m_buildButton = nullptr;
    QDialogButtonBox* m_buttonBox = nullptr;
    QLabel*         m_statusLabel = nullptr;
};

} // namespace gf::gui
