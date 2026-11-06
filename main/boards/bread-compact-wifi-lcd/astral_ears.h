#ifndef _ASTRAL_EARS_H_
#define _ASTRAL_EARS_H_

#include "application.h"
#include "astral_music.h"
#include "astral_pet.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Astral Boy: "ouvidos" do bichinho. Usa o mesmo som do microfone que a palavra de ativação
// já escuta em repouso (pedaços de 10 ms) e só calcula o volume de cada pedaço:
//  - SUSTO: um pico bem mais alto que o barulho de fundo, depois de um momento quieto
//    (porta batendo, palma, algo caindo).
//  - MÚSICA: som alto quase contínuo por vários segundos COM batida regular (o volume sobe e
//    desce num ritmo entre ~75 e 200 batidas por minuto). Fala tem pausas e não tem ritmo fixo.
// O nível de fundo se ajusta sozinho ao ambiente.
class AstralEars {
public:
    explicit AstralEars(AstralPet* pet) : pet_(pet), env_(kWindow, 0.0f) {
        Application::GetInstance().GetAudioService().SetMicListener(
            [this](const std::vector<int16_t>& pcm) { OnFrame(pcm); });
    }

private:
    static constexpr const char* TAG = "AstralEars";
    static constexpr int kWindow = 600;            // 6 s de volumes (10 ms cada)
    static constexpr int kAnalyzeEvery = 50;       // analisa a música a cada 0,5 s
    static constexpr float kStartleRatio = 4.5f;   // susto: volume pula 4,5x acima do último meio segundo...
    static constexpr float kStartleMin = 4000.0f;  // ...e acima deste volume absoluto
    static constexpr float kMusicLoudRatio = 2.2f; // música: volume acima de 2,2x o fundo quieto...
    static constexpr float kMusicMin = 900.0f;     // ...e acima deste volume absoluto
    static constexpr float kMusicLoudFrac = 0.65f; // ...em 65% do tempo
    static constexpr float kMusicBeat = 0.22f;     // força mínima da batida (autocorrelação)

    AstralPet* pet_;
    std::vector<float> env_;
    int pos_ = 0;
    int frames_ = 0;
    float floor_ = 0;       // barulho de fundo do ambiente quieto (0 = ainda não medido)
    float peak_log_ = 0;
    float beat_log_ = 0;
    float loud_log_ = 0;
    int music_hits_ = 0;
    int music_misses_ = 0;
    bool music_ = false;
    int64_t last_startle_us_ = 0;
    int64_t last_music_us_ = -60 * 1000000LL;

    void OnFrame(const std::vector<int16_t>& pcm) {
        if (pcm.empty()) {
            return;
        }
        if (AstralMusic::IsPlaying()) {  // a musiquinha dele mesmo: não é susto nem música de fora
            last_music_us_ = esp_timer_get_time();
            music_ = false;
            music_hits_ = 0;
            return;
        }
        // volume sem o "desvio" constante do microfone (DC): só a parte que vibra
        double mean = 0;
        for (int16_t s : pcm) {
            mean += s;
        }
        mean /= pcm.size();
        double sum = 0;
        for (int16_t s : pcm) {
            double d = s - mean;
            sum += d * d;
        }
        float rms = static_cast<float>(std::sqrt(sum / pcm.size()));

        // média dos últimos 0,5 s antes deste pedaço (para saber se estava quieto)
        float recent = 0;
        for (int k = 1; k <= 50; k++) {
            recent += env_[(pos_ - k + kWindow) % kWindow];
        }
        recent /= 50.0f;

        env_[pos_] = rms;
        pos_ = (pos_ + 1) % kWindow;
        frames_++;
        peak_log_ = std::max(peak_log_, rms);


        auto& app = Application::GetInstance();
        bool quiet_speaker = app.GetAudioService().IsPlaybackIdle();  // não se assusta com o próprio som

        int64_t now_us = esp_timer_get_time();
        // susto = salto repentino em relação ao meio segundo anterior (não depende do fundo)
        if (frames_ > 100 && quiet_speaker && rms > kStartleMin && rms > recent * kStartleRatio && !music_ &&
            now_us - last_music_us_ > 5 * 1000000LL && now_us - last_startle_us_ > 30 * 1000000LL) {
            last_startle_us_ = now_us;
            ESP_LOGI(TAG, "Susto! volume %d (antes %d)", static_cast<int>(rms), static_cast<int>(recent));
            pet_->OnLoudNoise();
        }

        if (frames_ % kAnalyzeEvery == 0 && frames_ >= kWindow) {
            AnalyzeMusic();
        }
        if (frames_ % 1000 == 0) {  // a cada 10 s: números para calibrar
            ESP_LOGI(TAG, "Ouvidos: fundo %d, pico %d, alto %d%%, batida %.2f, musica %d", static_cast<int>(floor_),
                     static_cast<int>(peak_log_), static_cast<int>(loud_log_ * 100), beat_log_, music_);
            peak_log_ = 0;
        }
    }

