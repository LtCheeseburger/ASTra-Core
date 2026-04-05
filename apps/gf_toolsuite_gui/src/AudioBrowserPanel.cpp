#include "AudioBrowserPanel.hpp"

#include "AudioPlayer.hpp"
#include "WaveformWidget.hpp"

#include "gf/audio/sbkr_reader.hpp"
#include "gf/audio/sbbe_reader.hpp"

#include <QAction>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSplitter>
#include <QTableWidget>
#include <QThread>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

// ---------------------------------------------------------------------------
// Metatype registration for cross-thread signal — must be at global scope
// ---------------------------------------------------------------------------
Q_DECLARE_METATYPE(QList<gf::gui::AudioScanResult>)

// ---------------------------------------------------------------------------
// WAV writing helpers
// ---------------------------------------------------------------------------
static void wavWriteLE16(std::ostream& out, uint16_t v)
{
    const char bytes[2] = { static_cast<char>(v & 0xFFu),
                             static_cast<char>((v >> 8u) & 0xFFu) };
    out.write(bytes, 2);
}

static void wavWriteLE32(std::ostream& out, uint32_t v)
{
    const char bytes[4] = { static_cast<char>(v & 0xFFu),
                             static_cast<char>((v >>  8u) & 0xFFu),
                             static_cast<char>((v >> 16u) & 0xFFu),
                             static_cast<char>((v >> 24u) & 0xFFu) };
    out.write(bytes, 4);
}

static bool writeWavFile(const QString&              outPath,
                         const std::vector<int16_t>& samples,
                         int                         sampleRate)
{
    std::ofstream ofs(outPath.toStdString(), std::ios::binary);
    if (!ofs.is_open()) return false;

    const uint32_t dataSize = static_cast<uint32_t>(samples.size() * 2u);
    const uint32_t riffSize = 4u + 8u + 16u + 8u + dataSize;
    const uint32_t byteRate = static_cast<uint32_t>(sampleRate) * 2u;

    ofs.write("RIFF", 4);  wavWriteLE32(ofs, riffSize);
    ofs.write("WAVE", 4);
    ofs.write("fmt ", 4);  wavWriteLE32(ofs, 16u);
    wavWriteLE16(ofs, 1u); wavWriteLE16(ofs, 1u);
    wavWriteLE32(ofs, static_cast<uint32_t>(sampleRate));
    wavWriteLE32(ofs, byteRate);
    wavWriteLE16(ofs, 2u); wavWriteLE16(ofs, 16u);
    ofs.write("data", 4);  wavWriteLE32(ofs, dataSize);
    ofs.write(reinterpret_cast<const char*>(samples.data()),
              static_cast<std::streamsize>(dataSize));
    return ofs.good();
}

static QString fmtDuration(double secs)
{
    return QString::number(secs, 'f', 3) + QStringLiteral("s");
}

