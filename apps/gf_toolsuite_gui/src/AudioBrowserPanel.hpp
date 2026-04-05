#pragma once
#include <QDockWidget>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <vector>

#include "gf/audio/sbbe_types.hpp"
#include "gf/audio/sbkr_types.hpp"

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QSplitter;
class QTableWidget;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;
QT_END_NAMESPACE

namespace gf::gui {

class AudioPlayer;
class WaveformWidget;

// ---------------------------------------------------------------------------
// AudioScanResult — one paired SBbe+SBKR entry from a scan
// ---------------------------------------------------------------------------
struct AudioScanResult {
    gf::audio::SbbeFile sbbeFile;
    gf::audio::SbkrFile sbkrFile;
    bool hasSbbe = false;
    bool hasSbkr = false;
};

// ---------------------------------------------------------------------------
// AudioBrowserPanel
// A dockable panel that scans a directory for SBbe/SBKR audio files,
// displays them in a tree, and allows playback / WAV export.
//
// Thread safety:
//   All public methods must be called from the UI thread.
//   AudioPlayer internally manages its own background thread.
// ---------------------------------------------------------------------------
class AudioBrowserPanel final : public QDockWidget {
    Q_OBJECT
public:
    explicit AudioBrowserPanel(QWidget* parent = nullptr);
    ~AudioBrowserPanel() override;

public slots:
    // Scan the given directory for audio files and populate the tree.
    void scan(const QString& directory);
    // Load a single SBbe or SBKR file directly (used for embedded items).
    void loadFile(const QString& filePath);

signals:
    void scanRequested(QString directory);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void onScanClicked();
    void onStopPlayback();
    void onExportWav();
    void onItemClicked(QTreeWidgetItem* item, int column);
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onSearchChanged(const QString& text);
    void onPlaybackProgress(float progress);
    void onPlaybackStopped();
    void onContextMenu(const QPoint& pos);
    void onCopySoundId();
    void onShowInExplorer();

private:
    void buildUi();
    void buildDetailPane(QWidget* parent);
    void populateCatalog(const QList<AudioScanResult>& results);
    void updateCountLabel();

    // Loads samples for the given slot, updates waveform + meta, starts playback.
    // Re-entrant safe: guarded by m_isSwitching.
    void activateSlot(const QString& filePath, int slotIndex);

    // Stops any current playback synchronously (blocking).
    void stopCurrentPlayback();

    // --- Widgets ---
    QWidget*       m_centralWidget   = nullptr;
    QToolBar*      m_toolbar         = nullptr;
    QLineEdit*     m_searchEdit      = nullptr;
    QLabel*        m_countLabel      = nullptr;
    QTreeWidget*   m_tree            = nullptr;
    QSplitter*     m_splitter        = nullptr;

    // Detail pane
    WaveformWidget* m_waveformWidget   = nullptr;
    QTableWidget*   m_metaTable        = nullptr;
    QLabel*         m_sbkrRefsLabel    = nullptr;

    // --- Audio ---
    AudioPlayer*              m_player           = nullptr;
    // Owned copy of the currently-loaded samples (kept so WAV export doesn't
    // need to re-read the file, and so the waveform widget always has valid data).
    std::vector<int16_t>      m_currentSamples;
    int                       m_currentSampleRate = 44100;

    // --- State ---
    QString        m_currentDirectory;
    QString        m_currentFilePath;
    int            m_currentSlotIndex  = -1;
    int            m_totalItems        = 0;
    int            m_visibleItems      = 0;

    // Re-entrancy guard: prevents crash from rapid clicks overlapping.
    bool           m_isSwitching       = false;
    bool           m_scanRunning       = false;

    // Map baseName → SbkrFile for cross-referencing SBKR entries by sound ID
    QHash<QString, gf::audio::SbkrFile> m_sbkrMap;

    // Cached scan results for context menus / re-use
    QList<AudioScanResult> m_scanResults;
};

} // namespace gf::gui
