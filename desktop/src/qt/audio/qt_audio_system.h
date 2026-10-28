#pragma once
#include <QObject>
#include <QAudioSink>
#include <QAudioFormat>
#include <QIODevice>
#include <vector>
#include <cmath>
#include <algorithm>

#include "core/pokewalker/peripherals/buzzer/buzzer.h"

#define AUDIO_SAMPLE_RATE 32000
#define AUDIO_BASE_AMPLITUDE (32768 / 2)
#define AUDIO_FLUSH_EVERY (AUDIO_SAMPLE_RATE / 100)
#define AUDIO_MIN_FREQUENCY 120.0f
#define AUDIO_FADE_SAMPLES (AUDIO_SAMPLE_RATE / 333)

#ifdef __ANDROID__
class AndroidAudioWorker;
class QThread;
#endif

class QtAudioSystem : public QObject
{
    Q_OBJECT

public:
    explicit QtAudioSystem(QObject* parent = nullptr);
    ~QtAudioSystem() override;

    void PushSample(BuzzerInformation info);

private:
    void Flush();

    QAudioSink* sink = nullptr;
    QIODevice* device = nullptr;
#ifdef __ANDROID__
    QThread* android_audio_thread = nullptr;
    AndroidAudioWorker* android_audio_worker = nullptr;
#endif

    std::vector<int16_t> buffer;
    float phase = 0.0f;
    float current_freq = 0.0f;
    int fade_counter = 0;
    bool playing = false;
};