// ---------------------------------------------------------------------------
// AudioScanWorker — at global scope so MOC can process it
// ---------------------------------------------------------------------------
class AudioScanWorker final : public QThread {
    Q_OBJECT
public:
    QString directory;

signals:
    void scanFinished(QList<gf::gui::AudioScanResult> results);

protected:
    void run() override
    {
        QDir dir(directory);
        if (!dir.exists()) { emit scanFinished({}); return; }

        const QStringList allFiles = dir.entryList(QDir::Files, QDir::Name);

        QHash<QString, QStringList> byBase;
        for (const QString& fname : allFiles) {
            const QFileInfo fi(fname);
            byBase[fi.completeBaseName().toLower()].append(dir.absoluteFilePath(fname));
        }

        QList<gf::gui::AudioScanResult> results;
        for (auto it = byBase.cbegin(); it != byBase.cend(); ++it) {
            gf::gui::AudioScanResult r;
            for (const QString& fpath : it.value()) {
                const QString suffix = QFileInfo(fpath).suffix().toLower();

                if (suffix == QStringLiteral("sbbe") || suffix == QStringLiteral("sbse")) {
                    auto parsed = gf::audio::SbbeReader::read(fpath.toStdString());
                    if (parsed && parsed->valid && !r.hasSbbe) {
                        r.sbbeFile = std::move(*parsed);
                        r.hasSbbe  = true;
                    }
                } else if (suffix == QStringLiteral("sbkr")) {
                    auto parsed = gf::audio::SbkrReader::read(fpath.toStdString());
                    if (parsed && parsed->valid && !r.hasSbkr) {
                        r.sbkrFile = std::move(*parsed);
                        r.hasSbkr  = true;
                    }
                } else {
                    if (!r.hasSbbe) {
                        auto sbbe = gf::audio::SbbeReader::read(fpath.toStdString());
                        if (sbbe && sbbe->valid) { r.sbbeFile = std::move(*sbbe); r.hasSbbe = true; continue; }
                    }
                    if (!r.hasSbkr) {
                        auto sbkr = gf::audio::SbkrReader::read(fpath.toStdString());
                        if (sbkr && sbkr->valid) { r.sbkrFile = std::move(*sbkr); r.hasSbkr = true; }
                    }
                }
            }
            if (r.hasSbbe || r.hasSbkr) results.append(r);
        }

        emit scanFinished(results);
    }
};

// MOC include for AudioScanWorker (must come after its definition, before any namespace)
#include "AudioBrowserPanel.moc"

// ---------------------------------------------------------------------------
// AudioBrowserPanel — namespace gf::gui
// ---------------------------------------------------------------------------
namespace gf::gui {

// Item data roles
static constexpr int kRoleType     = Qt::UserRole;
static constexpr int kRoleFilePath = Qt::UserRole + 1;
static constexpr int kRoleIndex    = Qt::UserRole + 2;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AudioBrowserPanel::AudioBrowserPanel(QWidget* parent)
    : QDockWidget(QStringLiteral("Audio Browser"), parent)
    , m_player(new AudioPlayer(this))
{
    qRegisterMetaType<QList<AudioScanResult>>("QList<gf::gui::AudioScanResult>");

    buildUi();

    // All connections queued: the AudioPlayer background thread emits signals
    // that arrive here on the UI thread.
    connect(m_player, &AudioPlayer::progressChanged,
            this,     &AudioBrowserPanel::onPlaybackProgress,
            Qt::QueuedConnection);
    connect(m_player, &AudioPlayer::playbackStopped,
            this,     &AudioBrowserPanel::onPlaybackStopped,
            Qt::QueuedConnection);

    setAllowedAreas(Qt::AllDockWidgetAreas);
}

AudioBrowserPanel::~AudioBrowserPanel()
{
    // Stop any playback before widgets are destroyed.
    // AudioPlayer destructor also calls stopAndJoin(), but doing it here first
    // avoids any signal dispatching after the panel is partially torn down.
    if (m_player) m_player->stop();
}

// ---------------------------------------------------------------------------
// buildUi
// ---------------------------------------------------------------------------
void AudioBrowserPanel::buildUi()
{
    m_centralWidget = new QWidget(this);
    setWidget(m_centralWidget);

    QVBoxLayout* mainLayout = new QVBoxLayout(m_centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Toolbar
    m_toolbar = new QToolBar(this);
    m_toolbar->setIconSize(QSize(16, 16));

    QAction* scanAction = m_toolbar->addAction(QStringLiteral("Scan Directory"));
    scanAction->setToolTip(QStringLiteral("Choose a directory and scan for audio files"));
    connect(scanAction, &QAction::triggered, this, &AudioBrowserPanel::onScanClicked);

    m_toolbar->addSeparator();

    QAction* stopAction = m_toolbar->addAction(QStringLiteral("Stop"));
    stopAction->setToolTip(QStringLiteral("Stop playback"));
    connect(stopAction, &QAction::triggered, this, &AudioBrowserPanel::onStopPlayback);

    QAction* exportAction = m_toolbar->addAction(QStringLiteral("Export WAV"));
    exportAction->setToolTip(QStringLiteral("Export selected slot as WAV file"));
    connect(exportAction, &QAction::triggered, this, &AudioBrowserPanel::onExportWav);

    mainLayout->addWidget(m_toolbar);

    // Search bar
    QWidget* searchRow       = new QWidget(m_centralWidget);
    QHBoxLayout* searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(4, 2, 4, 2);
    searchLayout->setSpacing(4);

    QLabel* searchLabel = new QLabel(QStringLiteral("Filter:"), searchRow);
    m_searchEdit = new QLineEdit(searchRow);
    m_searchEdit->setPlaceholderText(QStringLiteral("Name, Sound ID, Global ID, Duration\u2026"));
    m_countLabel = new QLabel(QStringLiteral("0 / 0"), searchRow);
    m_countLabel->setMinimumWidth(60);

    searchLayout->addWidget(searchLabel);
    searchLayout->addWidget(m_searchEdit, 1);
    searchLayout->addWidget(m_countLabel);
    mainLayout->addWidget(searchRow);

    connect(m_searchEdit, &QLineEdit::textChanged,
            this,         &AudioBrowserPanel::onSearchChanged);

    // Main splitter: tree on top, detail on bottom
    m_splitter = new QSplitter(Qt::Vertical, m_centralWidget);

    m_tree = new QTreeWidget(m_splitter);
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({ QStringLiteral("Name"),
                               QStringLiteral("Duration"),
                               QStringLiteral("Sample Rate"),
                               QStringLiteral("Global ID"),
                               QStringLiteral("Status") });
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSortingEnabled(false);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);

    connect(m_tree, &QTreeWidget::itemClicked,
            this,   &AudioBrowserPanel::onItemClicked);
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this,   &AudioBrowserPanel::onItemDoubleClicked);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this,   &AudioBrowserPanel::onContextMenu);

    m_splitter->addWidget(m_tree);

    QWidget* detailPane = new QWidget(m_splitter);
    buildDetailPane(detailPane);
    m_splitter->addWidget(detailPane);
    m_splitter->setStretchFactor(0, 2);
    m_splitter->setStretchFactor(1, 1);

    mainLayout->addWidget(m_splitter, 1);
}

