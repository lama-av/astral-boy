#ifndef _ASTRAL_GAME_H_
#define _ASTRAL_GAME_H_

#include "application.h"
#include "astral_display.h"
#include "astral_music.h"
#include "astral_pet.h"
#include "astral_radio.h"
#include "audio_codec.h"
#include "board.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

LV_IMAGE_DECLARE(astral_icon_food);
LV_IMAGE_DECLARE(astral_icon_health);
LV_IMAGE_DECLARE(astral_icon_star);
LV_IMAGE_DECLARE(astral_icon_bomb);
LV_IMAGE_DECLARE(astral_icon_banana);
LV_IMAGE_DECLARE(astral_icon_nail);
LV_IMAGE_DECLARE(astral_icon_ball);

// Astral Boy: minigames de 1 botão, jogados por cima do rosto animado (olhos, bochechas, boca).
//  - "Hora do Lanche": uma maçã cai e o dono aperta o BOOT quando ela chega na boca. Acerto = ponto,
//    cara de "delícia" e fica mais rápido; erro (fora de hora ou deixar passar) = perde 1 de 3 vidas,
//    cara triste. Mata a fome, alegra e cansa. Recorde salvo na flash.
//  - "Caça-Estrelas" (jogo de dormir): estrelas e pegadinhas (bomba, banana, prego, bola) aparecem,
//    às vezes duas ao mesmo tempo; aperte só quando houver ESTRELA na tela. Pegadinha = perde 1 de 3
//    vidas. Fica mais rápido e com mais pegadinhas. 10 estrelas: ele dorme e recupera energia.
// Começam pela voz; o BOOT responde no instante da pressão; segurar o BOOT sai.
class AstralGame {
public:
    AstralGame(AstralLcdDisplay* display, AstralPet* pet, AstralRadio* radio)
        : display_(display), pet_(pet), radio_(radio) {
        Settings s("astral_game");
        best_ = s.GetInt("best", 0);
        best_stars_ = s.GetInt("best_stars", 0);
        xTaskCreatePinnedToCoreWithCaps([](void* arg) { static_cast<AstralGame*>(arg)->TaskLoop(); },
                                        "astral_game", 8 * 1024, this, 4, &task_, 1, MALLOC_CAP_SPIRAM);
        RegisterTools();
    }

    bool IsActive() const { return active_.load(); }

    // Clique duplo no BOOT: a única forma de comer é jogando o Lanche.
    void StartSnack() {
        ESP_LOGI(TAG, "Lanche pelo botao");
        Start(Mode::kSnack);
    }

    // Clique triplo no BOOT: Caça-Estrelas (jogo de dormir).
    void StartStars() {
        ESP_LOGI(TAG, "Estrelas pelo botao");
        Start(Mode::kStars);
    }

    // Botão apertado (instante da pressão, sem esperar o clique)
    void Press() {
        if (playing_.load()) {
            pressed_ = true;
        }
    }

    // Segurar o BOOT
    void Quit() { quit_ = true; }

private:
    enum class Mode { kSnack, kStars };

    static constexpr const char* TAG = "AstralGame";
    static constexpr int kFrameMs = 25;
    static constexpr int kMouthY = 178;      // centro da boca do rosto (torus neutro)
    static constexpr int kStartY = 6;        // onde a maçã nasce
    static constexpr int kHitWindow = 36;    // distância (px) aceita como acerto (boca "larga")
    static constexpr int kLives = 3;
    static constexpr int kAppleScale = 640;  // 256 = 1x (ícone 18 px -> ~45 px)
    static constexpr int kAppleSize = 45;
    static constexpr int kStarsToSleep = 10;  // estrelas suficientes para ele dormir no fim (o jogo continua)

    AstralLcdDisplay* display_;
    AstralPet* pet_;
    AstralRadio* radio_;
    TaskHandle_t task_ = nullptr;
    Mode mode_ = Mode::kSnack;
    std::atomic<bool> active_{false};   // jogo pedido ou rodando
    std::atomic<bool> playing_{false};  // aceitando o botão
    std::atomic<bool> pressed_{false};
    std::atomic<bool> quit_{false};
    int best_ = 0;
    int best_stars_ = 0;

