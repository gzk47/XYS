#include "sound.hpp"

#include <SDL.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace sound {
namespace {

constexpr int kSampleRate = 48000;

struct Tone {
    double freq;   // Hz
    double ms;     // duration of this segment
    double gain;   // 0..1
};

SDL_AudioDeviceID g_device = 0;
std::vector<int16_t> g_samples[5];

// A short sine segment with a linear attack/decay envelope to avoid clicks.
void appendTone(std::vector<int16_t>& out, const Tone& tone) {
    const int count = static_cast<int>(kSampleRate * tone.ms / 1000.0);
    const int attack = count / 10 > 0 ? count / 10 : 1;
    const int release = count / 3 > 0 ? count / 3 : 1;
    const double step = 2.0 * 3.14159265358979323846 * tone.freq / kSampleRate;
    for (int i = 0; i < count; ++i) {
        double env = 1.0;
        if (i < attack) env = static_cast<double>(i) / attack;
        if (i > count - release) {
            env = static_cast<double>(count - i) / release;
        }
        const double v = std::sin(step * i) * tone.gain * env;
        out.push_back(static_cast<int16_t>(v * 32767.0));
    }
}

std::vector<int16_t> make(std::initializer_list<Tone> tones) {
    std::vector<int16_t> out;
    for (const auto& tone : tones) appendTone(out, tone);
    return out;
}

}  // namespace

bool init() {
    if (g_device != 0) return true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;

    SDL_AudioSpec want = {};
    want.freq = kSampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    SDL_AudioSpec have = {};
    g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_device == 0) return false;

    g_samples[static_cast<int>(Sfx::Move)] = make({{1400.0, 28.0, 0.16}});
    g_samples[static_cast<int>(Sfx::Confirm)] =
        make({{880.0, 45.0, 0.20}, {1320.0, 60.0, 0.20}});
    g_samples[static_cast<int>(Sfx::Cancel)] =
        make({{660.0, 45.0, 0.18}, {440.0, 60.0, 0.18}});
    g_samples[static_cast<int>(Sfx::Success)] = make(
        {{523.25, 70.0, 0.20}, {659.25, 70.0, 0.20}, {783.99, 110.0, 0.20}});
    g_samples[static_cast<int>(Sfx::Error)] =
        make({{220.0, 160.0, 0.22}, {196.0, 160.0, 0.22}});

    SDL_PauseAudioDevice(g_device, 0);
    return true;
}

void shutdown() {
    if (g_device != 0) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
}

void play(Sfx sfx) {
    if (g_device == 0) return;
    const auto& samples = g_samples[static_cast<int>(sfx)];
    if (samples.empty()) return;
    // Replace anything still queued: rapid cursor movement must not build up
    // a backlog of stale ticks.
    SDL_ClearQueuedAudio(g_device);
    SDL_QueueAudio(g_device, samples.data(),
                   static_cast<Uint32>(samples.size() * sizeof(int16_t)));
}

}  // namespace sound
