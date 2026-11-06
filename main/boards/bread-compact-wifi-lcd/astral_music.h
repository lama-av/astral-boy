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
#include <vector>

// Astral Boy: musiquinhas sintetizadas na própria placa (sem arquivos de áudio).
//  - BootJingle(): arpejo curto e brilhante quando ele fica pronto.
//  - Dance(): "chiptune" de 8 compassos (~14 s) com melodia, baixo, bumbo e chimbal,
//    tocado batida por batida numa tarefa própria (não trava o resto).
class AstralMusic {
public:
    static bool IsPlaying() { return playing().load(); }

    static void BootJingle() {
        int rate = Rate();
        if (rate <= 0) {
            return;
        }
        // Dó-Mi-Sol-Dó agudo + brilho
        const float notes[] = {72, 76, 79, 84};
        std::vector<int16_t> pcm;
        for (int i = 0; i < 4; i++) {
            AddNote(pcm, rate, Midi(notes[i]), i == 3 ? 0.42f : 0.11f, 4200.0f);
        }
        AddNote(pcm, rate, Midi(91), 0.25f, 1800.0f);
        Push(std::move(pcm));
    }

    // Toca a música da dança numa tarefa à parte. Devolve a duração em segundos (0 se já toca).
    static int Dance() {
        if (playing().exchange(true)) {
            return 0;
        }
        static TaskHandle_t task = nullptr;
        if (task == nullptr) {  // tarefa fixa: dorme até pedirem uma dança
            xTaskCreatePinnedToCoreWithCaps([](void*) {
                while (true) {
                    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                    PlayDanceSong();
                    playing() = false;
                }
            }, "astral_dance", 6 * 1024, nullptr, 1, &task, 0, MALLOC_CAP_SPIRAM);  // prioridade baixa: a tela vem antes
        }
        xTaskNotifyGive(task);
        return kBars * 4 * 60 / kBpm + 1;
    }

private:
    static constexpr int kBpm = 140;
    static constexpr int kBars = 8;

    static std::atomic<bool>& playing() {
        static std::atomic<bool> p{false};
        return p;
    }

    static int Rate() {
        auto codec = Board::GetInstance().GetAudioCodec();
        return codec != nullptr ? codec->output_sample_rate() : 0;
    }

    static float Midi(float n) { return 440.0f * std::pow(2.0f, (n - 69) / 12.0f); }

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

    static void Push(std::vector<int16_t>&& pcm) {
        Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(std::move(pcm), 2000);
    }

    // Nota "de videogame": seno + harmônicos ímpares, ataque rápido e caída suave.
    static void AddNote(std::vector<int16_t>& pcm, int rate, float hz, float seconds, float volume) {
        int count = static_cast<int>(rate * seconds);
        float phase = 0;
        for (int i = 0; i < count; i++) {
            phase += 2 * static_cast<float>(M_PI) * hz / rate;
            float k = static_cast<float>(i) / count;
            float env = std::min(1.0f, i / (rate * 0.004f)) * (1.0f - 0.7f * k) * std::min(1.0f, (count - i) / (rate * 0.01f));
            float v = std::sin(phase) + 0.25f * std::sin(3 * phase) + 0.1f * std::sin(5 * phase);
            pcm.push_back(static_cast<int16_t>(v * volume * env));
        }
    }

    static void PlayDanceSong() {
        int rate = Rate();
        if (rate <= 0) {
            return;
        }
        // Melodia: 4 semínimas por compasso (MIDI). Acordes: C, Am, Dm, G, C, Am, G, C.
        const int lead[kBars][4] = {
            {76, 79, 84, 79}, {81, 79, 76, 72}, {74, 77, 81, 77}, {79, 77, 74, 71},
            {76, 79, 84, 79}, {81, 84, 88, 84}, {86, 83, 79, 77}, {84, 79, 84, 0}};
        const int bass[kBars] = {48, 45, 50, 43, 48, 45, 43, 48};
        int beat_len = rate * 60 / kBpm;
        float lead_phase = 0, bass_phase = 0, kick_phase = 0, hat_prev = 0;  // fases em ciclos
        for (int bar = 0; bar < kBars; bar++) {
            for (int beat = 0; beat < 4; beat++) {
                std::vector<int16_t> pcm(beat_len);
                int note = lead[bar][beat];
                float lead_hz = note > 0 ? Midi(note) : 0;
                float bass_hz = Midi(bass[bar] + (beat % 2 == 1 ? 12 : 0));  // baixo "pulando" oitava
                bool kick = beat == 0 || beat == 2;
                for (int i = 0; i < beat_len; i++) {
                    float t = static_cast<float>(i) / rate;
                    float k = static_cast<float>(i) / beat_len;
                    float v = 0;
                    if (lead_hz > 0) {
                        lead_phase += lead_hz / rate;
                        if (lead_phase >= 1) lead_phase -= 1;
                        float env = std::min(1.0f, i / (rate * 0.004f)) * (1.0f - 0.55f * k);
                        v += (Sin(lead_phase) + 0.3f * Sin(3 * lead_phase)) * 3200.0f * env;
                    }
                    bass_phase += bass_hz / rate;
                    if (bass_phase >= 1) bass_phase -= 1;
                    float bass_env = std::min(1.0f, i / (rate * 0.003f)) * (k < 0.8f ? 1.0f : (1 - k) * 5);
                    v += Sin(bass_phase) * 2600.0f * bass_env;
                    if (kick && t < 0.12f) {  // bumbo: seno grave descendo
                        kick_phase += std::max(120.0f - 600.0f * t, 45.0f) / rate;
                        v += Sin(kick_phase) * 6000.0f * (1.0f - t / 0.12f) * (1.0f - t / 0.12f);
                    } else {
                        kick_phase = 0;
                    }
                    float off = t - (60.0f / kBpm) / 2;  // chimbal no contratempo
                    if (off >= 0 && off < 0.03f) {
                        float noise = static_cast<float>(static_cast<int32_t>(esp_random() % 2001) - 1000) / 1000.0f;
                        float hp = noise - hat_prev;
                        hat_prev = noise;
                        v += hp * 1300.0f * (1 - off / 0.03f);
                    }
                    pcm[i] = static_cast<int16_t>(std::clamp(v, -32000.0f, 32000.0f));
                }
                Push(std::move(pcm));
                vTaskDelay(1);  // deixa as outras tarefas (tela) respirarem
            }
        }
    }
};

#endif  // _ASTRAL_MUSIC_H_
