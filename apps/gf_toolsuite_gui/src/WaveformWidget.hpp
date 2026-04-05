#pragma once
#include <QWidget>
#include <cstdint>
#include <vector>

namespace gf::gui {

class WaveformWidget final : public QWidget {
    Q_OBJECT
public:
    explicit WaveformWidget(QWidget* parent = nullptr);

    void setSamples(const std::vector<int16_t>& samples, int sampleRate);
    void clearSamples();
    void setPlaybackProgress(float progress);  // 0.0 – 1.0; <0 hides cursor
    [[nodiscard]] QSize sizeHint() const override;

signals:
    void clicked(float position);  // 0.0 – 1.0, for future seeking

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    std::vector<int16_t> m_samples;
    int    m_sampleRate  = 44100;
    float  m_progress    = -1.0f;  // negative = no cursor
    double m_durationSec = 0.0;

    [[nodiscard]] std::vector<float> buildRmsBuckets(int bucketCount) const;
};

} // namespace gf::gui
