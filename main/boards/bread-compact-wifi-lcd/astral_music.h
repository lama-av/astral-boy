#ifndef _ASTRAL_MUSIC_H_
#define _ASTRAL_MUSIC_H_

#include "application.h"
#include "board.h"

#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

// Astral Boy: musiquinhas sintetizadas na própria placa (sem arquivos de áudio).
//  - BootJingle(): arpejo de 6 notas quando ele fica pronto.
//  - Dance(): a música da dança (8 compassos, ~14 s) com melodia, baixo, bumbo e chimbal.
//  - StartBgm()/StopBgm(): a mesma música em loop, mais baixa, de fundo nos minijogos.
//    Enquanto ela toca, os efeitos sonoros (Play) são MISTURADOS por cima dela em pedaços
//    de 40 ms, em vez de esperar na fila (a fila de som só guarda 2 pedaços).
// A música roda numa tarefa própria de prioridade baixa, com seno por tabela (leve).
class AstralMusic {
public:
    static bool IsPlaying() { return dancing().load() || bgm().load(); }

    // Efeito sonoro: mistura na música de fundo se ela estiver tocando; senão toca direto.
    static void Play(std::vector<int16_t>&& pcm) {
        if (bgm().load()) {
            std::lock_guard<std::mutex> lock(sfx_mutex());
            auto& buf = sfx();
            buf.insert(buf.end(), pcm.begin(), pcm.end());
            return;
        }
        Push(std::move(pcm));
    }

    static void BootJingle() {
        int rate = Rate();
        if (rate <= 0) {
            return;
        }
        const float notes[] = {72, 76, 79, 84, 88, 91};  // Dó Mi Sol Dó Mi Sol, subindo
        std::vector<int16_t> pcm;
        for (int i = 0; i < 6; i++) {
            AddNote(pcm, rate, Midi(notes[i]), i == 5 ? 0.45f : 0.1f, i == 5 ? 3600.0f : 4000.0f);
        }
        AddNote(pcm, rate, Midi(96), 0.22f, 1500.0f);  // brilho
        Push(std::move(pcm));
    }

    // Toca a música da dança. Devolve a duração em segundos (0 se já está tocando algo).
    static int Dance() {
        if (IsPlaying() || dancing().exchange(true)) {
            return 0;
        }
        Wake();
        return kBars * 4 * 60 / kBpm + 1;
    }

    static void StartBgm() {
        if (dancing().load() || bgm().exchange(true)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(sfx_mutex());
            sfx().clear();
        }
        Wake();
    }

    static void StopBgm() { bgm() = false; }

private:
    static constexpr int kBpm = 140;
    static constexpr int kBars = 8;
    static constexpr float kBgmVolume = 0.45f;  // de fundo nos jogos: mais baixinha

    struct SongState {
        int64_t pos = 0;
        float lead = 0, bass = 0, kick = 0, hat_prev = 0;  // fases em ciclos
    };

    static std::atomic<bool>& dancing() { static std::atomic<bool> v{false}; return v; }
    static std::atomic<bool>& bgm() { static std::atomic<bool> v{false}; return v; }
    static std::mutex& sfx_mutex() { static std::mutex m; return m; }
    static std::vector<int16_t>& sfx() { static std::vector<int16_t> v; return v; }

    static int Rate() {
        auto codec = Board::GetInstance().GetAudioCodec();
        return codec != nullptr ? codec->output_sample_rate() : 0;
    }

    static float Midi(float n) { return 440.0f * std::pow(2.0f, (n - 69) / 12.0f); }

    static void Push(std::vector<int16_t>&& pcm) {
        Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(std::move(pcm), 2000);
    }

    // Seno por tabela (fase em ciclos, 0..1): bem mais leve que std::sin a cada amostra.
    static float Sin(float cycles) {
        static float table[1024];
        static bool ready = false;
        if (!ready) {
            for (int i = 0; i < 1024; i++) {
                table[i] = std::sin(2 * static_cast<float>(M_PI) * i / 1024);
            }
            ready = true;
        }
        cycles -= static_cast<int>(cycles);
        if (cycles < 0) cycles += 1;
        return table[static_cast<int>(cycles * 1024) & 1023];
    }

    // Nota "de videogame" (para o jingle): seno + harmônicos ímpares, ataque rápido e caída suave.
    static void AddNote(std::vector<int16_t>& pcm, int rate, float hz, float seconds, float volume) {
        int count = static_cast<int>(rate * seconds);
        float phase = 0;
        for (int i = 0; i < count; i++) {
            phase += hz / rate;
            float k = static_cast<float>(i) / count;
            float env = std::min(1.0f, i / (rate * 0.004f)) * (1.0f - 0.7f * k) * std::min(1.0f, (count - i) / (rate * 0.01f));
            float v = Sin(phase) + 0.25f * Sin(3 * phase) + 0.1f * Sin(5 * phase);
            pcm.push_back(static_cast<int16_t>(v * volume * env));
        }
    }