    void AnalyzeMusic() {
        // Fundo = nível dos 20% de momentos mais quietos dos últimos 6 s. Desce na hora; sobe
        // devagar (~1 min), assim uma música longa não vira "fundo".
        std::vector<float> sorted(env_);
        std::nth_element(sorted.begin(), sorted.begin() + kWindow / 5, sorted.end());
        float p20 = std::max(sorted[kWindow / 5], 30.0f);
        if (floor_ <= 0 || p20 < floor_) {
            floor_ = p20;
        } else {
            floor_ += (p20 - floor_) * 0.01f;
        }

        // janela em ordem cronológica
        float mean = 0;
        int loud = 0;
        float loud_level = std::max(kMusicMin, floor_ * kMusicLoudRatio);
        for (int i = 0; i < kWindow; i++) {
            float v = env_[(pos_ + i) % kWindow];
            mean += v;
            if (v > loud_level) {
                loud++;
            }
        }
        mean /= kWindow;
        float loud_frac = static_cast<float>(loud) / kWindow;

        float beat = 0;
        if (mean > loud_level) {  // calcula a batida sempre que tem som (o log ajuda a calibrar)
            // autocorrelação do volume (sem a média) para atrasos de 0,3 a 0,8 s
            float r0 = 0;
            for (int i = 0; i < kWindow; i++) {
                float d = env_[i] - mean;
                r0 += d * d;
            }
            if (r0 > 0) {
                for (int lag = 30; lag <= 80; lag++) {
                    float r = 0;
                    for (int i = 0; i + lag < kWindow; i++) {
                        r += (env_[(pos_ + i) % kWindow] - mean) * (env_[(pos_ + i + lag) % kWindow] - mean);
                    }
                    beat = std::max(beat, r / r0);
                }
            }
        }
        beat_log_ = beat;
        loud_log_ = loud_frac;

        // Para COMEÇAR precisa de som contínuo + batida; para CONTINUAR basta o som seguir contínuo
        // (a força da batida oscila muito de um trecho da música para outro).
        bool looks_like_music = music_ ? loud_frac >= 0.50f : (loud_frac >= kMusicLoudFrac && beat >= kMusicBeat);
        if (esp_timer_get_time() - last_music_us_ < 4 * 1000000LL) {
            looks_like_music = false;  // logo depois da musiquinha dele, o eco não conta
        }
        if (looks_like_music) {
            music_misses_ = 0;
            if (++music_hits_ >= 2 && !music_) {  // 1 s seguido parecendo música
                music_ = true;
                ESP_LOGI(TAG, "Musica! (batida %.2f, %d%% alto)", beat, static_cast<int>(loud_frac * 100));
            }
        } else {
            music_hits_ = 0;
            if (music_ && ++music_misses_ >= 6) {  // 3 s sem música
                music_ = false;
                last_music_us_ = esp_timer_get_time();
                ESP_LOGI(TAG, "Musica parou");
            }
        }
        if (music_) {
            pet_->MusicTick();
        }
    }
};

#endif  // _ASTRAL_EARS_H_