void AudioBrowserPanel::buildDetailPane(QWidget* parent)
{
    QVBoxLayout* layout = new QVBoxLayout(parent);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    m_waveformWidget = new WaveformWidget(parent);
    m_waveformWidget->setMinimumHeight(64);
    layout->addWidget(m_waveformWidget);

    m_metaTable = new QTableWidget(0, 2, parent);
    m_metaTable->setHorizontalHeaderLabels({ QStringLiteral("Property"),
                                             QStringLiteral("Value") });
    m_metaTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_metaTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_metaTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_metaTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_metaTable->setMaximumHeight(120);
    layout->addWidget(m_metaTable);

    m_sbkrRefsLabel = new QLabel(parent);
    m_sbkrRefsLabel->setWordWrap(true);
    layout->addWidget(m_sbkrRefsLabel);
}

// ---------------------------------------------------------------------------
// scan / loadFile
// ---------------------------------------------------------------------------

void AudioBrowserPanel::scan(const QString& directory)
{
    if (directory.isEmpty() || m_scanRunning) return;

    // Stop playback before wiping the catalog — avoids any reference to the
    // old scan results from a live playback thread (samples are already owned
    // by AudioPlayer, but clearing m_scanResults while iterating is unsafe).
    stopCurrentPlayback();

    m_currentDirectory = directory;
    m_tree->clear();
    m_scanResults.clear();
    m_sbkrMap.clear();
    m_currentSamples.clear();
    m_waveformWidget->clearSamples();
    m_metaTable->setRowCount(0);
    m_sbkrRefsLabel->clear();
    m_currentFilePath.clear();
    m_currentSlotIndex = -1;
    m_scanRunning      = true;

    AudioScanWorker* worker = new AudioScanWorker();
    worker->directory = directory;

    connect(worker, &AudioScanWorker::scanFinished,
            this,   [this](QList<gf::gui::AudioScanResult> results)
    {
        m_scanRunning = false;
        m_scanResults = std::move(results);
        m_sbkrMap.clear();
        for (const AudioScanResult& r : m_scanResults) {
            if (r.hasSbkr)
                m_sbkrMap.insert(QString::fromStdString(r.sbkrFile.baseName), r.sbkrFile);
        }
        populateCatalog(m_scanResults);
    });

    connect(worker, &AudioScanWorker::finished, worker, &QObject::deleteLater);
    worker->start();
}

