#include "qt_audio_system.h"

#include <QMediaDevices>
#ifdef __ANDROID__
#include <cstring>
#include <mutex>
#else
#include <QThread>
#endif

#include "desktop/src/qt/settings/app_settings.h"

#ifdef __ANDROID__
namespace
{
constexpr int AUDIO_SINK_BUFFER_MS = 60;
constexpr int AUDIO_QUEUE_MS = 100;
}

class BufferedAudioDevice final : public QIODevice
{
public:
    explicit BufferedAudioDevice(const qsizetype capacity, QObject* parent = nullptr)
        : QIODevice(parent), ring(static_cast<size_t>(capacity))
    {
    }

    void Append(const char* data, qsizetype length)
    {
        if (!data || length <= 0 || ring.empty())
            return;

        {
            std::scoped_lock lock(mutex);
            size_t count = static_cast<size_t>(length);
            if (count >= ring.size())
            {
                data += count - ring.size();
                count = ring.size();
                read_offset = 0;
                buffered = 0;
            }

            const size_t overflow = buffered + count > ring.size()
                ? buffered + count - ring.size()
                : 0;
            read_offset = (read_offset + overflow) % ring.size();
            buffered -= overflow;

            const size_t write_offset = (read_offset + buffered) % ring.size();
            const size_t first = std::min(count, ring.size() - write_offset);
            std::memcpy(ring.data() + write_offset, data, first);
            if (count > first)
                std::memcpy(ring.data(), data + first, count - first);
            buffered += count;
        }

        Q_EMIT readyRead();
    }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        std::scoped_lock lock(mutex);
        return static_cast<qint64>(buffered) + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char* data, const qint64 max_length) override
    {
        if (!data || max_length <= 0)
            return 0;

        const size_t requested = static_cast<size_t>(max_length);
        size_t copied = 0;
        {
            std::scoped_lock lock(mutex);
            copied = std::min(requested, buffered);
            const size_t first = std::min(copied, ring.size() - read_offset);
            std::memcpy(data, ring.data() + read_offset, first);
            if (copied > first)
                std::memcpy(data + first, ring.data(), copied - first);
            read_offset = (read_offset + copied) % ring.size();
            buffered -= copied;
        }

        return static_cast<qint64>(copied);
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    mutable std::mutex mutex;
    std::vector<char> ring;
    size_t read_offset = 0;
    size_t buffered = 0;
};
#endif

QtAudioSystem::QtAudioSystem(QObject* parent) : QObject(parent)
{
    QAudioFormat fmt;
    fmt.setSampleRate(AUDIO_SAMPLE_RATE);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);

    sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), fmt, this);
#ifdef __ANDROID__
    sink->setBufferSize(AUDIO_SAMPLE_RATE * sizeof(int16_t) * AUDIO_SINK_BUFFER_MS / 1000);
    buffered_device = new BufferedAudioDevice(
        AUDIO_SAMPLE_RATE * sizeof(int16_t) * AUDIO_QUEUE_MS / 1000, this);
    buffered_device->open(QIODevice::ReadOnly);
    device = buffered_device;
    sink->start(buffered_device);
#else
    sink->setBufferSize(AUDIO_SAMPLE_RATE * sizeof(int16_t) / 10);
    device = sink->start();
#endif

    buffer.reserve(AUDIO_FLUSH_EVERY * 2);
}

QtAudioSystem::~QtAudioSystem()
{
    Flush();
#ifdef __ANDROID__
    sink->stop();
    device->close();
#else
    sink->disconnect();
    sink->stop();
    device = nullptr;
    QThread::msleep(50);
    delete sink;
    sink = nullptr;
#endif
}

void QtAudioSystem::PushSample(BuzzerInformation info)
{
    const bool is_active = info.frequency >= AUDIO_MIN_FREQUENCY;

    if (is_active && !playing)
    {
        current_freq = info.frequency;
        playing = true;
        fade_counter = 0;
    }
    else if (!is_active && playing)
    {
        playing = false;
        fade_counter = AUDIO_FADE_SAMPLES;
    }
    else if (is_active && info.frequency != current_freq)
    {
        current_freq = info.frequency;
    }

    float raw = 0.0f;
    if (current_freq >= AUDIO_MIN_FREQUENCY && (playing || fade_counter > 0))
    {
        const float amplitude = info.is_full_volume
                                    ? static_cast<float>(AUDIO_BASE_AMPLITUDE)
                                    : static_cast<float>(AUDIO_BASE_AMPLITUDE) / 2.0f;

#ifdef __ANDROID__
        raw = (phase < 0.5f ? 1.0f : -1.0f) * amplitude;
#else
        const float nyquist = AUDIO_SAMPLE_RATE / 2.0f;
        const int max_harmonic = static_cast<int>(std::floor(nyquist / current_freq));
        float sample = 0.0f;
        for (int h = 1; h <= max_harmonic; h += 2)
            sample += std::sin(2.0f * M_PI * static_cast<float>(h) * phase) / static_cast<float>(h);
        raw = sample * static_cast<float>(4.0f / M_PI) * amplitude;
#endif

        if (playing && fade_counter < AUDIO_FADE_SAMPLES)
        {
            const float t = static_cast<float>(fade_counter) / static_cast<float>(AUDIO_FADE_SAMPLES);
            raw *= t * t;
            fade_counter++;
        }
        else if (!playing && fade_counter > 0)
        {
            const float t = static_cast<float>(fade_counter) / static_cast<float>(AUDIO_FADE_SAMPLES);
            raw *= t * t;
            fade_counter--;
        }

        raw = std::clamp(raw, -32768.0f, 32767.0f) * AppSettings::instance.audio.volume;
    }

    phase += current_freq / static_cast<float>(AUDIO_SAMPLE_RATE);
    phase -= std::floor(phase);

    buffer.push_back(static_cast<int16_t>(raw));
    if (static_cast<int>(buffer.size()) >= AUDIO_FLUSH_EVERY)
        Flush();
}

void QtAudioSystem::Flush()
{
    if (buffer.empty() || !device) return;
#ifdef __ANDROID__
    buffered_device->Append(
        reinterpret_cast<const char*>(buffer.data()),
        static_cast<qsizetype>(buffer.size() * sizeof(int16_t)));
#else
    device->write(reinterpret_cast<const char*>(buffer.data()), buffer.size() * sizeof(int16_t));
#endif
    buffer.clear();
}