    // Uma amostra da música (melodia + baixo + bumbo + chimbal). A música repete a cada 8 compassos.
    static float SongSample(SongState& s, int rate) {
        // Melodia: 4 semínimas por compasso (MIDI, 0 = pausa). Acordes: C, Am, Dm, G, C, Am, G, C.
        static const int lead[kBars][4] = {
            {76, 79, 84, 79}, {81, 79, 76, 72}, {74, 77, 81, 77}, {79, 77, 74, 71},
            {76, 79, 84, 79}, {81, 84, 88, 84}, {86, 83, 79, 77}, {84, 79, 84, 0}};
        static const int bass[kBars] = {48, 45, 50, 43, 48, 45, 43, 48};
        int beat_len = rate * 60 / kBpm;
        int64_t song_len = static_cast<int64_t>(beat_len) * 4 * kBars;
        int64_t pos = s.pos % song_len;
        s.pos++;
        int beat_index = static_cast<int>(pos / beat_len);
        int bar = beat_index / 4;
        int beat = beat_index % 4;
        int i = static_cast<int>(pos % beat_len);
        float t = static_cast<float>(i) / rate;
        float k = static_cast<float>(i) / beat_len;

        float v = 0;
        int note = lead[bar][beat];
        if (note > 0) {
            s.lead += Midi(note) / rate;
            if (s.lead >= 1) s.lead -= 1;
            float env = std::min(1.0f, i / (rate * 0.004f)) * (1.0f - 0.55f * k);
            v += (Sin(s.lead) + 0.3f * Sin(3 * s.lead)) * 3200.0f * env;
        }
        float bass_hz = Midi(bass[bar] + (beat % 2 == 1 ? 12 : 0));  // baixo "pulando" oitava
        s.bass += bass_hz / rate;
        if (s.bass >= 1) s.bass -= 1;
        float bass_env = std::min(1.0f, i / (rate * 0.003f)) * (k < 0.8f ? 1.0f : (1 - k) * 5);
        v += Sin(s.bass) * 2600.0f * bass_env;
        if ((beat == 0 || beat == 2) && t < 0.12f) {  // bumbo: seno grave descendo
            s.kick += std::max(120.0f - 600.0f * t, 45.0f) / rate;
            v += Sin(s.kick) * 6000.0f * (1.0f - t / 0.12f) * (1.0f - t / 0.12f);
        } else {
            s.kick = 0;
        }
        float off = t - (60.0f / kBpm) / 2;  // chimbal no contratempo
        if (off >= 0 && off < 0.03f) {
            float noise = static_cast<float>(static_cast<int32_t>(esp_random() % 2001) - 1000) / 1000.0f;
            float hp = noise - s.hat_prev;
            s.hat_prev = noise;
            v += hp * 1300.0f * (1 - off / 0.03f);
        }
        return v;
    }

    static void Wake() {
        static TaskHandle_t task = nullptr;
        if (task == nullptr) {  // tarefa fixa: dorme até pedirem música
            xTaskCreatePinnedToCoreWithCaps([](void*) {
                while (true) {
                    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                    if (dancing().load()) {
                        PlayDance();
                        dancing() = false;
                    } else if (bgm().load()) {
                        PlayBgm();
                    }
                }
            }, "astral_music", 6 * 1024, nullptr, 1, &task, 0, MALLOC_CAP_SPIRAM);
        }
        xTaskNotifyGive(task);
    }

    static void PlayDance() {
        int rate = Rate();
        if (rate <= 0) {
            return;
        }
        SongState s;
        int beat_len = rate * 60 / kBpm;
        for (int b = 0; b < kBars * 4; b++) {
            std::vector<int16_t> pcm(beat_len);
            for (int i = 0; i < beat_len; i++) {
                pcm[i] = static_cast<int16_t>(std::clamp(SongSample(s, rate), -32000.0f, 32000.0f));
            }
            Push(std::move(pcm));
            vTaskDelay(1);  // deixa a tela respirar
        }
    }

    // Música de fundo em loop até StopBgm(), misturando os efeitos sonoros do jogo por cima.
    static void PlayBgm() {
        int rate = Rate();
        if (rate <= 0) {
            bgm() = false;
            return;
        }
        SongState s;
        int chunk = rate * 40 / 1000;
        while (bgm().load()) {
            std::vector<int16_t> pcm(chunk);
            {
                std::lock_guard<std::mutex> lock(sfx_mutex());
                auto& fx = sfx();
                size_t take = std::min(fx.size(), static_cast<size_t>(chunk));
                for (int i = 0; i < chunk; i++) {
                    float v = SongSample(s, rate) * kBgmVolume;
                    if (static_cast<size_t>(i) < take) {
                        v += fx[i];
                    }
                    pcm[i] = static_cast<int16_t>(std::clamp(v, -32000.0f, 32000.0f));
                }
                fx.erase(fx.begin(), fx.begin() + take);
            }
            Push(std::move(pcm));  // espera vaga na fila: é isso que dá o ritmo
            vTaskDelay(1);
        }
        // o que sobrou de efeito (ex.: fanfarra do fim) toca sem música
        std::vector<int16_t> rest;
        {
            std::lock_guard<std::mutex> lock(sfx_mutex());
            rest.swap(sfx());
        }
        if (!rest.empty()) {
            Push(std::move(rest));
        }
    }
};

#endif  // _ASTRAL_MUSIC_H_
