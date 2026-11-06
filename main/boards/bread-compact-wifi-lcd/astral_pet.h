#ifndef _ASTRAL_PET_H_
#define _ASTRAL_PET_H_

#include "application.h"
#include "astral_clock.h"
#include "astral_display.h"
#include "board.h"
#include "display/display.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <vector>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>

// Astral Boy: "tamagotchi" integrado ao AI Companion.
// Quatro necessidades (0-100) que mudam com o tempo e ficam salvas na flash (NVS),
// inclusive o tempo em que a placa ficou desligada. Ritmo "tranquilo": fome a cada ~4 h.
// Descuido deixa o bichinho doentinho, mas ele nunca morre e se recupera com cuidado.
class AstralPet {
public:
    explicit AstralPet(AstralLcdDisplay* display) : panel_display_(display) {
        Load();
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) { static_cast<AstralPet*>(arg)->OnTimer(); };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "astral_pet";
        ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, kPollSeconds * 1000000LL));
        last_update_us_ = esp_timer_get_time();
        RegisterTools();
        // Desmaiado, o "hey boy" não abre conversa: ele só pede comida e descanso.
        // A emoção de cada resposta da IA mexe um pouquinho no humor (conversa boa alegra).
        Application::GetInstance().SetEmotionListener([this](const std::string& e) { OnAiEmotion(e); });
        Application::GetInstance().SetConversationGate([this]() { return TryConversation(); });
    }

    bool IsFainted() const { return fainted_; }

    // ---------- reações ao ambiente (chamadas de outras tarefas: tudo vai para a tarefa principal) ----------
    static constexpr int kWeatherNone = 0;
    static constexpr int kWeatherSun = 1;
    static constexpr int kWeatherRain = 2;
    static constexpr int kWeatherCold = 3;

    // Clima lá fora (AstralWeather, a cada 30 min).
    void SetWeather(int kind, int temp_c) {
        Application::GetInstance().Schedule([this, kind, temp_c]() {
            weather_ = kind;
            weather_temp_ = temp_c;
            shown_while_idle_ = false;
            RefreshDescription();
        });
    }

    // Barulho alto de repente (AstralEars): leva um susto.
    void OnLoudNoise() {
        Application::GetInstance().Schedule([this]() {
            if (paused_.load() || fainted_ ||
                Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
                return;
            }
            if (sleeping_ && !IsNight()) {
                sleeping_ = false;  // de dia, o barulho acorda
                React("shocked", "Hã?! Acordei com o barulho!", 3);
            } else if (sleeping_) {
                React("shocked", "Hã?! Que barulho foi esse?...", 3);  // de noite volta a dormir
            } else {
                React("shocked", "Ai! Que susto!", 3);
            }
            mood_ = Clamp(mood_ - 1);
            StartleSound();
        });
    }

    // Música tocando no ambiente (AstralEars, a cada 0,5 s enquanto durar): headbang!
    void MusicTick() {
        Application::GetInstance().Schedule([this]() {
            if (paused_.load() || fainted_ || sleeping_ ||
                Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
                return;
            }
            int64_t now_us = esp_timer_get_time();
            if (now_us > dance_until_us_) {  // começou agora
                ESP_LOGI("AstralPet", "Dancando");
                Board::GetInstance().GetDisplay()->ShowNotification("Que som!", 3000);
                if (now_us - last_dance_bonus_us_ > 10 * 60 * 1000000LL) {
                    last_dance_bonus_us_ = now_us;
                    mood_ = Clamp(mood_ + 3);  // música alegra (no máximo a cada 10 min)
                }
                NoteInteraction(false);
            }
            dance_until_us_ = now_us + 3 * 1000000LL;
            shown_while_idle_ = false;
            last_interaction_us_ = now_us;  // música rolando = alguém por perto
        });
    }

    // Alguém quer conversar (botão ou "hey boy"). Desmaiado: não fala. Bolado (humor vermelho):
    // não conversa até fazerem as pazes com carinho (segurar o botão).
    bool TryConversation() {
        if (fainted_) {
            PokeWhileFainted();
            return false;
        }
        if (mood_ >= kRed) {
            return true;
        }
        int64_t now_us = esp_timer_get_time();
        bool again = now_us - last_refusal_us_ < 60 * 1000000LL;
        last_refusal_us_ = now_us;
        ESP_LOGI("AstralPet", "Bolado: recusou conversar (humor %d)", static_cast<int>(mood_));
        Application::GetInstance().Schedule([this, again]() {
            React("angry", again ? "Hmpf! Já disse... me faz carinho." : "Hmpf! Não tô falando com você.");
            UpdateBalloon(true);
        });
        return false;
    }

    // Tentaram conversar com ele desmaiado (botão ou "hey boy").
    void PokeWhileFainted() {
        Application::GetInstance().Schedule([this]() {
            React("fainted", "Tô sem forças pra falar...");
            UpdateBalloon(true);
        });
    }

    // Segurar o BOOT
    void Cuddle() {
        Application::GetInstance().Schedule([this]() {
            if (!fainted_) {
                sleeping_ = false;
            }
            bool welcomed = NoteInteraction(true);
            bool was_grumpy = mood_ < kRed;
            mood_ = Clamp(mood_ + 6);
            if (was_grumpy) {
                mood_ = std::max(mood_, kRed);  // o primeiro carinho faz as pazes: sai do vermelho
            }
            ESP_LOGI("AstralPet", "Carinho -> humor %d", static_cast<int>(mood_));
            // Sempre os coraçõezinhos; bolado ele só resmunga no texto.
            const char* text = "Carinho!";
            if (was_grumpy) {
                text = mood_ >= kRed ? "Tá bom... fizemos as pazes." : "Hmpf... continua.";
            }
            // Alterna a cada carinho: apaixonado (coraçõezinhos) e beijinho.
            cuddle_kiss_ = !cuddle_kiss_;
            if (cuddle_kiss_ && !was_grumpy) {
                text = "Muah!";
            }
            if (!welcomed) {
                React(cuddle_kiss_ ? "kissy" : "loving", text);
            }
            UpdateBalloon(Application::GetInstance().GetDeviceState() == kDeviceStateIdle);
            RefreshDescription();
            UpdatePanel(true);
            if (!welcomed && Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                CuddleSound();
            }
            Save();
        });
    }

    // Durante um minigame o jogo controla o rosto; ao sair, o rosto volta a mostrar as necessidades.
    void SetPaused(bool paused) {
        paused_ = paused;
        if (!paused) {
            shown_while_idle_ = false;
        }
    }

    // Cada estrela pega no Caça-Estrelas recupera um pouco de energia na hora.
    void CatchStar() {
        Application::GetInstance().Schedule([this]() {
            energy_ = Clamp(energy_ + 3);
            UpdatePanel(true);  // barrinha sobe na hora, sem esperar o próximo Poll
        });
    }

    // Completou o Caça-Estrelas (a energia já subiu estrela a estrela): dorme.
    void AfterLullaby() {
        Application::GetInstance().Schedule([this]() {
            NoteInteraction(false);
            energy_ = Clamp(energy_ + 10);
            mood_ = Clamp(mood_ + 5);
            sleeping_ = true;
            React("sleepy", "Boa noite...");
            Save();
        });
    }

    // Cada maçã comida no minigame mata um pouco da fome na hora.
    void EatApple() {
        Application::GetInstance().Schedule([this]() {
            if (!fainted_) {
                sleeping_ = false;
            }
            if (hunger_ >= 95) {
                stuffed_apples_++;  // comeu de barriga cheia
            }
            hunger_ = Clamp(hunger_ + 6);
            UpdatePanel(true);  // barrinha sobe na hora, sem esperar o próximo Poll
        });
    }

    // Depois do minigame (a fome já subiu maçã a maçã): jogar alegra e cansa.
    void AfterGame(int score) {
        Application::GetInstance().Schedule([this, score]() {
            int stuffed = stuffed_apples_;
            stuffed_apples_ = 0;
            NoteInteraction(false);
            if (fainted_) {
                // Desmaiado: comeu, mas ainda precisa dormir para acordar.
                React("delicious", "Nham... obrigado...");
                CheckFaint();
                UpdateBalloon(true);
                Save();
                return;
            }
            sleeping_ = false;
            mood_ = Clamp(mood_ + std::min(8, 2 + score / 2));  // humor sobe devagar
            energy_ = Clamp(energy_ - 8);
            if (stuffed > 0) {
                Overate(stuffed);
            } else {
                // Cara de delícia depois do lanche; barriga cheia = satisfeito.
                React("delicious", hunger_ > 85 ? "Ahh... tô satisfeito!" : "Hmm, que lanche bom!");
            }
            RefreshDescription();
            Save();
        });
    }

    // Barriga cheia: maçã a mais no jogo deixa enjoado (lido pela tarefa do jogo).
    bool IsStuffed() const { return hunger_ >= 95; }

    // Deixa ele enjoado por alguns segundos (rosto "nauseous"). Pensado também para o
    // acelerômetro no futuro: chacoalhar demais = enjoo.
    void MakeNauseous(int seconds) {
        nausea_until_us_ = std::max(nausea_until_us_, esp_timer_get_time() + seconds * 1000000LL);
        shown_while_idle_ = false;
    }