void AudioBrowserPanel::loadFile(const QString& filePath)
{
    if (filePath.isEmpty() || m_scanRunning) return;

    stopCurrentPlayback();

    m_currentDirectory = QFileInfo(filePath).absolutePath();
    m_tree->clear();
    m_scanResults.clear();
    m_sbkrMap.clear();
    m_currentSamples.clear();
    m_waveformWidget->clearSamples();
    m_metaTable->setRowCount(0);
    m_sbkrRefsLabel->clear();
    m_currentFilePath.clear();
    m_currentSlotIndex = -1;
    m_totalItems       = 0;
    m_visibleItems     = 0;

    AudioScanResult r;
    const std::string stdPath = filePath.toStdString();

    auto sbbe = gf::audio::SbbeReader::read(stdPath);
    if (sbbe && sbbe->valid) { r.sbbeFile = std::move(*sbbe); r.hasSbbe = true; }

    auto sbkr = gf::audio::SbkrReader::read(stdPath);
    if (sbkr && sbkr->valid) {
        r.sbkrFile = std::move(*sbkr);
        r.hasSbkr  = true;
        m_sbkrMap.insert(QString::fromStdString(r.sbkrFile.baseName), r.sbkrFile);
    }

    if (r.hasSbbe || r.hasSbkr) {
        m_scanResults = {r};
        populateCatalog(m_scanResults);
    }
}

// ---------------------------------------------------------------------------
// onScanClicked
// ---------------------------------------------------------------------------

void AudioBrowserPanel::onScanClicked()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select Audio Directory"),
        m_currentDirectory.isEmpty() ? QDir::homePath() : m_currentDirectory,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!dir.isEmpty()) {
        emit scanRequested(dir);
        scan(dir);
    }
}

// ---------------------------------------------------------------------------
// populateCatalog
// ---------------------------------------------------------------------------

void AudioBrowserPanel::populateCatalog(const QList<AudioScanResult>& results)
{
    m_tree->clear();
    m_totalItems   = 0;
    m_visibleItems = 0;

    for (const AudioScanResult& sr : results) {
        const QString displayName = sr.hasSbbe
            ? QString::fromStdString(sr.sbbeFile.baseName)
            : QString::fromStdString(sr.sbkrFile.baseName);

        QTreeWidgetItem* topItem = new QTreeWidgetItem(m_tree);
        topItem->setText(0, displayName);
        topItem->setData(0, kRoleType,
                         sr.hasSbbe ? QStringLiteral("file_sbbe") : QStringLiteral("file_sbkr"));
        topItem->setData(0, kRoleFilePath,
                         sr.hasSbbe
                             ? QString::fromStdString(sr.sbbeFile.filePath)
                             : QString::fromStdString(sr.sbkrFile.filePath));

        if (sr.hasSbbe) {
            topItem->setText(4, QStringLiteral("SBbe"));
            int slotIdx = 0;
            for (const gf::audio::SbbeSlot& slot : sr.sbbeFile.audioSlots) {
                QTreeWidgetItem* slotItem = new QTreeWidgetItem(topItem);
                slotItem->setText(0, QStringLiteral("Slot %1").arg(slot.index));
                slotItem->setText(1, fmtDuration(slot.duration()));
                slotItem->setText(2, QString::number(slot.sampleRate) + QStringLiteral(" Hz"));
                slotItem->setText(3, slot.globalId != 0
                                       ? QStringLiteral("0x") + QString::number(slot.globalId, 16).toUpper()
                                       : QStringLiteral("\u2014"));
                slotItem->setText(4, QStringLiteral("PCM"));
                slotItem->setData(0, kRoleType,    QStringLiteral("slot"));
                slotItem->setData(0, kRoleFilePath, QString::fromStdString(sr.sbbeFile.filePath));
                slotItem->setData(0, kRoleIndex,   slotIdx);
                ++slotIdx;
                ++m_totalItems;
            }
        }

        if (sr.hasSbkr) {
            if (!sr.hasSbbe) topItem->setText(4, QStringLiteral("SBKR"));
            int entryIdx = 0;
            for (const gf::audio::SbkrEntry& entry : sr.sbkrFile.entries) {
                QTreeWidgetItem* entryItem = new QTreeWidgetItem(topItem);
                entryItem->setText(0, QStringLiteral("SoundID 0x%1")
                                       .arg(entry.soundId, 4, 16, QLatin1Char('0')).toUpper());
                entryItem->setText(1, QStringLiteral("\u2014"));
                entryItem->setText(2, QStringLiteral("\u2014"));
                entryItem->setText(3, QStringLiteral("\u2014"));
                entryItem->setText(4, QStringLiteral("SBKR ref"));
                entryItem->setData(0, kRoleType,    QStringLiteral("sbkr_entry"));
                entryItem->setData(0, kRoleFilePath, QString::fromStdString(sr.sbkrFile.filePath));
                entryItem->setData(0, kRoleIndex,   entryIdx);
                ++entryIdx;
                ++m_totalItems;
            }
        }

        ++m_totalItems;
        m_tree->expandItem(topItem);
    }

    m_visibleItems = m_totalItems;
    updateCountLabel();
}

