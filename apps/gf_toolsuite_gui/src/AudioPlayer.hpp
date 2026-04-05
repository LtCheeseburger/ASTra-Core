#pragma once
#include <QObject>
#include <QThread>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace gf::gui {

// ---------------------------------------------------------------------------
// AudioPlayThread — internal; do not use directly outside AudioPlayer
//
// Owns its own copy of PCM samples. hdr.lpData points into m_samples which
// lives for the entire duration of run(). The thread signals progress and
// completion; it NEVER touches any UI widget.
// ---------------------------------------------------------------------------
class AudioPlayThread final : public QThread {
    Q_OBJECT
public:
    // Called before start(). Sets owned data. Thread-safe: called from
    // UI thread before start(), never mutated after that.
    void setSamples(std::vector<int16_t> samples, int sampleRate);

    // Request graceful stop. Safe to call from any thread at any time.
    void requestStop() noexcept;

    // Whether a stop has been requested (non-blocking check).
    [[nodiscard]] bool stopRequested() const noexcept;

signals:
    // 0.0–1.0; emitted from background thread via queued connection.
    void progressChanged(float progress);

protected:
    void run() override;

private:
    std::vector<int16_t> m_samples;
    int                  m_sampleRate = 44100;
    std::atomic<bool>    m_stopRequested{false};
};

// ---------------------------------------------------------------------------
// AudioPlayer — single-instance waveOut player
//
// Thread safety: all public methods must be called from the UI (main) thread.
//
// Lifecycle contract:
//   play()  — stops any running session synchronously, then starts new one.
//   stop()  — stops running session synchronously; no-op if idle.
//
// "Synchronously stopped" means the WinMM waveOut handle has been closed and
// the background thread has fully exited before the call returns. This makes
// it safe to call play() immediately after stop() without races.
// ---------------------------------------------------------------------------
class AudioPlayer final : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer() override;

    // Takes ownership of a sample copy. Trims trailing silence, then plays.
    // Always stops the previous session first (blocking).
    void play(std::vector<int16_t> samples, int sampleRate);

    // Stop playback synchronously. Safe to call when already stopped.
    void stop();

    [[nodiscard]] bool isPlaying() const;

signals:
    void playbackStarted();
    void playbackStopped();
    void progressChanged(float progress);   // 0.0–1.0

private:
    // Stops the running thread synchronously, then deletes it.
    // After return: m_thread == nullptr, no WinMM handle is open.
    void stopAndJoin();

    // Trim trailing silence in-place (abs(sample) < threshold for > kTailSamples).
    static void trimSilence(std::vector<int16_t>& samples) noexcept;

    AudioPlayThread* m_thread = nullptr;
};

} // namespace gf::gui