    lv_obj_t* layer_ = nullptr;
    lv_obj_t* sprite_ = nullptr;  // maçã ou objeto do Caça-Estrelas
    lv_obj_t* sprite2_ = nullptr; // segundo objeto do Caça-Estrelas
    lv_obj_t* score_label_ = nullptr;
    lv_obj_t* info_box_ = nullptr;
    lv_obj_t* info_label_ = nullptr;
    lv_obj_t* hearts_[kLives] = {};

    // ---------- ferramentas ----------

    void RegisterTools() {
        // Uma ferramenta só (com 'jogo'): o servidor aceita no máximo 32 ferramentas no total.
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.game",
                    "Minigames de 1 botão (a conversa é encerrada e o jogo aparece na tela). jogo='lanche': "
                    "'Hora do Lanche', mata a sua fome; use quando pedirem para jogar ou comer. jogo='estrelas': "
                    "'Caça-Estrelas', o jogo de dormir que recupera a sua energia; use para dormir, ninar, "
                    "descansar, contar carneirinhos ou pegar/contar estrelinhas (NÃO use o cronômetro para isso). "
                    "jogo='recorde': só diz os recordes dos dois jogos. IMPORTANTE: "
                    "chame a ferramenta direto, sem explicar as regras antes (a tela do jogo já explica). Depois "
                    "diga só UMA frase bem curta, como 'Bora!' ou 'Boa noite...', e não repita instruções.",
                    PropertyList({Property("jogo", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        std::string game = p["jogo"].value<std::string>();
                        if (game == "recorde") {
                            return "{\"recorde_lanche\":" + std::to_string(best_) +
                                   ",\"recorde_estrelas\":" + std::to_string(best_stars_) + "}";
                        }
                        return Start(game == "estrelas" ? Mode::kStars : Mode::kSnack);
                    });
    }

    std::string Start(Mode mode) {
        if (active_.exchange(true)) {
            return "{\"ok\":false,\"erro\":\"Ja tem um jogo rodando.\"}";
        }
        mode_ = mode;
        quit_ = false;
        // Desde já: nada de palavra de ativação (evita reabrir a conversa) e nada de relógio grande
        Application::GetInstance().SetWakeWordSuppressed(true);
        display_->SetGameMode(true);
        xTaskNotifyGive(task_);
        return "{\"ok\":true,\"instrucao\":\"O jogo ja vai aparecer na tela com as regras. Responda so com uma "
               "frase bem curta, sem repetir instrucoes.\"}";
    }

    // ---------- som ----------

    static void Tone(float hz, int ms, float volume = 7000.0f) {
        auto codec = Board::GetInstance().GetAudioCodec();
        int rate = codec->output_sample_rate();
        int count = rate * ms / 1000;
        int fade = rate * 6 / 1000;
        std::vector<int16_t> pcm(count);
        for (int i = 0; i < count; i++) {
            float env = std::min({1.0f, i / static_cast<float>(fade), (count - i) / static_cast<float>(fade)});
            pcm[i] = static_cast<int16_t>(std::sin(2.0f * static_cast<float>(M_PI) * hz * i / rate) * volume * env);
        }
        AstralMusic::Play(std::move(pcm));  // mistura na música de fundo, se tocando
    }

    // "Bzzzz": buzina de erro (onda quadrada grave, áspera, com tremidinha).
    static void Buzz(int ms = 420) {
        int rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
        int count = rate * ms / 1000;
        int fade = rate * 8 / 1000;
        std::vector<int16_t> pcm(count);
        float phase = 0;
        for (int i = 0; i < count; i++) {
            float s = static_cast<float>(i) / rate;
            float hz = 118.0f + 6.0f * std::sin(2.0f * static_cast<float>(M_PI) * 22.0f * s);
            phase += hz / rate;
            float sq = (phase - std::floor(phase)) < 0.5f ? 1.0f : -1.0f;
            float env = std::min({1.0f, i / static_cast<float>(fade), (count - i) / static_cast<float>(fade)});
            pcm[i] = static_cast<int16_t>(sq * 4200.0f * env);
        }
        AstralMusic::Play(std::move(pcm));  // mistura na música de fundo, se tocando
    }

    // Mordeu o ar: estalo dos dentes ("nhac") + "bonk" grave descendo.
    static void Whiff() {
        int rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
        std::vector<int16_t> pcm;
        int click = rate * 22 / 1000;
        for (int i = 0; i < click; i++) {
            float noise = static_cast<float>(static_cast<int32_t>(esp_random() % 2001) - 1000) / 1000.0f;
            pcm.push_back(static_cast<int16_t>(noise * 9000.0f * (1.0f - static_cast<float>(i) / click)));
        }
        pcm.insert(pcm.end(), rate * 40 / 1000, 0);
        int len = rate * 200 / 1000;
        float phase = 0;
        for (int i = 0; i < len; i++) {
            float k = static_cast<float>(i) / len;
            float hz = 300.0f - 190.0f * k;
            phase += 2.0f * static_cast<float>(M_PI) * hz / rate;
            float env = std::min(1.0f, i / (rate * 0.004f)) * (1.0f - k);
            pcm.push_back(static_cast<int16_t>((std::sin(phase) + 0.35f * std::sin(2 * phase)) * 6500.0f * env));
        }
        AstralMusic::Play(std::move(pcm));  // mistura na música de fundo, se tocando
    }

    // A maçã passou: apito caindo ("fiuuuu") e um "tum" abafado no chão.
    static void FallAway() {
        int rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
        std::vector<int16_t> pcm;
        int len = rate * 480 / 1000;
        float phase = 0;
        for (int i = 0; i < len; i++) {
            float k = static_cast<float>(i) / len;
            float hz = 950.0f * std::pow(0.26f, k) * (1.0f + 0.015f * std::sin(2.0f * static_cast<float>(M_PI) * 7.0f * i / rate));
            phase += 2.0f * static_cast<float>(M_PI) * hz / rate;
            float env = std::min({1.0f, i / (rate * 0.01f), (len - i) / (rate * 0.02f)});
            pcm.push_back(static_cast<int16_t>(std::sin(phase) * 4500.0f * env));
        }
        int thud = rate * 140 / 1000;
        float prev = 0;
        phase = 0;
        for (int i = 0; i < thud; i++) {
            float k = static_cast<float>(i) / thud;
            float noise = static_cast<float>(static_cast<int32_t>(esp_random() % 2001) - 1000) / 1000.0f;
            prev = prev * 0.9f + noise * 0.1f;
            phase += 2.0f * static_cast<float>(M_PI) * (90.0f - 40.0f * k) / rate;
            float env = std::exp(-7.0f * k);
            pcm.push_back(static_cast<int16_t>((std::sin(phase) * 7000.0f + prev * 14000.0f) * env));
        }
        AstralMusic::Play(std::move(pcm));  // mistura na música de fundo, se tocando
    }

    // "Crunch": duas mordidas de ruído com caída rápida (som de maçã crocante).
    static void Crunch() {
        int rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
        std::vector<int16_t> pcm;
        for (int bite = 0; bite < 2; bite++) {
            int len = rate * (bite == 0 ? 70 : 55) / 1000;
            float prev = 0;
            for (int i = 0; i < len; i++) {
                float noise = static_cast<float>(static_cast<int32_t>(esp_random() % 2001) - 1000) / 1000.0f;
                prev = prev * 0.35f + noise * 0.65f;  // tira um pouco do chiado agudo
                float env = std::exp(-6.0f * i / len) * ((i % (rate / 140)) < rate / 400 ? 1.0f : 0.55f);
                pcm.push_back(static_cast<int16_t>(prev * 9000.0f * env));
            }
            pcm.insert(pcm.end(), rate * 45 / 1000, 0);
        }
        AstralMusic::Play(std::move(pcm));  // mistura na música de fundo, se tocando
    }

    // "Plim": sininho (nota + harmônico) que vai sumindo.
    static void Plim(float hz) {
        int rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
        int len = rate * 450 / 1000;
        std::vector<int16_t> pcm(len);
        for (int i = 0; i < len; i++) {
            float t = static_cast<float>(i) / rate;
            float env = std::exp(-7.0f * t) * std::min(1.0f, i / (rate * 0.004f));
            float v = std::sin(2 * static_cast<float>(M_PI) * hz * t) + 0.45f * std::sin(2 * static_cast<float>(M_PI) * hz * 2.76f * t) +
                      0.2f * std::sin(2 * static_cast<float>(M_PI) * hz * 5.4f * t);
            pcm[i] = static_cast<int16_t>(v * 4200.0f * env);
        }
        AstralMusic::Play(std::move(pcm));  // mistura na música de fundo, se tocando
    }

    // ---------- tela (por cima do rosto animado) ----------

    void Face(const char* emotion) { display_->SetEmotion(emotion); }

    void BuildScene() {
        DisplayLockGuard lock(display_);
        const lv_font_t* font = lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN);
        layer_ = lv_obj_create(lv_layer_top());
        lv_obj_set_size(layer_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_opa(layer_, LV_OPA_TRANSP, 0);  // o rosto aparece por trás
        lv_obj_set_style_border_width(layer_, 0, 0);
        lv_obj_set_style_radius(layer_, 0, 0);
        lv_obj_set_style_pad_all(layer_, 0, 0);
        lv_obj_set_scrollbar_mode(layer_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(layer_, LV_OBJ_FLAG_SCROLLABLE);

        sprite_ = lv_image_create(layer_);
        lv_image_set_src(sprite_, mode_ == Mode::kSnack ? &astral_icon_food : &astral_icon_star);
        lv_image_set_scale(sprite_, kAppleScale);
        lv_obj_add_flag(sprite_, LV_OBJ_FLAG_HIDDEN);
        sprite2_ = lv_image_create(layer_);
        lv_image_set_src(sprite2_, &astral_icon_bomb);
        lv_image_set_scale(sprite2_, kAppleScale);
        lv_obj_add_flag(sprite2_, LV_OBJ_FLAG_HIDDEN);

        score_label_ = lv_label_create(layer_);
        lv_obj_set_style_text_font(score_label_, font, 0);
        lv_obj_set_style_text_color(score_label_, lv_color_hex(0x1A1824), 0);
        lv_obj_set_style_bg_color(score_label_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(score_label_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(score_label_, 8, 0);
        lv_obj_set_style_pad_hor(score_label_, 6, 0);
        lv_obj_align(score_label_, LV_ALIGN_TOP_LEFT, 6, 4);

        for (int i = 0; i < kLives; i++) {
            hearts_[i] = lv_image_create(layer_);
            lv_image_set_src(hearts_[i], &astral_icon_health);
            lv_obj_align(hearts_[i], LV_ALIGN_TOP_RIGHT, -8 - i * 22, 8);
        }

        info_box_ = lv_obj_create(layer_);
        lv_obj_set_size(info_box_, LV_HOR_RES - 30, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(info_box_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(info_box_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(info_box_, 16, 0);
        lv_obj_set_style_border_width(info_box_, 4, 0);
        lv_obj_set_style_border_color(info_box_, lv_color_hex(0x1E90EB), 0);
        lv_obj_set_style_pad_all(info_box_, 8, 0);
        lv_obj_set_scrollbar_mode(info_box_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(info_box_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(info_box_, LV_ALIGN_CENTER, 0, -20);
        info_label_ = lv_label_create(info_box_);
        lv_obj_set_width(info_label_, lv_pct(100));
        lv_obj_set_style_text_font(info_label_, font, 0);
        lv_obj_set_style_text_align(info_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(info_label_, lv_color_hex(0x1A1824), 0);
        lv_obj_center(info_label_);
    }

    void DestroyScene() {
        DisplayLockGuard lock(display_);
        if (layer_ != nullptr) {
            lv_obj_delete(layer_);
            layer_ = nullptr;
        }
    }

    void ShowInfo(const std::string& text) {
        DisplayLockGuard lock(display_);
        lv_label_set_text(info_label_, text.c_str());
        lv_obj_remove_flag(info_box_, LV_OBJ_FLAG_HIDDEN);
    }

    void HideInfo() {
        DisplayLockGuard lock(display_);
        lv_obj_add_flag(info_box_, LV_OBJ_FLAG_HIDDEN);
    }

    void UpdateHud(const char* text, int lives) {
        DisplayLockGuard lock(display_);
        lv_label_set_text(score_label_, text);
        for (int i = 0; i < kLives; i++) {
            if (i < lives) {
                lv_obj_remove_flag(hearts_[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(hearts_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Mostra/esconde um objeto do Caça-Estrelas com a imagem dada.
    void PlaceItem(lv_obj_t* obj, const lv_image_dsc_t* image, int x, int y, bool visible) {
        DisplayLockGuard lock(display_);
        if (image != nullptr) {
            lv_image_set_src(obj, image);
        }
        lv_obj_set_pos(obj, x, y);
        if (visible) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void MoveSprite(int x, int y, bool visible, int scale = kAppleScale) {
        DisplayLockGuard lock(display_);
        lv_obj_set_pos(sprite_, x, y);
        lv_image_set_scale(sprite_, scale);
        if (visible) {
            lv_obj_remove_flag(sprite_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sprite_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // ---------- controle ----------

    static void SetWakeWord(bool enabled) {
        Application::GetInstance().Schedule([enabled]() {
            Application::GetInstance().GetAudioService().EnableWakeWordDetection(enabled);
        });
    }

    // Encerra a conversa e espera o repouso ficar estável. A palavra de ativação é desligada assim
    // que ele fica em repouso (ela chegava a "ouvir" o fim da própria fala e reabria a conversa);
    // se mesmo assim a conversa reabrir, fecha de novo.
    bool WaitForIdle() {
        auto& app = Application::GetInstance();
        int idle_ms = 0;
        int toggles = 0;
        int64_t last_toggle_us = 0;
        for (int waited = 0; waited < 20000; waited += 100) {
            if (quit_.load()) {
                return false;
            }
            DeviceState state = app.GetDeviceState();
            if (state == kDeviceStateIdle) {
                if (idle_ms == 0) {
                    SetWakeWord(false);
                }
                idle_ms += 100;
                if (idle_ms >= 600) {
                    return true;
                }
            } else {
                idle_ms = 0;
                bool can_toggle = toggles < 3 && esp_timer_get_time() - last_toggle_us > 1500000;
                if (state == kDeviceStateListening && can_toggle && app.GetAudioService().IsPlaybackIdle()) {
                    app.ToggleChatState();
                    toggles++;
                    last_toggle_us = esp_timer_get_time();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return false;
    }

    bool StillIdle() const {
        return !quit_.load() && Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    }

    void Wait(int ms) {
        for (int t = 0; t < ms && StillIdle(); t += 50) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    void TaskLoop() {
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (WaitForIdle()) {
                if (radio_ != nullptr) {
                    radio_->Stop();
                }
                if (pet_ != nullptr) {
                    pet_->SetPaused(true);  // o tamagotchi não troca o rosto durante o jogo
                }
                BuildScene();
                if (mode_ == Mode::kSnack) {
                    PlaySnack();
                } else {
                    PlayStars();
                }
                DestroyScene();
                if (pet_ != nullptr) {
                    pet_->SetPaused(false);
                }
            }
            display_->SetGameMode(false);
            Application::GetInstance().SetWakeWordSuppressed(false);
            SetWakeWord(true);  // a palavra de ativação volta ao fim do jogo (ou se ele não começou)
            playing_ = false;
            active_ = false;
        }
    }

    // ---------- Hora do Lanche ----------

    void PlaySnack() {
        int score = 0;
        int lives = kLives;
        UpdateHud("0", lives);
        Face("game_open");
        ShowInfo("Hora do Lanche!\nAperte quando a maçã\nchegar na boca");
        Wait(2500);
        HideInfo();

        // Velocidade em px por SEGUNDO (movimento pelo tempo real, não por quadro: a tela da
        // protoboard redesenha devagar e cada quadro leva bem mais que kFrameMs).
        float speed = 180.0f;
        AstralMusic::StartBgm();  // a música da dança, de fundo
        playing_ = true;
        while (lives > 0 && StillIdle()) {
            float x = 20.0f + static_cast<float>(esp_random() % (LV_HOR_RES - 40 - kAppleSize));
            float y = kStartY;
            float target_x = (LV_HOR_RES - kAppleSize) / 2.0f;
            float fall_seconds = (kMouthY - kAppleSize / 2 - kStartY) / speed;
            float vx = (target_x - x) / fall_seconds;
            bool resolved = false;
            bool hit = false;
            bool bit = false;  // errou mordendo o ar (em vez de deixar a maçã passar)
            pressed_ = false;
            int64_t last_us = esp_timer_get_time();
            while (!resolved && StillIdle()) {
                int64_t now_us = esp_timer_get_time();
                float dt = (now_us - last_us) / 1e6f;
                last_us = now_us;
                y += speed * dt;
                x += vx * dt;
                MoveSprite(static_cast<int>(x), static_cast<int>(y), true);
                float center = y + kAppleSize / 2.0f;
                // a margem cresce com a velocidade: rápido, a maçã anda muitos px entre quadros
                float window = kHitWindow + speed * 0.07f;
                if (pressed_.exchange(false)) {
                    resolved = true;
                    hit = std::fabs(center - kMouthY) <= window;
                    bit = true;
                    Face("game_chomp");  // fecha a boca na hora do clique
                } else if (center > kMouthY + window) {
                    resolved = true;  // passou direto
                }
                vTaskDelay(pdMS_TO_TICKS(kFrameMs));
            }
            MoveSprite(0, 0, false);
            if (!resolved) {
                break;
            }
            bool stuffed = false;
            if (hit) {
                score++;
                speed = std::min(440.0f, speed * 1.07f);
                if (pet_ != nullptr) {
                    stuffed = pet_->IsStuffed();  // já estava de barriga cheia antes dessa maçã
                    pet_->EatApple();  // a barrinha de fome sobe na hora
                }
                Crunch();
                if (stuffed) {
                    Face("nauseous");  // comeu de barriga cheia: enjoa na hora
                    Tone(150.0f, 180, 5000.0f);  // "urp"
                }
            } else {
                lives--;
                // o erro aparece no coração que some; a boca continua no lugar
                if (bit) {
                    Whiff();    // mordeu o ar: nhac + bonk
                } else {
                    FallAway(); // a maçã passou e caiu da tela: fiuuu + tum
                }
            }
            char hud[16];
            snprintf(hud, sizeof(hud), "%d", score);
            UpdateHud(hud, lives);
            Wait(stuffed ? 800 : (hit ? 400 : 500));  // enjoado fica um pouco mais na tela
            Face("game_open");
            Wait(300);  // a troca de rosto leva um instante: a próxima maçã só cai com a boca aberta
        }
        playing_ = false;
        AstralMusic::StopBgm();

        bool new_record = score > best_;
        if (new_record) {
            best_ = score;
            // Gravar na flash não pode partir de uma tarefa com pilha na PSRAM (trava o chip):
            // a gravação vai para a tarefa principal.
            int best = best_;
            Application::GetInstance().Schedule([best]() {
                Settings s("astral_game", true);
                s.SetInt("best", best);
            });
        }
        Face(new_record ? "laughing" : (score > 0 ? "happy" : "sad"));
        char buf[96];
        snprintf(buf, sizeof(buf), "Fim de jogo!\nPontos: %d\n%s %d", score,
                 new_record ? "NOVO RECORDE!" : "Recorde:", best_);
        ShowInfo(buf);
        if (new_record) {
            Tone(988.0f, 120);
            Tone(1319.0f, 120);
            Tone(1760.0f, 220);
        }
        for (int i = 0; i < 80 && !quit_.load(); i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (pet_ != nullptr && score > 0) {
            pet_->AfterGame(score);
        }
        ESP_LOGI(TAG, "Lanche: %d pontos (recorde %d)", score, best_);
    }

    // ---------- Caça-Estrelas ----------

    static int RandomInt(int max) { return static_cast<int>(esp_random() % static_cast<uint32_t>(max)); }

    void PlayStars() {
        const lv_image_dsc_t* decoys[] = {&astral_icon_bomb, &astral_icon_banana, &astral_icon_nail,
                                          &astral_icon_ball};
        const float notes[] = {523.0f, 587.0f, 659.0f, 784.0f, 659.0f, 587.0f};
        int counted = 0;
        int lives = kLives;
        UpdateHud("0", lives);
        Face("relaxed");
        ShowInfo("Caça-Estrelas\nAperte só quando tiver\numa ESTRELA na tela!");
        Wait(3200);
        HideInfo();

        int show_ms = 1500;  // quanto tempo os objetos ficam na tela (diminui)
        int gap_ms = 700;    // pausa entre rodadas (diminui)
        int round = 0;
        AstralMusic::StartBgm();
        playing_ = true;
        // Sem limite: vai até acabarem as vidas. Cada estrela pega acelera o jogo.
        while (lives > 0 && StillIdle()) {
            Wait(gap_ms + RandomInt(300));
            round++;
            // mais pegadinhas e às vezes dois objetos juntos conforme o jogo avança
            int decoy_pct = std::min(60, 30 + round * 2);
            bool two = round > 3 && RandomInt(100) < 35;
            bool first_star = RandomInt(100) >= decoy_pct;
            bool second_star = two && !first_star && RandomInt(100) < 35;
            bool has_star = first_star || second_star;

            // um objeto em cada metade da tela, na faixa de cima (longe da boca e do painel)
            int y1 = 30 + RandomInt(70);
            int y2 = 30 + RandomInt(70);
            int x1 = two ? 6 + RandomInt(LV_HOR_RES / 2 - kAppleSize - 10) : 20 + RandomInt(LV_HOR_RES - 40 - kAppleSize);
            int x2 = LV_HOR_RES / 2 + 4 + RandomInt(LV_HOR_RES / 2 - kAppleSize - 10);
            PlaceItem(sprite_, first_star ? &astral_icon_star : decoys[RandomInt(4)], x1, y1, true);
            if (two) {
                PlaceItem(sprite2_, second_star ? &astral_icon_star : decoys[RandomInt(4)], x2, y2, true);
            }
            Tone(two ? 660.0f : 520.0f, 50, 2500.0f);  // "pop" igual para tudo: não entrega a resposta

            pressed_ = false;
            bool pressed = false;
            for (int t = 0; t < show_ms && StillIdle() && !pressed; t += kFrameMs) {
                pressed = pressed_.exchange(false);
                vTaskDelay(pdMS_TO_TICKS(kFrameMs));
            }
            PlaceItem(sprite_, nullptr, 0, 0, false);
            PlaceItem(sprite2_, nullptr, 0, 0, false);
            if (!pressed) {
                if (has_star) {
                    lives--;  // deixou a estrela fugir: perde 1 vida (senão o jogo nunca acaba)
                    Tone(392.0f, 170, 4500.0f);  // estrela fugiu: tan-tan-taaan
                    Tone(330.0f, 170, 4500.0f);
                    Tone(262.0f, 380, 4500.0f);
                    char hud[16];
                    snprintf(hud, sizeof(hud), "%d", counted);
                    UpdateHud(hud, lives);
                }
                show_ms = std::max(450, show_ms * 98 / 100);
                continue;
            }
            if (has_star) {
                counted++;
                if (pet_ != nullptr) {
                    pet_->CatchStar();  // a barrinha de energia sobe na hora
                }
                Plim(notes[counted % 6] * 1.5f);
                if (counted == kStarsToSleep / 2) {
                    Face("sleepy");
                }
            } else {
                lives--;
                Face("shocked");
                Buzz();  // apertou na pegadinha: bzzzz
                Wait(600);
                Face(counted >= kStarsToSleep / 2 ? "sleepy" : "relaxed");
            }
            char hud[16];
            snprintf(hud, sizeof(hud), "%d", counted);
            UpdateHud(hud, lives);
            if (has_star) {
                // quanto mais acerta, mais rápido fica
                show_ms = std::max(450, show_ms * 91 / 100);
                gap_ms = std::max(200, gap_ms * 92 / 100);
            }
        }
        playing_ = false;
        AstralMusic::StopBgm();
        bool new_record = counted > best_stars_;
        if (new_record) {
            best_stars_ = counted;
            int best = best_stars_;
            Application::GetInstance().Schedule([best]() {  // NVS só na tarefa principal
                Settings s("astral_game", true);
                s.SetInt("best_stars", best);
            });
        }
        bool sleeps = counted >= kStarsToSleep;
        char buf[110];
        snprintf(buf, sizeof(buf), "%s\nEstrelas: %d\n%s %d", sleeps ? "Zzz... boa noite!" : "Ops! Perdi o sono...",
                 counted, new_record ? "NOVO RECORDE!" : "Recorde:", best_stars_);
        ShowInfo(buf);
        if (new_record) {
            Face("laughing");
            Tone(988.0f, 120);
            Tone(1319.0f, 120);
            Tone(1760.0f, 220);
            Wait(900);
        }
        if (sleeps) {
            Face("sleepy");
            Tone(784.0f, 200, 3000.0f);
            Tone(659.0f, 200, 3000.0f);
            Tone(523.0f, 400, 3000.0f);
        } else {
            Face("confused");
            Tone(220.0f, 300);
        }
        for (int i = 0; i < 70 && !quit_.load(); i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (pet_ != nullptr && counted >= kStarsToSleep) {
            pet_->AfterLullaby();
        }
        ESP_LOGI(TAG, "Estrelas: %d pegas (recorde %d)", counted, best_stars_);
    }
};

#endif  // _ASTRAL_GAME_H_