// ---------------------------------------------------------------------------
// activateSlot — central slot-activation logic; guarded against re-entrancy
// ---------------------------------------------------------------------------

void AudioBrowserPanel::activateSlot(const QString& filePath, int slotIndex)
{
    // Re-entrancy guard: rapid clicks are silently ignored while we're
    // in the middle of loading/starting a previous slot.
    if (m_isSwitching) {
        std::fprintf(stderr, "[AUDIO] activateSlot: re-entrant call ignored (still switching)\n");
        return;
    }
    m_isSwitching = true;

    // 1. Stop previous playback synchronously FIRST.
    //    After this returns, no WinMM handle is open and no thread is running.
    stopCurrentPlayback();

    // 2. Find the slot in the cached scan results.
    const gf::audio::SbbeSlot* targetSlot = nullptr;
    const AudioScanResult*     targetSr   = nullptr;

    for (const AudioScanResult& sr : m_scanResults) {
        if (!sr.hasSbbe) continue;
        if (QString::fromStdString(sr.sbbeFile.filePath) != filePath) continue;
        if (slotIndex < 0 || slotIndex >= static_cast<int>(sr.sbbeFile.audioSlots.size())) break;
        targetSlot = &sr.sbbeFile.audioSlots[static_cast<std::size_t>(slotIndex)];
        targetSr   = &sr;
        break;
    }

    if (!targetSlot || !targetSr) {
        std::fprintf(stderr, "[AUDIO] activateSlot: slot %d not found in %s\n",
                     slotIndex, filePath.toStdString().c_str());
        m_isSwitching = false;
        return;
    }

    // 3. Read PCM samples from file. This is the only file I/O here.
    //    If it fails, we abort cleanly without touching the player.
    std::string errStr;
    std::vector<int16_t> samples = gf::audio::SbbeReader::readSlotAudio(
        filePath.toStdString(), *targetSlot, &errStr);

    if (samples.empty()) {
        std::fprintf(stderr, "[AUDIO] activateSlot: read failed: %s\n", errStr.c_str());
        QMessageBox::warning(this, QStringLiteral("Playback Error"),
                             QStringLiteral("Failed to read audio:\n%1")
                                 .arg(QString::fromStdString(errStr)));
        m_isSwitching = false;
        return;
    }

    // 4. Update waveform BEFORE starting playback so the UI is consistent.
    m_currentSamples    = samples;             // keep owned copy for export
    m_currentSampleRate = static_cast<int>(targetSlot->sampleRate);
    m_currentFilePath   = filePath;
    m_currentSlotIndex  = slotIndex;

    m_waveformWidget->setSamples(m_currentSamples, m_currentSampleRate);

    // 5. Populate metadata table.
    m_metaTable->setRowCount(0);
    m_sbkrRefsLabel->clear();

    auto addRow = [this](const QString& key, const QString& val) {
        const int row = m_metaTable->rowCount();
        m_metaTable->insertRow(row);
        m_metaTable->setItem(row, 0, new QTableWidgetItem(key));
        m_metaTable->setItem(row, 1, new QTableWidgetItem(val));
    };

    addRow(QStringLiteral("Slot Index"),  QString::number(targetSlot->index));
    addRow(QStringLiteral("Global ID"),   targetSlot->globalId != 0
                                            ? QStringLiteral("0x") + QString::number(targetSlot->globalId, 16).toUpper()
                                            : QStringLiteral("(none)"));
    addRow(QStringLiteral("Sample Rate"), QString::number(targetSlot->sampleRate) + QStringLiteral(" Hz"));
    addRow(QStringLiteral("Duration"),    fmtDuration(targetSlot->duration()));
    addRow(QStringLiteral("Audio Bytes"), QString::number(targetSlot->audioBytes()));
    addRow(QStringLiteral("Audio Start"), QStringLiteral("0x") + QString::number(targetSlot->audioStart(), 16).toUpper());
    addRow(QStringLiteral("Audio End"),   QStringLiteral("0x") + QString::number(targetSlot->audioEnd(),   16).toUpper());
    addRow(QStringLiteral("File"),        filePath);

    // SBKR cross-reference
    if (targetSlot->globalId != 0) {
        QStringList refs;
        for (auto mapIt = m_sbkrMap.cbegin(); mapIt != m_sbkrMap.cend(); ++mapIt) {
            for (const gf::audio::SbkrEntry& e : mapIt.value().entries) {
                if (static_cast<uint32_t>(e.soundId) == targetSlot->globalId)
                    refs.append(QStringLiteral("%1: SoundID 0x%2")
                                    .arg(mapIt.key())
                                    .arg(e.soundId, 4, 16, QLatin1Char('0')).toUpper());
            }
        }
        if (!refs.isEmpty())
            m_sbkrRefsLabel->setText(QStringLiteral("SBKR refs: ") + refs.join(QStringLiteral(", ")));
    }

    // 6. Start playback. AudioPlayer::play() takes ownership of a copy of the
    //    samples — it will not touch m_currentSamples after this call.
    m_player->play(std::vector<int16_t>(m_currentSamples), m_currentSampleRate);

    m_isSwitching = false;
}