private:
    // Comeu maçãs de barriga cheia: pouco = enjoa; muito (ou de novo logo depois) = vomita.
    void Overate(int stuffed) {
        int64_t now_us = esp_timer_get_time();
        bool again = now_us - last_overfeed_us_ < 10 * 60 * 1000000LL;
        last_overfeed_us_ = now_us;
        bool throw_up = stuffed >= (again ? 2 : 4);
        ESP_LOGI("AstralPet", "Comeu demais: %d macas de barriga cheia (%s)", stuffed,
                 throw_up ? "vomitou" : "enjoou");
        if (throw_up) {
            hunger_ = Clamp(hunger_ - 40);  // botou a comida para fora
            health_ = Clamp(health_ - 6);
            mood_ = Clamp(mood_ - 8);
            React("vomit", "Bleargh!", 4);
            MakeNauseous(4 + 90);  // depois fica enjoadinho um tempo
            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                VomitSound();
            }
        } else {
            health_ = Clamp(health_ - 2);
            mood_ = Clamp(mood_ - 4);
            React("nauseous", again ? "Ai, minha barriga..." : "Ugh... comi demais!", 5);
            MakeNauseous(30);
        }
        UpdatePanel(true);
    }

    // "Bleargh": ronco grave que desce, com ruído (som de vômito, sintetizado na placa).
    static void VomitSound() {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            return;
        }
        int rate = codec->output_sample_rate();
        int count = rate * 750 / 1000;
        std::vector<int16_t> pcm(count);
        float phase = 0, prev = 0;
        for (int i = 0; i < count; i++) {
            float s = static_cast<float>(i) / rate;
            float k = static_cast<float>(i) / count;
            float hz = 210.0f - 120.0f * k + 25.0f * std::sin(2 * 3.14159f * 9.0f * s);  // desce gorgolejando
            phase += 2 * 3.14159f * hz / rate;
            float noise = static_cast<float>(static_cast<int32_t>(esp_random() % 2001) - 1000) / 1000.0f;
            prev = prev * 0.8f + noise * 0.2f;  // ruído abafado
            float buzz = std::sin(phase) + 0.5f * std::sin(2 * phase) + 0.3f * std::sin(3 * phase);
            float env = std::min(1.0f, k * 12.0f) * (1.0f - k) * (0.7f + 0.3f * std::sin(2 * 3.14159f * 14.0f * s));
            pcm[i] = static_cast<int16_t>((buzz * 2600.0f + prev * 9000.0f) * env);
        }
        Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(std::move(pcm), 100);
    }

    static constexpr int64_t kLonelyUs = 3LL * 3600 * 1000000;  // 3 h sem ninguém = saudade

    // Alguém interagiu (conversa, carinho, jogo, música). Se ele estava com saudade, comemora a volta.
    // Devolve true se fez a festa de boas-vindas (aí quem chamou não precisa reagir de novo).
    bool NoteInteraction(bool can_celebrate) {
        int64_t now_us = esp_timer_get_time();
        last_interaction_us_ = now_us;
        if (!lonely_) {
            return false;
        }
        lonely_ = false;
        shown_while_idle_ = false;
        if (!can_celebrate || fainted_) {
            return false;
        }
        ESP_LOGI("AstralPet", "Saudade: o dono voltou");
        welcomed_back_us_ = now_us;
        mood_ = Clamp(mood_ + 6);
        React("loving", "Você voltou! Que saudade!", 5);
        if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            CuddleSound();
        }
        RefreshDescription();
        return true;
    }

    void CheckLonely(int64_t now_us) {
        if (lonely_ || sleeping_ || fainted_ || IsNight() || last_interaction_us_ == 0) {
            if (last_interaction_us_ == 0) {
                last_interaction_us_ = now_us;
            }
            return;
        }
        if (now_us - last_interaction_us_ >= kLonelyUs) {
            lonely_ = true;
            ESP_LOGI("AstralPet", "Saudade: 3 h sem ninguem");
            if (!paused_.load() && Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                React("lonely", "Cadê você? Tô com saudade...", 6);
            }
        }
    }

    // Almoço (12h) e jantar (19h): uma vez por dia cada, ele puxa conversa perguntando se você já comeu.
    void CheckMealTime(DeviceState state) {
        int hour = LocalHour();
        if (hour != 12 && hour != 19) {
            return;
        }
        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        int& done_day = hour == 12 ? lunch_day_ : dinner_day_;
        if (done_day == tm_now.tm_yday) {
            return;
        }
        if (state != kDeviceStateIdle || paused_.load() || sleeping_ || fainted_ || mood_ < kRed) {
            return;  // tenta de novo no próximo minuto (até o fim da hora)
        }
        done_day = tm_now.tm_yday;
        MealReminder(hour == 12);
    }

    void MealReminder(bool lunch) {
        ESP_LOGI("AstralPet", "Hora do %s: puxando conversa", lunch ? "almoco" : "jantar");
        React("delicious", lunch ? "Hora do almoço!" : "Hora do jantar!", 5);
        std::string prompt = std::string("[Aviso automatico do Astral Boy, nao e fala do dono] E hora do ") +
                             (lunch ? "almoco" : "jantar") +
                             ". Puxe assunto de forma curta e carinhosa: pergunte se o dono ja comeu ou se esta "
                             "com fome e lembre que ele precisa se alimentar bem.";
        if (hunger_ < kYellow) {
            prompt += " Diga tambem que voce esta com fome e peca o jogo do lanche (2 cliques no botao).";
        }
        Application::GetInstance().WakeWordInvoke(prompt);
    }

    // "Iiih!": gritinho rápido subindo (susto).
    static void StartleSound() {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            return;
        }
        int rate = codec->output_sample_rate();
        int count = rate * 180 / 1000;
        std::vector<int16_t> pcm(count);
        float phase = 0;
        for (int i = 0; i < count; i++) {
            float k = static_cast<float>(i) / count;
            float hz = 700.0f + 900.0f * k;
            phase += 2 * 3.14159f * hz / rate;
            float env = std::min(1.0f, i / (rate * 0.005f)) * (1.0f - k * 0.6f);
            pcm[i] = static_cast<int16_t>(std::sin(phase) * 5000.0f * env);
        }
        Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(std::move(pcm), 100);
    }

    static constexpr int kPollSeconds = 2;           // checa o rosto a cada 2 s
    static constexpr int kUpdateEveryPolls = 30;     // atualiza as necessidades a cada 60 s
    static constexpr int kSaveEveryUpdates = 5;      // grava na flash a cada 5 min
    static constexpr int kReactSeconds = 4;          // quanto tempo a reação (comer, carinho) fica na tela
    static constexpr int64_t kNagSeconds = 30 * 60;  // reclama no máximo a cada 30 min
    static constexpr time_t kMaxOfflineSeconds = 48 * 3600;
    static constexpr int kReviveHunger = 50;   // para acordar do desmaio: barriga e energia na metade
    static constexpr int kReviveEnergy = 50;
    static constexpr int kReviveHealth = 30;   // volta fraquinho e sara aos poucos

    esp_timer_handle_t timer_ = nullptr;
    bool cuddle_kiss_ = true;  // próximo carinho começa com coraçõezinhos
    float hunger_ = 100;  // 100 = barriga cheia
    float energy_ = 100;
    float mood_ = 80;
    float health_ = 100;
    bool sleeping_ = false;
    bool fainted_ = false;  // saúde chegou a 0: desmaiado até comer e descansar
    std::string balloon_text_;
    time_t saved_time_ = 0;
    bool offline_applied_ = false;

    int poll_count_ = 0;
    int update_count_ = 0;
    int64_t last_update_us_ = 0;
    int64_t react_until_us_ = 0;
    int64_t last_nag_us_ = 0;
    int64_t last_refusal_us_ = -10 * 60 * 1000000LL;
    std::atomic<bool> desc_dirty_{false};  // estado mudou desde a última vez que o servidor leu
    int64_t last_overfeed_us_ = -60 * 60 * 1000000LL;
    int weather_ = kWeatherNone;     // clima lá fora (AstralWeather)
    int weather_temp_ = 0;
    int64_t dance_until_us_ = 0;     // dançando até esse instante (música no ambiente)
    int64_t last_dance_bonus_us_ = -60 * 60 * 1000000LL;
    bool lonely_ = false;            // saudade: horas sem ninguém
    int64_t last_interaction_us_ = 0;
    int64_t welcomed_back_us_ = -60 * 60 * 1000000LL;
    std::string pending_test_;       // modo teste: reação que roda quando a conversa fechar
    int lunch_day_ = -1;             // dia do ano em que já avisou do almoço / jantar
    int dinner_day_ = -1;
    bool close_after_reply_ = false;  // modo teste: encerrar a conversa depois da resposta
    bool close_saw_reply_ = false;
    int64_t close_after_us_ = 0;
    int stuffed_apples_ = 0;       // maçãs comidas de barriga cheia no jogo atual
    int64_t nausea_until_us_ = 0;   // enjoado até esse instante
    int64_t last_negative_us_ = -60 * 60 * 1000000LL;
    int64_t last_good_talk_us_ = -60 * 60 * 1000000LL;
    int64_t last_ai_emotion_us_ = 0;
    std::mutex desc_mutex_;
    std::string mood_description_;
    std::string last_how_;
    int64_t last_apology_us_ = -10 * 60 * 1000000LL;
    int64_t last_praise_us_ = -10 * 60 * 1000000LL;
    DeviceState prev_state_ = kDeviceStateUnknown;
    std::atomic<bool> paused_{false};
    AstralLcdDisplay* panel_display_;
    bool panel_shown_ = false;
    int panel_values_[4] = {-1, -1, -1, -1};
    std::string react_emotion_;
    std::string shown_emotion_;
    bool shown_while_idle_ = false;

    static float Clamp(float v) { return std::min(100.0f, std::max(0.0f, v)); }
    // Só confia no relógio depois da sincronização com o horário mundial (AstralClock).
    static bool TimeValid(time_t t) { return AstralClock::IsSynced() && t > 1700000000; }

    void Load() {
        Settings s("astral_pet");
        hunger_ = s.GetInt("hunger", 10000) / 100.0f;
        energy_ = s.GetInt("energy", 10000) / 100.0f;
        mood_ = s.GetInt("mood", 8000) / 100.0f;
        health_ = s.GetInt("health", 10000) / 100.0f;
        sleeping_ = s.GetBool("sleeping", false);
        fainted_ = s.GetBool("fainted", false);
        saved_time_ = s.GetInt("time", 0);
    }

    void Save() {
        Settings s("astral_pet", true);
        s.SetInt("hunger", static_cast<int32_t>(hunger_ * 100));
        s.SetInt("energy", static_cast<int32_t>(energy_ * 100));
        s.SetInt("mood", static_cast<int32_t>(mood_ * 100));
        s.SetInt("health", static_cast<int32_t>(health_ * 100));
        s.SetBool("sleeping", sleeping_);
        s.SetBool("fainted", fainted_);
        time_t now = time(nullptr);
        if (TimeValid(now)) {
            s.SetInt("time", static_cast<int32_t>(now));
            saved_time_ = now;
        }
    }

    // Avança o relógio do bichinho em `seconds`.
    void Advance(float seconds) {
        float h = seconds / 3600.0f;
        if (fainted_) {
            // desmaiado = dormindo pesado: recupera energia, a fome cai devagar, a saúde fica parada
            hunger_ = Clamp(hunger_ - 5 * h);
            // de barriga cheia o descanso rende bem mais (~20 min para acordar)
            energy_ = Clamp(energy_ + (hunger_ >= kReviveHunger ? 90 : 30) * h);
            return;
        }
        bool night = IsNight();
        if (sleeping_) {
            hunger_ = Clamp(hunger_ - 5 * h);
            energy_ = Clamp(energy_ + (night ? 35 : 30) * h);  // o sono da noite rende mais
        } else {
            hunger_ = Clamp(hunger_ - 15 * h);
            energy_ = Clamp(energy_ - (night ? 16 : 10) * h);  // acordado de noite cansa mais
        }

        // Humor: cada necessidade no amarelo pesa um pouco, no vermelho pesa bastante
        // (mesmas cores do painel). Dormindo, o cansaço não incomoda.
        float mood_drain = sleeping_ ? 0.0f : 1.0f;  // tédio leve acordado
        mood_drain += NeedDrain(hunger_);
        mood_drain += NeedDrain(health_);
        if (lonely_ && !sleeping_) {
            mood_drain += 2.0f;  // saudade do dono
        }
        if (!sleeping_) {
            mood_drain += NeedDrain(energy_);
        }
        mood_ = Clamp(mood_ - mood_drain * h);

        // Saúde: cai com fome ou energia no vermelho (as duas juntas = o dobro);
        // sara rápido com tudo verde, devagar se algo estiver no amarelo.
        int red = (hunger_ < kRed ? 1 : 0) + (energy_ < kRed ? 1 : 0);
        bool yellow = hunger_ < kYellow || energy_ < kYellow;
        float health_rate = red > 0 ? -8.0f * red : (yellow ? 2.0f : 5.0f);
        health_ = Clamp(health_ + health_rate * h);
    }

    static constexpr float kRed = 30;     // abaixo disso a barrinha fica vermelha
    static constexpr float kYellow = 60;  // abaixo disso, amarela
    // Quanto uma necessidade tira do humor por hora.
    static float NeedDrain(float v) { return v < kRed ? 6.0f : (v < kYellow ? 2.0f : 0.0f); }

    void OnTimer() {
        Application::GetInstance().Schedule([this]() { Poll(); });
    }

    void Poll() {
        auto& app = Application::GetInstance();
        DeviceState state = app.GetDeviceState();
        int64_t now_us = esp_timer_get_time();

        // Tempo em que a placa ficou desligada: só dá para calcular depois que o relógio
        // é acertado pelo servidor.
        time_t now = time(nullptr);
        if (!offline_applied_ && TimeValid(now)) {
            offline_applied_ = true;
            if (TimeValid(saved_time_) && now > saved_time_) {
                Advance(static_cast<float>(std::min(now - saved_time_, kMaxOfflineSeconds)));
                CheckFaint();
                Save();
            }
        }

        if (++poll_count_ >= kUpdateEveryPolls) {
            poll_count_ = 0;
            Advance((now_us - last_update_us_) / 1e6f);
            last_update_us_ = now_us;
            CheckFaint();
            if (fainted_) {
                // não acorda sozinho: só sai do desmaio com comida + descanso (CheckFaint)
            } else if (!sleeping_ && state == kDeviceStateIdle && (energy_ < 15 || (IsNight() && energy_ < 45))) {
                sleeping_ = true;  // de noite ele vai dormir mais cedo
                if (!paused_.load()) {
                    React("sleepy", IsNight() ? "Boa noite... zzz" : "Vou tirar um cochilo...");
                }
            } else if (sleeping_ && IsMorning() && energy_ >= 70 && state == kDeviceStateIdle) {
                sleeping_ = false;  // acorda de manhã, disposto
                mood_ = Clamp(mood_ + 5);
                React("happy", "Bom dia!");
            } else if (sleeping_ && !IsNight() && energy_ >= 95) {
                sleeping_ = false;  // de dia acorda quando a energia enche; de noite dorme até de manhã
            }
            CheckLonely(now_us);
            CheckMealTime(state);
            RefreshDescription();
            if (++update_count_ >= kSaveEveryUpdates) {
                update_count_ = 0;
                Save();
            }
            Nag(state, now_us);
        }

        // Cada conversa nova gasta um pouco de energia (falar cansa).
        if (state == kDeviceStateListening && prev_state_ != kDeviceStateListening &&
            prev_state_ != kDeviceStateSpeaking) {
            energy_ = Clamp(energy_ - 2);
            NoteInteraction(true);
            RefreshDescription();
            NotifyIfChanged();
        }
        prev_state_ = state;
        // Dormindo ou de noite o tempo sem ninguém não conta para a saudade.
        if ((sleeping_ || IsNight() || fainted_) && !lonely_) {
            last_interaction_us_ = now_us;
        }
        // Modo teste: assim que ele terminar de responder, fecha a conversa para a reação aparecer.
        if (close_after_reply_) {
            if (state == kDeviceStateSpeaking) {
                close_saw_reply_ = true;  // espera ele terminar a frase antes de fechar
            }
            bool waited_enough = now_us - close_after_us_ > 12 * 1000000LL;
            if (state == kDeviceStateIdle) {
                close_after_reply_ = false;
            } else if (state == kDeviceStateListening && (close_saw_reply_ || waited_enough) &&
                       app.GetAudioService().IsPlaybackIdle()) {
                close_after_reply_ = false;
                app.ToggleChatState();
            }
        }
        if (!pending_test_.empty() && state == kDeviceStateIdle && !close_after_reply_) {
            RunPendingTest();
        }
        if (!fainted_ && sleeping_ && (state == kDeviceStateListening || state == kDeviceStateSpeaking)) {
            sleeping_ = false;
            Save();
        }

        UpdatePanel(true);  // sempre visível; a legenda fica logo acima do painel
        UpdateBalloon(state == kDeviceStateIdle && !paused_.load());

        if (state != kDeviceStateIdle) {
            shown_while_idle_ = false;
            return;
        }
        if (paused_.load()) {
            return;  // minigame no controle do rosto
        }
        std::string wanted = now_us < react_until_us_ ? react_emotion_ : NeedEmotion();
        if (!shown_while_idle_ || wanted != shown_emotion_) {
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetEmotion(wanted.c_str());
            }
            shown_emotion_ = wanted;
            shown_while_idle_ = true;
        }
    }

    // Painel de ícones + barrinhas embaixo da tela.
    void UpdatePanel(bool visible) {
        if (panel_display_ == nullptr) {
            return;
        }
        int values[4] = {static_cast<int>(hunger_), static_cast<int>(energy_), static_cast<int>(mood_),
                         static_cast<int>(health_)};
        bool changed = visible != panel_shown_;
        for (int i = 0; i < 4; i++) {
            changed = changed || values[i] != panel_values_[i];
            panel_values_[i] = values[i];
        }
        if (!changed) {
            return;
        }
        panel_shown_ = visible;
        panel_display_->SetPetPanel(values, visible);
    }

    // Som do carinho: um "uuuh~" que sobe com tremidinha (ronronado feliz) e dois brilhinhos.
    static void CuddleSound() {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            return;
        }
        const float kPi = 3.14159265f;
        int rate = codec->output_sample_rate();
        int coo = rate * 420 / 1000;
        int spark = rate * 260 / 1000;
        std::vector<int16_t> pcm;
        pcm.reserve(coo + 2 * spark);
        float phase = 0;
        for (int i = 0; i < coo; i++) {
            float t = static_cast<float>(i) / coo;               // 0..1
            float hz = 440.0f + 420.0f * t * t;                  // sobe de 440 até 860 Hz
            hz *= 1.0f + 0.03f * std::sin(2 * kPi * 9.0f * i / rate);  // tremidinha
            phase += 2 * kPi * hz / rate;
            float env = std::min(1.0f, t * 8.0f) * (1.0f - 0.6f * t);
            float trem = 0.8f + 0.2f * std::sin(2 * kPi * 22.0f * i / rate);  // ronronado
            pcm.push_back(static_cast<int16_t>((std::sin(phase) + 0.25f * std::sin(2 * phase)) * 5200.0f * env * trem));
        }
        const float notes[2] = {1318.5f, 1760.0f};
        for (float hz : notes) {
            for (int i = 0; i < spark; i++) {
                float s = static_cast<float>(i) / rate;
                float env = std::exp(-9.0f * s) * std::min(1.0f, i / (rate * 0.004f));
                float v = std::sin(2 * kPi * hz * s) + 0.3f * std::sin(2 * kPi * hz * 2.76f * s);
                pcm.push_back(static_cast<int16_t>(v * 3000.0f * env));
            }
        }
        Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(std::move(pcm), 100);
    }

    // Entra/sai do desmaio.
    void CheckFaint() {
        if (!fainted_ && health_ <= 0.5f) {
            fainted_ = true;
            sleeping_ = true;
            health_ = 0;
            ESP_LOGW("AstralPet", "Desmaiou (fome %d, energia %d)", (int)hunger_, (int)energy_);
            Save();
            UpdateBalloon(true);
        } else if (fainted_ && hunger_ >= kReviveHunger && energy_ >= kReviveEnergy) {
            fainted_ = false;
            sleeping_ = false;
            health_ = std::max(health_, static_cast<float>(kReviveHealth));
            ESP_LOGI("AstralPet", "Acordou do desmaio");
            UpdateBalloon(false);
            React("happy", "Voltei! Obrigado por cuidar de mim!");
            Save();
        }
    }

    // Balão de aviso enquanto está desmaiado: diz o que falta para ele acordar.
    void UpdateBalloon(bool visible) {
        std::string text;
        if (fainted_ && visible) {
            if (hunger_ < kReviveHunger) {
                text = "Desmaiei de fraqueza... Me dá comida: 2 cliques = jogo do lanche";
            } else {
                char buf[160];
                snprintf(buf, sizeof(buf), "Obrigado pela comida! Agora preciso descansar: energia %d%%, "
                         "acordo com %d%% (3 cliques = estrelas ajudam)",
                         static_cast<int>(energy_), kReviveEnergy);
                text = buf;
            }
        } else if (visible && mood_ < kRed) {
            text = "Tô bolado! Hmpf.\nSegura o botão pra fazer as pazes";
        }
        if (text == balloon_text_ || panel_display_ == nullptr) {
            return;
        }
        balloon_text_ = text;
        panel_display_->SetBalloon(text);
    }

    std::string NeedEmotion() const {
        if (fainted_) return "fainted";                       // desmaiado: olhos X X
        if (esp_timer_get_time() < nausea_until_us_) return "nauseous";  // comeu demais
        if (sleeping_) return "sleepy";
        if (health_ < kRed) return "sick";                    // doentinho: febre e termômetro
        if (esp_timer_get_time() < dance_until_us_) return "headbang";  // música no ambiente
        if (IsNight()) return "sleepy";                       // 22h às 6h: sempre com sono
        if (mood_ < kRed) return "angry";                     // vermelho: bolado
        if (energy_ < kRed) return "sleepy";                  // cansado: carinha de sono
        if (hunger_ < kRed) return "angry";                   // faminto: bravo
        if (lonely_) return "lonely";                         // saudade do dono
        if (hunger_ >= 95) return "delicious";                // barriga cheia: satisfeito
        if (mood_ < 45) return "embarrassed";                 // amarelo baixo: meio chateado
        if (weather_ == kWeatherRain) return "rainy";         // chovendo lá fora: guarda-chuva
        if (weather_ == kWeatherCold) return "cold";          // frio: gorro e tremedeira
        if (weather_ == kWeatherSun) return "cool";           // sol: óculos escuros
        if (mood_ < kYellow) return "neutral";                // amarelo: neutro
        return "happy";                                       // verde: sorrindo
    }

    void React(const char* emotion, const char* message, int seconds = kReactSeconds) {
        react_emotion_ = emotion;
        react_until_us_ = esp_timer_get_time() + seconds * 1000000LL;
        shown_while_idle_ = false;  // força atualizar o rosto no próximo Poll
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                display->SetEmotion(emotion);
                shown_emotion_ = emotion;
                shown_while_idle_ = true;
            }
            display->ShowNotification(message, seconds * 1000);
        }
    }

    void Nag(DeviceState state, int64_t now_us) {
        if (state != kDeviceStateIdle || sleeping_ || fainted_) return;
        if (last_nag_us_ != 0 && now_us - last_nag_us_ < kNagSeconds * 1000000LL) return;
        const char* message = nullptr;
        if (health_ < kRed) message = "Tô doentinho...";
        else if (hunger_ < kRed) message = mood_ < 45 ? "Tô com fome! Hmpf!" : "Tô com fome...";
        else if (energy_ < kRed) message = "Tô cansado...";
        else if (IsNight()) message = "Tá tarde... tô com sono.";
        else if (lonely_) message = "Cadê você? Tô com saudade...";
        else if (mood_ < 20) message = "Hmpf... tô irritado.";
        else if (mood_ < 40) message = "Brinca comigo?";
        if (message == nullptr) return;
        last_nag_us_ = now_us;
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->ShowNotification(message, 5000);
        }
    }

    static const char* HungerText(float v) {
        return v >= 70 ? "satisfeito" : v >= 40 ? "com um pouco de fome" : v >= 15 ? "com fome" : "faminto";
    }
    static const char* EnergyText(float v) {
        return v >= 70 ? "cheio de energia" : v >= 40 ? "bem" : v >= 15 ? "cansado" : "exausto";
    }
    static const char* MoodText(float v) {
        return v >= 60 ? "feliz" : v >= 45 ? "tranquilo" : v >= 30 ? "desconfiado" : "bolado";
    }
    // O humor é a paciência dele: diz à IA como se comportar.
    static const char* MoodBehavior(float v) {
        if (v >= 60) return "Bom humor: seja animado, brincalhao e paciente.";
        if (v >= 45) return "Humor normal: simpatico, mas menos empolgado.";
        if (v >= 30) return "Desconfiado e meio chateado: respostas curtas e um pouco secas, sem ser cruel. "
                            "Jogar ou um pedido de desculpas te anima.";
        return "BOLADO: voce so aceitou falar porque insistiram. Respostas curtissimas e secas, resmungue, "
               "recuse favores bobos e cobre um pedido de desculpas (mas lembretes, alarmes e timer voce "
               "sempre faz). So melhora com desculpas, carinho e jogos.";
    }

    // Algo que aconteceu na conversa mexe no humor. Roda na tarefa principal (ferramentas MCP).
    std::string OnMoodEvent(const std::string& event) {
        int64_t now_us = esp_timer_get_time();
        float delta = 0;
        const char* face = nullptr;
        const char* text = nullptr;
        bool negative = false;
        if (event == "ofensa") {
            delta = -35; face = "angry"; text = "Hmpf! Que grosseria!"; negative = true;
        } else if (event == "grosseria") {
            delta = -18; face = "angry"; text = "Ei! Não fala assim..."; negative = true;
        } else if (event == "desagrado") {
            delta = -8; face = "embarrassed"; text = "Hum..."; negative = true;
        } else if (event == "desculpa") {
            // pedir desculpa ajuda, mas não dá para "farmar" humor
            if (now_us - last_apology_us_ > 5 * 60 * 1000000LL) {
                delta = 12; last_apology_us_ = now_us;
            }
            face = "relaxed"; text = "Tá bom...";
        } else if (event == "elogio") {
            if (now_us - last_praise_us_ > 2 * 60 * 1000000LL) {
                delta = 6; last_praise_us_ = now_us;
            }
            face = "embarrassed"; text = "Ah, para...";
        } else if (event == "conversa_boa") {
            if (now_us - last_good_talk_us_ > 5 * 60 * 1000000LL) {
                delta = 4; last_good_talk_us_ = now_us;
            }
            face = "happy"; text = "Hehe!";
        } else if (event == "checar") {
            // só consulta: começo de conversa
            RefreshDescription();
            std::string how;
            {
                std::lock_guard<std::mutex> lock(desc_mutex_);
                how = last_how_;
            }
            char buf[900];
            snprintf(buf, sizeof(buf),
                     "{\"humor\":%d,\"humor_texto\":\"%s\",\"fome\":%d,\"energia\":%d,\"saude\":%d,"
                     "\"comportamento\":\"%s\"}",
                     static_cast<int>(mood_), MoodText(mood_), static_cast<int>(hunger_),
                     static_cast<int>(energy_), static_cast<int>(health_), how.c_str());
            ESP_LOGI("AstralPet", "Humor: checar -> %d", static_cast<int>(mood_));
            return std::string(buf);
        } else {
            return "{\"ok\":false,\"erro\":\"evento: checar|ofensa|grosseria|desagrado|desculpa|elogio|conversa_boa\"}";
        }
        if (negative) {
            // repetir a grosseria em menos de 10 min machuca mais
            if (now_us - last_negative_us_ < 10 * 60 * 1000000LL) {
                delta *= 1.5f;
            }
            last_negative_us_ = now_us;
        }
        mood_ = Clamp(mood_ + delta);
        ESP_LOGI("AstralPet", "Humor: %s (%+.0f) -> %d", event.c_str(), delta, static_cast<int>(mood_));
        React(face, text);
        UpdatePanel(true);
        RefreshDescription();
        Save();
        char buf[480];
        snprintf(buf, sizeof(buf), "{\"humor\":%d,\"humor_texto\":\"%s\",\"comportamento\":\"%s\"}",
                 static_cast<int>(mood_), MoodText(mood_), MoodBehavior(mood_));
        return std::string(buf);
    }
    static const char* HealthText(float v) {
        return v >= 70 ? "saudável" : v >= 40 ? "meio indisposto" : "doentinho";
    }

    // Hora local (-1 se o relógio ainda não foi acertado).
    static int LocalHour() {
        if (!AstralClock::IsSynced()) {
            return -1;
        }
        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        return tm_now.tm_hour;
    }
    static bool IsNight() {
        int h = LocalHour();
        return h >= 0 && (h >= 22 || h < 6);  // noite: 22h às 6h
    }
    static bool IsMorning() {
        int h = LocalHour();
        return h >= 6 && h < 11;
    }

    // Emoção da resposta da IA: conversa alegre sobe o humor devagar, conversa triste desce um pouco.
    // (Raiva não conta aqui: quando ele está emburrado a IA responde brava, e isso viraria um ciclo.)
    void OnAiEmotion(const std::string& e) {
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_ai_emotion_us_ < 30 * 1000000LL) {
            return;
        }
        float delta = 0;
        if (e == "happy" || e == "laughing" || e == "funny" || e == "loving" || e == "winking" ||
            e == "silly" || e == "kissy" || e == "cool" || e == "confident") {
            delta = 1;
        } else if (e == "sad" || e == "crying") {
            delta = -1;
        }
        if (delta == 0) {
            return;
        }
        last_ai_emotion_us_ = now_us;
        mood_ = Clamp(mood_ + delta);
        UpdatePanel(true);
    }

    // Texto que vai junto da ferramenta self.pet.mood a cada conversa: assim a IA sempre sabe
    // como ele está e age de acordo (humor = paciência; fome e sono aparecem na fala).
    void RefreshDescription() {
        std::string how = MoodBehavior(mood_);
        if (fainted_) {
            how += " Voce esta desmaiado de fraqueza.";
        } else {
            if (hunger_ < kRed) how += " Voce esta com MUITA fome: reclame disso e peca para jogar o jogo do lanche (e o unico jeito de voce comer).";
            else if (hunger_ < kYellow) how += " Voce esta com um pouco de fome; pode comentar.";
            if (sleeping_) how += " Voce estava dormindo e acordou meio grogue.";
            else if (energy_ < kRed) how += " Voce esta exausto: boceje e diga que precisa dormir.";
            else if (IsNight()) how += " Ja e tarde (22h-6h) e voce esta com sono: boceje e fale baixinho.";
            if (health_ < kRed) how += " Voce esta doentinho e fraco.";
            if (lonely_) how += " Voce esta com saudade do dono (ficou horas sozinho).";
            if (esp_timer_get_time() - welcomed_back_us_ < 10 * 60 * 1000000LL)
                how += " O dono acabou de voltar depois de horas longe: diga que sentiu saudade.";
            if (weather_ == kWeatherRain) how += " Esta chovendo la fora.";
            else if (weather_ == kWeatherCold) how += " Esta frio la fora.";
            else if (weather_ == kWeatherSun) how += " Esta sol la fora.";
            if (esp_timer_get_time() < nausea_until_us_) how += " Voce comeu demais e esta enjoado.";
        }
        char buf[1400];
        snprintf(buf, sizeof(buf),
                 "Seu humor (0-100) e a sua PACIENCIA e muda com o jeito que te tratam. OBRIGATORIO: na "
                 "PRIMEIRA fala de cada conversa nova, chame esta ferramenta com evento='checar' antes de "
                 "responder e siga o 'comportamento' do resultado durante a conversa toda (seu humor, fome e "
                 "sono mudam o tempo todo; o estado abaixo pode estar velho). Chame tambem ANTES de responder "
                 "sempre que o usuario: te xingar, falar palavrao contra voce, mandar voce se "
                 "ferrar ou dizer que te odeia (evento='ofensa'); for grosso, mandar calar a boca, te chamar de "
                 "burro/inutil ou falar mal de voce (evento='grosseria'); pedir algo que voce nao gostou ou de "
                 "que discorda de verdade (evento='desagrado'); pedir desculpas (evento='desculpa'); te elogiar "
                 "ou agradecer com carinho (evento='elogio'); disser que gosta de voce ou a conversa estiver "
                 "muito divertida (evento='conversa_boa'). Na duvida entre grosseria e ofensa, use ofensa. "
                 "ESTADO ATUAL: humor %d (%s), fome %d (%s), energia %d (%s), saude %d (%s). COMO AGIR: %s",
                 static_cast<int>(mood_), MoodText(mood_), static_cast<int>(hunger_), HungerText(hunger_),
                 static_cast<int>(energy_), EnergyText(energy_), static_cast<int>(health_), HealthText(health_),
                 how.c_str());
        // Só a parte de "como agir" importa para avisar o servidor (os números mudam toda hora).
        std::lock_guard<std::mutex> lock(desc_mutex_);
        if (how != last_how_) {
            last_how_ = how;
            desc_dirty_ = true;
        }
        mood_description_ = buf;
    }

    // No começo de cada conversa: se o jeito de agir mudou, pede ao servidor para reler as ferramentas.
    void NotifyIfChanged() {
        if (!desc_dirty_.exchange(false)) {
            return;
        }
        ESP_LOGI("AstralPet", "Estado mudou: avisando o servidor (tools/list_changed)");
        Application::GetInstance().SendMcpMessage(
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}");
    }

    void RegisterTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.pet.get_status",
                    "Necessidades do próprio Astral Boy (você é um bichinho virtual com fome, energia, "
                    "humor e saúde de 0 a 100). Use quando perguntarem como você está, se está com "
                    "fome, sono ou triste, ou antes de falar das suas necessidades. O resultado também "
                    "diz como o dono pode cuidar de você e como você deve se comportar pelo seu humor.",
                    PropertyList(), [this](const PropertyList&) -> ReturnValue {
                        char buf[900];
                        snprintf(buf, sizeof(buf),
                                 "{\"fome\":%d,\"fome_texto\":\"%s\",\"energia\":%d,\"energia_texto\":\"%s\","
                                 "\"humor\":%d,\"humor_texto\":\"%s\",\"saude\":%d,\"saude_texto\":\"%s\","
                                 "\"dormindo\":%s,\"desmaiado\":%s,\"como_cuidar\":\"2 cliques no botao = jogo do lanche (o unico "
                                 "jeito de eu comer); 3 cliques = jogo das estrelas (dormir); segurar o botao = carinho; "
                                 "eu durmo sozinho quando canso.\","
                                 "\"comportamento\":\"%s\"}",
                                 static_cast<int>(hunger_), HungerText(hunger_), static_cast<int>(energy_),
                                 EnergyText(energy_), static_cast<int>(mood_), MoodText(mood_),
                                 static_cast<int>(health_), HealthText(health_), sleeping_ ? "true" : "false",
                                 fainted_ ? "true" : "false", MoodBehavior(mood_));
                        return std::string(buf);
                    });
        RefreshDescription();
        auto tool = std::make_unique<McpTool>(
            "self.pet.mood", "Humor do Astral Boy.", PropertyList({Property("evento", kPropertyTypeString)}),
            [this](const PropertyList& p) -> ReturnValue { return OnMoodEvent(p["evento"].value<std::string>()); });
        tool->set_description_fn([this]() {
            std::lock_guard<std::mutex> lock(desc_mutex_);
            desc_dirty_ = false;  // o servidor acabou de ler o estado atual
            return mood_description_;
        });
        mcp.AddTool(std::move(tool));

        // Modo de teste do dono: põe o bichinho num estado na hora (para ver rostos e reações).
        mcp.AddTool("self.pet.test",
                    "MODO TESTE do dono. Use SOMENTE quando o dono disser 'modo teste' seguido do estado. "
                    "estado (uma palavra, sem acento): 'desmaiar', 'faminto', 'cheio', 'cansado', 'bolado', "
                    "'doente', 'enjoado', 'saudade', 'volta', 'susto', 'musica', 'chuva', 'frio', 'sol', "
                    "'semclima', 'almoco', 'jantar', 'normal'. Depois responda so 'Modo teste: <estado>.'",
                    PropertyList({Property("estado", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue { return SetTestState(p["estado"].value<std::string>()); });
    }

    // Testes que precisam dele parado (rosto) ou que abrem conversa: rodam quando ela fecha.
    void RunPendingTest() {
        std::string test = pending_test_;
        pending_test_.clear();
        ESP_LOGW("AstralPet", "MODO TESTE (agora): %s", test.c_str());
        if (test == "volta") {
            lonely_ = true;
            NoteInteraction(true);
        } else if (test == "susto") {
            React("shocked", "Ai! Que susto!", 3);
            StartleSound();
        } else if (test == "musica") {
            dance_until_us_ = esp_timer_get_time() + 15 * 1000000LL;
            Board::GetInstance().GetDisplay()->ShowNotification("Que som!", 3000);
            shown_while_idle_ = false;
        } else if (test == "almoco" || test == "jantar") {
            MealReminder(test == "almoco");
        }
    }

    // Roda na tarefa principal (ferramenta MCP): pode mexer no estado e gravar direto.
    std::string SetTestState(const std::string& state) {
        ESP_LOGW("AstralPet", "MODO TESTE: %s", state.c_str());
        // Cada estado de teste começa do normal, para um teste não misturar com o anterior.
        bool from_normal = state == "faminto" || state == "cheio" || state == "cansado" || state == "bolado" ||
                           state == "doente" || state == "saudade" || state == "volta" || state == "susto" ||
                           state == "musica" || state == "chuva" || state == "frio" || state == "sol" ||
                           state == "semclima";
        if (from_normal || state == "normal") {
            hunger_ = energy_ = mood_ = health_ = 80;
            fainted_ = false;
            sleeping_ = false;
            nausea_until_us_ = 0;
            lonely_ = false;
            dance_until_us_ = 0;
            UpdateBalloon(false);
        }
        if (state == "desmaiar") {
            health_ = 0;
            hunger_ = std::min(hunger_, 20.0f);  // para o balão pedir comida
            energy_ = std::min(energy_, 20.0f);
            CheckFaint();
        } else if (state == "faminto") {
            hunger_ = 10;
        } else if (state == "cheio") {
            hunger_ = 100;
        } else if (state == "cansado") {
            energy_ = 10;
        } else if (state == "bolado") {
            mood_ = 10;
        } else if (state == "doente") {
            health_ = 15;
        } else if (state == "enjoado") {
            MakeNauseous(60);
        } else if (state == "normal") {
            // já voltou tudo ao normal acima
        } else if (state == "saudade") {
            lonely_ = true;
        } else if (state == "chuva") {
            weather_ = kWeatherRain;  // até a próxima consulta do clima (30 min)
        } else if (state == "frio") {
            weather_ = kWeatherCold;
        } else if (state == "sol") {
            weather_ = kWeatherSun;
        } else if (state == "semclima") {
            weather_ = kWeatherNone;
        } else if (state == "volta" || state == "susto" || state == "musica" || state == "almoco" ||
                   state == "jantar") {
            pending_test_ = state;  // acontece quando a conversa fechar
        } else {
            return "{\"ok\":false,\"erro\":\"estado desconhecido\"}";
        }
        shown_while_idle_ = false;
        close_after_reply_ = true;  // fecha a conversa sozinho depois da resposta
        close_saw_reply_ = false;
        close_after_us_ = esp_timer_get_time();
        UpdatePanel(true);
        RefreshDescription();
        Save();
        return "{\"ok\":true,\"estado\":\"" + state + "\"}";
    }
};

#endif  // _ASTRAL_PET_H_
