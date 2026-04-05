#include "AudioPlayer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

// WinMM — order matters on MinGW
#include <windows.h>
#include <mmsystem.h>

namespace gf::gui {

// ---------------------------------------------------------------------------
// AudioPlayThread
// ---------------------------------------------------------------------------

void AudioPlayThread::setSamples(std::vector<int16_t> samples, int sampleRate)
{
    m_samples    = std::move(samples);
    m_sampleRate = (sampleRate > 0) ? sampleRate : 44100;
    m_stopRequested.store(false, std::memory_order_relaxed);
}

void AudioPlayThread::requestStop() noexcept
{
    m_stopRequested.store(true, std::memory_order_release);
}

bool AudioPlayThread::stopRequested() const noexcept
{
    return m_stopRequested.load(std::memory_order_acquire);
}

void AudioPlayThread::run()
{
    std::fprintf(stderr, "[AUDIO] thread started (%zu samples @ %d Hz)\n",
                 m_samples.size(), m_sampleRate);

    if (m_samples.empty() || m_sampleRate <= 0) {
        std::fprintf(stderr, "[AUDIO] thread: empty samples, aborting\n");
        return;
    }

    WAVEFORMATEX fmt{};
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = 1;
    fmt.nSamplesPerSec  = static_cast<DWORD>(m_sampleRate);
    fmt.nAvgBytesPerSec = static_cast<DWORD>(m_sampleRate) * 2u;
    fmt.nBlockAlign     = 2;
    fmt.wBitsPerSample  = 16;
    fmt.cbSize          = 0;

    HWAVEOUT hwo = nullptr;
    if (waveOutOpen(&hwo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        std::fprintf(stderr, "[AUDIO] thread: waveOutOpen failed\n");
        return;
    }

    WAVEHDR hdr{};
    hdr.lpData         = reinterpret_cast<LPSTR>(m_samples.data());
    hdr.dwBufferLength = static_cast<DWORD>(m_samples.size() * 2u);
    hdr.dwFlags        = 0;

    if (waveOutPrepareHeader(hwo, &hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
        std::fprintf(stderr, "[AUDIO] thread: waveOutPrepareHeader failed\n");
        waveOutClose(hwo);
        return;
    }

    if (waveOutWrite(hwo, &hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
        std::fprintf(stderr, "[AUDIO] thread: waveOutWrite failed\n");
        waveOutUnprepareHeader(hwo, &hdr, sizeof(WAVEHDR));
        waveOutClose(hwo);
        return;
    }

    // Poll until done or stop requested.
    while (!(hdr.dwFlags & WHDR_DONE) && !m_stopRequested.load(std::memory_order_acquire)) {
        msleep(20);

        MMTIME mmt{};
        mmt.wType = TIME_SAMPLES;
        if (waveOutGetPosition(hwo, &mmt, sizeof(MMTIME)) == MMSYSERR_NOERROR &&
            mmt.wType == TIME_SAMPLES && !m_samples.empty()) {
            const float progress = std::clamp(
                static_cast<float>(mmt.u.sample) / static_cast<float>(m_samples.size()),
                0.0f, 1.0f);
            emit progressChanged(progress);
        }
    }

    // Always reset so the buffer is returned immediately, whether we were
    // stopped early or completed normally.
    waveOutReset(hwo);

    // Spin until WinMM returns the buffer (WHDR_DONE must be set).
    // In practice this is instant after waveOutReset, but be safe.
    for (int spin = 0; spin < 200 && !(hdr.dwFlags & WHDR_DONE); ++spin)
        msleep(5);

    waveOutUnprepareHeader(hwo, &hdr, sizeof(WAVEHDR));
    waveOutClose(hwo);

    std::fprintf(stderr, "[AUDIO] thread finished\n");
}

// ---------------------------------------------------------------------------
// AudioPlayer
// ---------------------------------------------------------------------------

AudioPlayer::AudioPlayer(QObject* parent)
    : QObject(parent)
{}

AudioPlayer::~AudioPlayer()
{
    stopAndJoin();
}

void AudioPlayer::play(std::vector<int16_t> samples, int sampleRate)
{
    std::fprintf(stderr, "[AUDIO] play requested (%zu samples)\n", samples.size());

    // Always stop previous session fully before touching anything.
    stopAndJoin();

    trimSilence(samples);
    if (samples.empty()) {
        std::fprintf(stderr, "[AUDIO] play: empty after silence trim, skipping\n");
        return;
    }

    std::fprintf(stderr, "[AUDIO] samples loaded (%zu samples)\n", samples.size());

    // Create thread. Ownership: this object owns the thread until stopAndJoin()
    // deletes it. We do NOT use deleteLater — we manage lifetime explicitly so
    // we can always join before any destruction.
    m_thread = new AudioPlayThread();
    m_thread->setSamples(std::move(samples), sampleRate);

    // All connections use Qt::QueuedConnection so the thread never touches UI.
    connect(m_thread, &AudioPlayThread::progressChanged,
            this,     &AudioPlayer::progressChanged,
            Qt::QueuedConnection);
    connect(m_thread, &AudioPlayThread::finished,
            this,     &AudioPlayer::playbackStopped,
            Qt::QueuedConnection);

    emit playbackStarted();
    m_thread->start();
    std::fprintf(stderr, "[AUDIO] thread started\n");
}

void AudioPlayer::stop()
{
    std::fprintf(stderr, "[AUDIO] stop requested\n");
    stopAndJoin();
}

bool AudioPlayer::isPlaying() const
{
    return m_thread != nullptr && m_thread->isRunning();
}

void AudioPlayer::stopAndJoin()
{
    if (!m_thread) return;

    std::fprintf(stderr, "[AUDIO] stopping current playback\n");

    // Signal the run() loop to call waveOutReset and exit.
    m_thread->requestStop();

    // Block until the thread has fully exited and all WinMM handles closed.
    // run() always calls waveOutReset + waveOutClose before returning, so
    // after wait() completes it is unconditionally safe to free sample memory.
    if (!m_thread->wait(5000)) {
        // Thread failed to exit within 5 s — force-terminate as last resort.
        // This should never happen in practice.
        std::fprintf(stderr, "[AUDIO] WARNING: thread did not exit in 5s, terminating\n");
        m_thread->terminate();
        m_thread->wait(1000);
    }

    // Thread is dead: safe to delete. Do NOT use deleteLater — we need
    // synchronous destruction so the sample buffer is freed now.
    delete m_thread;
    m_thread = nullptr;

    std::fprintf(stderr, "[AUDIO] playback stopped\n");
}

void AudioPlayer::trimSilence(std::vector<int16_t>& samples) noexcept
{
    // Find last sample whose absolute value is above the silence threshold.
    constexpr int kThreshold  = 32;
    constexpr int kTailSamples = 64;

    int last = -1;
    for (int i = static_cast<int>(samples.size()) - 1; i >= 0; --i) {
        if (std::abs(static_cast<int>(samples[static_cast<std::size_t>(i)])) >= kThreshold) {
            last = i;
            break;
        }
    }
    // Only truncate if there's a meaningful silent tail.
    if (last >= 0 && static_cast<int>(samples.size()) - 1 - last > kTailSamples)
        samples.resize(static_cast<std::size_t>(last + 1));
}

} // namespace gf::gui