// ---------------------------------------------------------------------------
// onItemClicked / onItemDoubleClicked
// ---------------------------------------------------------------------------

void AudioBrowserPanel::onItemClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (!item) return;
    const QString type = item->data(0, kRoleType).toString();
    if (type == QStringLiteral("slot")) {
        activateSlot(item->data(0, kRoleFilePath).toString(),
                     item->data(0, kRoleIndex).toInt());
    } else if (type == QStringLiteral("sbkr_entry")) {
        // Show SBKR entry metadata, no playback.
        stopCurrentPlayback();
        m_metaTable->setRowCount(0);
        m_sbkrRefsLabel->clear();
        m_waveformWidget->clearSamples();

        const QString filePath = item->data(0, kRoleFilePath).toString();
        const int     idx      = item->data(0, kRoleIndex).toInt();

        for (const AudioScanResult& sr : m_scanResults) {
            if (!sr.hasSbkr) continue;
            if (QString::fromStdString(sr.sbkrFile.filePath) != filePath) continue;
            if (idx < 0 || idx >= static_cast<int>(sr.sbkrFile.entries.size())) break;

            const gf::audio::SbkrEntry& entry = sr.sbkrFile.entries[static_cast<std::size_t>(idx)];
            auto addRow = [this](const QString& key, const QString& val) {
                const int row = m_metaTable->rowCount();
                m_metaTable->insertRow(row);
                m_metaTable->setItem(row, 0, new QTableWidgetItem(key));
                m_metaTable->setItem(row, 1, new QTableWidgetItem(val));
            };
            addRow(QStringLiteral("Sound ID"),
                   QStringLiteral("0x%1 (%2)")
                       .arg(entry.soundId, 4, 16, QLatin1Char('0')).toUpper()
                       .arg(entry.soundId));
            addRow(QStringLiteral("Flags"),    QString::number(entry.flags));
            addRow(QStringLiteral("Extra[0]"), QString::number(entry.extra[0]));
            addRow(QStringLiteral("Extra[1]"), QString::number(entry.extra[1]));
            addRow(QStringLiteral("Extra[2]"), QString::number(entry.extra[2]));
            addRow(QStringLiteral("Extra[3]"), QString::number(entry.extra[3]));
            addRow(QStringLiteral("File"),     filePath);
            break;
        }
    }
}

