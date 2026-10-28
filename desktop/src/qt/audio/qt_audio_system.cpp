#include "qt_audio_system.h"

#include <QMediaDevices>
#include <QThread>
#ifdef __ANDROID__
#include <QByteArray>
#include <QTimer>
#endif

#include "desktop/src/qt/settings/app_settings.h"

#ifdef __ANDROID__
namespace
{
constexpr int AUDIO_SINK_BUFFER_MS = 120;
constexpr int AUDIO_QUEUE_MS = 300;
constexpr int AUDIO_DRAIN_INTERVAL_MS = 5;

constexpr qsizetype AudioBytesForMs(const int milliseconds)
{
    return AUDIO_SAMPLE_RATE * static_cast<qsizetype>(sizeof(int16_t)) * milliseconds / 1000;
}
}

class AndroidAudioWorker final : public QObject
{
public:
    void Initialize()
    {
        QAudioFormat format;
        format.setSampleRate(AUDIO_SAMPLE_RATE);
        format.setChannelCount(1);
        format.setSampleFormat(QAudioFormat::Int16);

        sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), format, this);
        sink->setBufferSize(AudioBytesForMs(AUDIO_SINK_BUFFER_MS));
        device = sink->start();

        drain_timer = new QTimer(this);
        drain_timer->setTimerType(Qt::PreciseTimer);
        drain_timer->setInterval(AUDIO_DRAIN_INTERVAL_MS);
        connect(drain_timer, &QTimer::timeout, this, [this] { Drain(); });
        drain_timer->start();
        Drain();
    }

    void Enqueue(QByteArray samples)
    {
        if (samples.isEmpty())
            return;

        pending.append(samples);
        const qsizetype capacity = AudioBytesForMs(AUDIO_QUEUE_MS);
        if (pending.size() > capacity)
            pending.remove(0, pending.size() - capacity);
        Drain();
    }

    void Shutdown()
    {
        if (drain_timer)
            drain_timer->stop();
        if (sink)
        {
            sink->stop();
            delete sink;
            sink = nullptr;
            device = nullptr;
        }
        pending.clear();
    }

private:
    void Drain()
    {
        if (!sink || !device || pending.isEmpty())
            return;

        qint64 writable = sink->bytesFree();
        while (writable > 0 && !pending.isEmpty())
        {
            const qint64 requested = std::min<qint64>(writable, pending.size());
            const qint64 written = device->write(pending.constData(), requested);
            if (written <= 0)
                break;
            pending.remove(0, written);
            writable = sink->bytesFree();
        }
    }

    QAudioSink* sink = nullptr;
    QIODevice* device = nullptr;
    QTimer* drain_timer = nullptr;
    QByteArray pending;
};
#endif

QtAudioSystem::QtAudioSystem(QObject* parent) : QObject(parent)
{
#ifdef __ANDROID__
    android_audio_thread = new QThread(this);
    android_audio_worker = new AndroidAudioWorker();
    android_audio_worker->moveToThread(android_audio_thread);
    connect(android_audio_thread, &QThread::started, android_audio_worker,
            [worker = android_audio_worker] { worker->Initialize(); });
    connect(android_audio_thread, &QThread::finished,
            android_audio_worker, &QObject::deleteLater);
    android_audio_thread->start();
#else
    QAudioFormat fmt;
    fmt.setSampleRate(AUDIO_SAMPLE_RATE);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);

    sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), fmt, this);
    sink->setBufferSize(AUDIO_SAMPLE_RATE * sizeof(int16_t) / 10);
    device = sink->start();
#endif

    buffer.reserve(AUDIO_FLUSH_EVERY * 2);
}

QtAudioSystem::~QtAudioSystem()
{
    Flush();
#ifdef __ANDROID__
    if (android_audio_worker && android_audio_thread && android_audio_thread->isRunning())
    {
        QMetaObject::invokeMethod(
            android_audio_worker,
            [worker = android_audio_worker] { worker->Shutdown(); },
            Qt::BlockingQueuedConnection);
        android_audio_thread->quit();
        android_audio_thread->wait();
    }
    android_audio_worker = nullptr;
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
    if (buffer.empty()) return;
#ifdef __ANDROID__
    if (android_audio_worker)
    {
        QByteArray samples(
            reinterpret_cast<const char*>(buffer.data()),
            static_cast<qsizetype>(buffer.size() * sizeof(int16_t)));
        QMetaObject::invokeMethod(
            android_audio_worker,
            [worker = android_audio_worker, samples = std::move(samples)]() mutable
            {
                worker->Enqueue(std::move(samples));
            },
            Qt::QueuedConnection);
    }
#else
    if (!device) return;
    device->write(reinterpret_cast<const char*>(buffer.data()), buffer.size() * sizeof(int16_t));
#endif
    buffer.clear();
}
