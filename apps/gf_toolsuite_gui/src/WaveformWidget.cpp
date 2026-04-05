#include "WaveformWidget.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QString>

#include <algorithm>
#include <cmath>

namespace gf::gui {

WaveformWidget::WaveformWidget(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(40);
}

QSize WaveformWidget::sizeHint() const
{
    return QSize(-1, 64);
}

void WaveformWidget::setSamples(const std::vector<int16_t>& samples, int sampleRate)
{
    m_samples    = samples;
    m_sampleRate = (sampleRate > 0) ? sampleRate : 44100;
    m_durationSec = (m_sampleRate > 0 && !m_samples.empty())
        ? static_cast<double>(m_samples.size()) / m_sampleRate
        : 0.0;
    m_progress = -1.0f;
    update();
}

void WaveformWidget::clearSamples()
{
    m_samples.clear();
    m_durationSec = 0.0;
    m_progress    = -1.0f;
    update();
}

void WaveformWidget::setPlaybackProgress(float progress)
{
    m_progress = progress;
    update();
}

std::vector<float> WaveformWidget::buildRmsBuckets(int bucketCount) const
{
    if (m_samples.empty() || bucketCount <= 0)
        return std::vector<float>(static_cast<std::size_t>(bucketCount), 0.0f);

    const std::size_t total   = m_samples.size();
    const std::size_t bCount  = static_cast<std::size_t>(bucketCount);
    std::vector<float> rms(bCount, 0.0f);

    float maxRms = 0.0f;
    for (std::size_t b = 0; b < bCount; ++b) {
        const std::size_t bStart = b * total / bCount;
        const std::size_t bEnd   = (b + 1u) * total / bCount;
        if (bStart >= bEnd) continue;

        double sumSq = 0.0;
        for (std::size_t i = bStart; i < bEnd; ++i) {
            const double v = static_cast<double>(m_samples[i]);
            sumSq += v * v;
        }
        const float r = static_cast<float>(std::sqrt(sumSq / static_cast<double>(bEnd - bStart)));
        rms[b] = r;
        if (r > maxRms) maxRms = r;
    }

    // Normalize 0..1
    if (maxRms > 0.0f) {
        for (float& v : rms)
            v /= maxRms;
    }

    return rms;
}

void WaveformWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const int w = width();
    const int h = height();

    // Background
    painter.fillRect(rect(), palette().window());

    if (m_samples.empty()) {
        painter.setPen(palette().windowText().color());
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No audio"));
        return;
    }

    // Build RMS buckets (one per pixel column)
    const std::vector<float> rms = buildRmsBuckets(w);
    const int centerY = h / 2;
    constexpr int kMaxHalfHeight = 30;

    const QColor waveColor = palette().highlight().color();
    painter.setPen(waveColor);

    for (int x = 0; x < w && x < static_cast<int>(rms.size()); ++x) {
        const int halfH = static_cast<int>(rms[static_cast<std::size_t>(x)] * kMaxHalfHeight);
        if (halfH <= 0) continue;
        painter.drawLine(x, centerY - halfH, x, centerY + halfH);
    }

    // Playback cursor
    if (m_progress >= 0.0f && m_progress <= 1.0f) {
        const int cursorX = static_cast<int>(m_progress * static_cast<float>(w));
        painter.setPen(Qt::white);
        painter.drawLine(cursorX, 0, cursorX, h - 1);
    }

    // Duration text at top-right
    if (m_durationSec > 0.0) {
        const QString durStr = QString::number(m_durationSec, 'f', 3) + QStringLiteral("s");
        painter.setPen(palette().windowText().color());
        const QRect textRect = rect().adjusted(0, 2, -4, 0);
        painter.drawText(textRect, Qt::AlignTop | Qt::AlignRight, durStr);
    }
}

void WaveformWidget::mousePressEvent(QMouseEvent* event)
{
    if (width() > 0) {
        const float pos = static_cast<float>(event->pos().x()) / static_cast<float>(width());
        emit clicked(std::clamp(pos, 0.0f, 1.0f));
    }
    QWidget::mousePressEvent(event);
}

} // namespace gf::gui