void AudioBrowserPanel::onItemDoubleClicked(QTreeWidgetItem* item, int column)
{
    // Double-click on a slot restarts playback from the beginning.
    if (!item) return;
    if (item->data(0, kRoleType).toString() == QStringLiteral("slot"))
        onItemClicked(item, column);
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

void AudioBrowserPanel::stopCurrentPlayback()
{
    // This is synchronous — after return, the WinMM thread is fully dead.
    if (m_player) m_player->stop();
    // Reset waveform cursor.
    if (m_waveformWidget) m_waveformWidget->setPlaybackProgress(-1.0f);
}

void AudioBrowserPanel::onStopPlayback()   { stopCurrentPlayback(); }

void AudioBrowserPanel::onPlaybackProgress(float progress)
{
    m_waveformWidget->setPlaybackProgress(progress);
}

void AudioBrowserPanel::onPlaybackStopped()
{
    m_waveformWidget->setPlaybackProgress(-1.0f);
}

// ---------------------------------------------------------------------------
// WAV export — uses the owned m_currentSamples (no file re-read needed)
// ---------------------------------------------------------------------------

void AudioBrowserPanel::onExportWav()
{
    if (m_currentSamples.empty() || m_currentSlotIndex < 0) {
        QMessageBox::information(this, QStringLiteral("Export WAV"),
                                 QStringLiteral("Select a slot first."));
        return;
    }

    // Derive a default filename from scan results.
    QString defaultName = QStringLiteral("slot%1.wav").arg(m_currentSlotIndex);
    for (const AudioScanResult& sr : m_scanResults) {
        if (!sr.hasSbbe) continue;
        if (QString::fromStdString(sr.sbbeFile.filePath) != m_currentFilePath) continue;
        defaultName = QStringLiteral("%1_slot%2.wav")
                          .arg(QString::fromStdString(sr.sbbeFile.baseName))
                          .arg(m_currentSlotIndex);
        break;
    }

    const QString outPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export WAV"), defaultName,
        QStringLiteral("WAV files (*.wav)"));
    if (outPath.isEmpty()) return;

    if (!writeWavFile(outPath, m_currentSamples, m_currentSampleRate))
        QMessageBox::critical(this, QStringLiteral("Export Error"),
                              QStringLiteral("Failed to write: ") + outPath);
}

// ---------------------------------------------------------------------------
// Search / filter
// ---------------------------------------------------------------------------

void AudioBrowserPanel::onSearchChanged(const QString& text)
{
    m_visibleItems = 0;
    const QString lower = text.trimmed().toLower();

    const int topCount = m_tree->topLevelItemCount();
    for (int t = 0; t < topCount; ++t) {
        QTreeWidgetItem* topItem = m_tree->topLevelItem(t);
        if (!topItem) continue;

        bool anyChildVisible = false;
        const int childCount = topItem->childCount();
        for (int c = 0; c < childCount; ++c) {
            QTreeWidgetItem* child = topItem->child(c);
            if (!child) continue;
            bool match = lower.isEmpty();
            if (!match) {
                for (int col = 0; col < m_tree->columnCount(); ++col) {
                    if (child->text(col).toLower().contains(lower)) { match = true; break; }
                }
            }
            child->setHidden(!match);
            if (match) { anyChildVisible = true; ++m_visibleItems; }
        }

        bool topMatch = lower.isEmpty();
        if (!topMatch) {
            for (int col = 0; col < m_tree->columnCount(); ++col) {
                if (topItem->text(col).toLower().contains(lower)) { topMatch = true; break; }
            }
        }
        const bool showTop = topMatch || anyChildVisible;
        topItem->setHidden(!showTop);
        if (showTop && childCount == 0) ++m_visibleItems;
    }

    updateCountLabel();
}

void AudioBrowserPanel::updateCountLabel()
{
    m_countLabel->setText(QStringLiteral("%1 / %2").arg(m_visibleItems).arg(m_totalItems));
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

void AudioBrowserPanel::onContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    if (!item) return;

    const QString type = item->data(0, kRoleType).toString();
    QMenu menu(this);

    if (type == QStringLiteral("slot")) {
        QAction* playAct = menu.addAction(QStringLiteral("Play"));
        connect(playAct, &QAction::triggered, this, [this, item]() {
            activateSlot(item->data(0, kRoleFilePath).toString(),
                         item->data(0, kRoleIndex).toInt());
        });

        QAction* stopAct = menu.addAction(QStringLiteral("Stop"));
        connect(stopAct, &QAction::triggered, this, &AudioBrowserPanel::onStopPlayback);

        menu.addSeparator();

        QAction* exportAct = menu.addAction(QStringLiteral("Export WAV\u2026"));
        connect(exportAct, &QAction::triggered, this, [this, item]() {
            activateSlot(item->data(0, kRoleFilePath).toString(),
                         item->data(0, kRoleIndex).toInt());
            onExportWav();
        });

        menu.addSeparator();

        const QString globalIdText = item->text(3);
        QAction* copyGlobalId = menu.addAction(
            QStringLiteral("Copy Global ID (%1)").arg(globalIdText));
        connect(copyGlobalId, &QAction::triggered, this, [globalIdText]() {
            QGuiApplication::clipboard()->setText(globalIdText);
        });
    }

    if (type == QStringLiteral("sbkr_entry")) {
        const QString soundIdText = item->text(0);
        QAction* copyId = menu.addAction(QStringLiteral("Copy Sound ID"));
        connect(copyId, &QAction::triggered, this, [soundIdText]() {
            QGuiApplication::clipboard()->setText(soundIdText);
        });
    }

    menu.addSeparator();

    QAction* explorerAct = menu.addAction(QStringLiteral("Show in Explorer"));
    connect(explorerAct, &QAction::triggered, this, [item]() {
        const QString fp = item->data(0, kRoleFilePath).toString();
        if (!fp.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(fp).absolutePath()));
    });

    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void AudioBrowserPanel::onCopySoundId()
{
    QTreeWidgetItem* cur = m_tree->currentItem();
    if (cur) QGuiApplication::clipboard()->setText(cur->text(0));
}

void AudioBrowserPanel::onShowInExplorer()
{
    QTreeWidgetItem* cur = m_tree->currentItem();
    if (!cur) return;
    const QString fp = cur->data(0, kRoleFilePath).toString();
    if (!fp.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(fp).absolutePath()));
}

// ---------------------------------------------------------------------------
// Key events
// ---------------------------------------------------------------------------

void AudioBrowserPanel::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space) {
        if (m_player->isPlaying()) {
            stopCurrentPlayback();
        } else if (!m_currentFilePath.isEmpty() && m_currentSlotIndex >= 0) {
            // Re-play last slot from its owned samples — no file re-read.
            if (!m_currentSamples.empty())
                m_player->play(std::vector<int16_t>(m_currentSamples), m_currentSampleRate);
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        stopCurrentPlayback();
        event->accept();
        return;
    }
    QDockWidget::keyPressEvent(event);
}

} // namespace gf::gui
