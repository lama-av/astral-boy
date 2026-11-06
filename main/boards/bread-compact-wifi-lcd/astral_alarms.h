#ifndef _ASTRAL_ALARMS_H_
#define _ASTRAL_ALARMS_H_

#include "application.h"
#include "astral_clock.h"
#include "astral_display.h"
#include "astral_radio.h"
#include "audio_codec.h"
#include "board.h"
#include "display/display.h"
#include "mcp_server.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

// Astral Boy: lembretes, alarmes, timer e cronômetro criados pela voz.
// Lembretes/alarmes ficam salvos na flash (NVS) e disparam com a placa ligada (se houver conversa
// aberta, ela é encerrada para tocar na hora): melodia "bip-bip" repetindo, texto na tela e carinha
// de surpresa. Botão: 1 clique desliga, clique duplo = soneca. Timer e cronômetro usam o relógio interno (não dependem da internet) e
// aparecem grandes no centro da tela enquanto contam.
class AstralAlarms {
public:
    AstralAlarms(AstralRadio* radio, AstralLcdDisplay* display) : radio_(radio), display_(display) {
        AstralClock::Init();
        Load();
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) { static_cast<AstralAlarms*>(arg)->OnTick(); };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "astral_alarms";
        ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, 1000000));
        xTaskCreatePinnedToCoreWithCaps([](void* arg) { static_cast<AstralAlarms*>(arg)->RingLoop(); },
                                        "astral_ring", 6 * 1024, this, 3, &ring_task_, 1, MALLOC_CAP_SPIRAM);
        RegisterTools();
    }

    bool IsRinging() const { return ringing_.load(); }

    // 1 clique (ou segurar) enquanto toca
    void Dismiss() { ringing_ = false; }

    // Clique duplo enquanto toca: toca de novo em 5 minutos
    void Snooze() {
        if (!ringing_.load()) {
            return;
        }
        std::string label;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            label = ringing_label_;
        }
        ringing_ = false;
        Add(label, time(nullptr) + kSnoozeSeconds, false);
        ShowText("Soneca: 5 minutos");
    }

private:
    static constexpr const char* TAG = "AstralAlarms";
    static constexpr int kMaxAlarms = 12;
    static constexpr time_t kSnoozeSeconds = 5 * 60;
    static constexpr int kRingMaxSeconds = 120;

    struct Alarm {
        int id;
        time_t when;  // UTC
        bool daily;
        std::string label;
    };

    AstralRadio* radio_;
    AstralLcdDisplay* display_;
    esp_timer_handle_t timer_ = nullptr;
    TaskHandle_t ring_task_ = nullptr;
    std::mutex mutex_;
    std::vector<Alarm> alarms_;
    int next_id_ = 1;
    std::atomic<bool> ringing_{false};
    std::string ringing_label_;

    // Timer (um por vez) e cronômetro, em microssegundos do relógio interno.
    int64_t timer_deadline_us_ = 0;
    std::string timer_label_;
    bool stopwatch_running_ = false;
    int64_t stopwatch_start_us_ = 0;
    int64_t stopwatch_accum_us_ = 0;
    bool big_clock_shown_ = false;
    bool ringing_is_timer_ = false;

    // ---------- armazenamento ----------

    void Load() {
        Settings s("astral_alarm");
        std::string json = s.GetString("list", "[]");
        next_id_ = s.GetInt("next_id", 1);
        cJSON* root = cJSON_Parse(json.c_str());
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            cJSON* id = cJSON_GetObjectItem(item, "id");
            cJSON* when = cJSON_GetObjectItem(item, "w");
            cJSON* daily = cJSON_GetObjectItem(item, "d");
            cJSON* label = cJSON_GetObjectItem(item, "t");
            if (cJSON_IsNumber(id) && cJSON_IsNumber(when) && cJSON_IsString(label)) {
                alarms_.push_back({id->valueint, static_cast<time_t>(when->valuedouble),
                                   cJSON_IsTrue(daily) != 0, label->valuestring});
            }
        }
        cJSON_Delete(root);
    }

    void SaveLocked() {
        cJSON* root = cJSON_CreateArray();
        for (const auto& a : alarms_) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", a.id);
            cJSON_AddNumberToObject(item, "w", static_cast<double>(a.when));
            cJSON_AddBoolToObject(item, "d", a.daily);
            cJSON_AddStringToObject(item, "t", a.label.c_str());
            cJSON_AddItemToArray(root, item);
        }
        char* text = cJSON_PrintUnformatted(root);
        Settings s("astral_alarm", true);
        s.SetString("list", text);
        s.SetInt("next_id", next_id_);
        cJSON_free(text);
        cJSON_Delete(root);
    }

    int Add(const std::string& label, time_t when, bool daily) {
        std::lock_guard<std::mutex> lock(mutex_);
        int id = next_id_++;
        alarms_.push_back({id, when, daily, label});
        SaveLocked();
        return id;
    }

    // ---------- relógio ----------

    static std::string LocalText(time_t t) {
        struct tm tm_local;
        localtime_r(&t, &tm_local);
        char buf[32];
        strftime(buf, sizeof(buf), "%d/%m %H:%M", &tm_local);
        return buf;
    }

    // Próxima vez que o relógio de Brasília marcar HH:MM.
    static time_t NextAt(int hour, int minute) {
        time_t now = time(nullptr);
        struct tm tm_local;
        localtime_r(&now, &tm_local);
        tm_local.tm_hour = hour;
        tm_local.tm_min = minute;
        tm_local.tm_sec = 0;
        time_t t = mktime(&tm_local);
        if (t <= now) {
            t += 24 * 3600;
        }
        return t;
    }

    static std::string Clock(int64_t us) {
        int64_t s = std::max<int64_t>(0, us / 1000000);
        char buf[16];
        if (s >= 3600) {
            snprintf(buf, sizeof(buf), "%d:%02d:%02d", static_cast<int>(s / 3600), static_cast<int>(s / 60 % 60),
                     static_cast<int>(s % 60));
        } else {
            snprintf(buf, sizeof(buf), "%02d:%02d", static_cast<int>(s / 60), static_cast<int>(s % 60));
        }
        return buf;
    }

    int64_t StopwatchElapsedLocked() const {
        return stopwatch_accum_us_ + (stopwatch_running_ ? esp_timer_get_time() - stopwatch_start_us_ : 0);
    }

    // Texto do relógio grande: "MM:SS", ou "H:MM" a partir de 1 hora (para caber na tela).
    static std::string BigClockText(int64_t us) {
        int64_t s = std::max<int64_t>(0, us / 1000000);
        char buf[16];
        if (s >= 3600) {
            snprintf(buf, sizeof(buf), "%d:%02d", static_cast<int>(s / 3600), static_cast<int>(s / 60 % 60));
        } else {
            snprintf(buf, sizeof(buf), "%02d:%02d", static_cast<int>(s / 60), static_cast<int>(s % 60));
        }
        return buf;
    }

    // Relógio grande no centro: timer contando ou cronômetro rodando.
    void UpdateBigClock(bool show_allowed) {
        std::string text;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (timer_deadline_us_ != 0) {
                text = BigClockText(timer_deadline_us_ - esp_timer_get_time() + 999999);
            } else if (stopwatch_running_) {
                text = BigClockText(StopwatchElapsedLocked());
            }
        }
        if (display_ == nullptr) {
            return;
        }
        if (!show_allowed || text.empty()) {
            if (big_clock_shown_) {
                big_clock_shown_ = false;
                display_->HideBigClock();
            }
            return;
        }
        big_clock_shown_ = true;
        display_->ShowBigClock(text);
    }

    // Abre caminho para o alarme tocar na hora: encerra a conversa ou interrompe a fala.
    static void RequestIdle(DeviceState state) {
        auto& app = Application::GetInstance();
        if (state == kDeviceStateListening) {
            app.ToggleChatState();
        } else if (state == kDeviceStateSpeaking) {
            app.AbortSpeaking(kAbortReasonNone);
        }
    }

    void OnTick() {
        auto& app = Application::GetInstance();
        DeviceState state = app.GetDeviceState();
        bool idle = state == kDeviceStateIdle;
        if (idle) {
            AstralClock::StartSync();
        }
        // Relógio grande em repouso e enquanto ouve; some quando ele fala (para ver o rosto).
        UpdateBigClock((idle || state == kDeviceStateListening) && !ringing_.load());
        if (ringing_.load()) {
            return;
        }

        // Timer: não depende do relógio da internet.
        bool timer_due = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            timer_due = timer_deadline_us_ != 0 && esp_timer_get_time() >= timer_deadline_us_;
        }
        // Lembretes: só com o relógio acertado.
        bool alarm_due = false;
        time_t now = time(nullptr);
        if (AstralClock::IsSynced()) {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& a : alarms_) {
                alarm_due = alarm_due || a.when <= now;
            }
        }
        if (!timer_due && !alarm_due) {
            return;
        }
        if (!idle) {
            RequestIdle(state);  // toca assim que a conversa fechar (1-2 s)
            return;
        }

        std::string label;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (timer_due) {
                timer_deadline_us_ = 0;
                label = timer_label_;
                ringing_is_timer_ = true;
            } else {
                for (auto it = alarms_.begin(); it != alarms_.end(); ++it) {
                    if (it->when > now) {
                        continue;
                    }
                    label = it->label;
                    if (it->daily) {
                        while (it->when <= now) {
                            it->when += 24 * 3600;
                        }
                    } else {
                        alarms_.erase(it);
                    }
                    SaveLocked();
                    break;
                }
                ringing_is_timer_ = false;
            }
            ringing_label_ = label;
        }
        ESP_LOGI(TAG, "%s: %s", timer_due ? "Timer terminou" : "Disparou", label.c_str());
        ringing_ = true;
        xTaskNotifyGive(ring_task_);
    }

    // ---------- toque ----------

    void ShowText(const std::string& text, int duration_ms = 4000) {
        Application::GetInstance().Schedule([text, duration_ms]() {
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->ShowNotification(text, duration_ms);
            }
        });
    }

    // Um "bip-bip" de duas notas, em pedaços de 100 ms.
    static std::vector<std::vector<int16_t>> MakeMelody(int rate) {
        struct Note {
            float hz;
            int ms;
        };
        const Note notes[] = {{880.0f, 140}, {0, 60}, {1175.0f, 160}, {0, 640}};
        std::vector<int16_t> pcm;
        for (const auto& n : notes) {
            int count = rate * n.ms / 1000;
            int fade = rate * 8 / 1000;
            for (int i = 0; i < count; i++) {
                float v = 0;
                if (n.hz > 0) {
                    float env = std::min({1.0f, i / static_cast<float>(fade), (count - i) / static_cast<float>(fade)});
                    v = std::sin(2.0f * static_cast<float>(M_PI) * n.hz * i / rate) * 9000.0f * env;
                }
                pcm.push_back(static_cast<int16_t>(v));
            }
        }
        std::vector<std::vector<int16_t>> chunks;
        size_t step = rate / 10;
        for (size_t i = 0; i < pcm.size(); i += step) {
            chunks.emplace_back(pcm.begin() + i, pcm.begin() + std::min(pcm.size(), i + step));
        }
        return chunks;
    }

    void RingLoop() {
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (!ringing_.load()) {
                continue;
            }
            if (radio_ != nullptr) {
                radio_->Stop();
            }
            std::string label;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                label = ringing_label_;
            }
            Application::GetInstance().Schedule([]() {
                auto display = Board::GetInstance().GetDisplay();
                if (display != nullptr) {
                    display->SetEmotion("surprised");
                }
            });
            auto& app = Application::GetInstance();
            auto& audio = app.GetAudioService();
            const auto melody = MakeMelody(Board::GetInstance().GetAudioCodec()->output_sample_rate());
            const bool is_timer = ringing_is_timer_;
            ShowText((is_timer ? "Timer: " : "Lembrete: ") + label, 8000);
            int64_t start = esp_timer_get_time();
            int64_t last_text = esp_timer_get_time();
            while (ringing_.load() && esp_timer_get_time() - start < kRingMaxSeconds * 1000000LL) {
                if (app.GetDeviceState() != kDeviceStateIdle) {
                    break;  // começou uma conversa: para de tocar
                }
                if (esp_timer_get_time() - last_text > 7000000) {
                    ShowText((is_timer ? "Timer: " : "Lembrete: ") + label, 8000);
                    last_text = esp_timer_get_time();
                }
                for (const auto& chunk : melody) {
                    if (!ringing_.load()) {
                        break;
                    }
                    std::vector<int16_t> copy = chunk;
                    while (ringing_.load() && !audio.PushPcmToPlaybackQueue(std::move(copy), 300)) {
                    }
                }
            }
            ringing_ = false;
        }
    }

    // ---------- ferramentas para a IA ----------

    void RegisterTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.reminder.add",
                    "Cria um lembrete ou alarme no próprio Astral Boy (toca um som e mostra o texto na hora). "
                    "Use 'em_minutos' para 'daqui a X minutos' OU 'horario' no formato HH:MM (24 h, horário de "
                    "Brasília) para hora marcada. 'todo_dia' = true repete todos os dias nesse horário. "
                    "'texto' é o que lembrar, curto (ex.: 'tomar água', 'acordar').",
                    PropertyList({Property("texto", kPropertyTypeString),
                                  Property("em_minutos", kPropertyTypeInteger, 0, 0, 10080),
                                  Property("horario", kPropertyTypeString, std::string("")),
                                  Property("todo_dia", kPropertyTypeBoolean, false)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        return AddFromTool(p["texto"].value<std::string>(), p["em_minutos"].value<int>(),
                                           p["horario"].value<std::string>(), p["todo_dia"].value<bool>());
                    });
        // Ferramentas agrupadas com 'acao': o servidor aceita no máximo 32 ferramentas no total.
        mcp.AddTool("self.reminder.manage",
                    "Lembretes e alarmes já criados. acao='listar': mostra todos (com id) e a hora atual de "
                    "Brasília. acao='cancelar': apaga o lembrete do 'id' informado (liste antes para saber o id).",
                    PropertyList({Property("acao", kPropertyTypeString),
                                  Property("id", kPropertyTypeInteger, 0, 0, 100000)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        if (ToLower(p["acao"].value<std::string>()) != "cancelar") {
                            return ListJson();
                        }
                        int id = p["id"].value<int>();
                        std::lock_guard<std::mutex> lock(mutex_);
                        auto it = std::find_if(alarms_.begin(), alarms_.end(),
                                               [id](const Alarm& a) { return a.id == id; });
                        if (it == alarms_.end()) {
                            return std::string("{\"ok\":false,\"erro\":\"id nao encontrado\"}");
                        }
                        alarms_.erase(it);
                        SaveLocked();
                        return std::string("{\"ok\":true}");
                    });
        mcp.AddTool("self.timer",
                    "Timer (contagem regressiva) que toca quando acabar; a contagem aparece grande na tela. "
                    "acao='iniciar' com 'minutos' e/ou 'segundos' (ex.: 'timer de 10 minutos'; substitui o "
                    "anterior). acao='status': quanto falta. acao='cancelar': cancela.",
                    PropertyList({Property("acao", kPropertyTypeString),
                                  Property("minutos", kPropertyTypeInteger, 0, 0, 600),
                                  Property("segundos", kPropertyTypeInteger, 0, 0, 3599),
                                  Property("texto", kPropertyTypeString, std::string("Timer"))}),
                    [this](const PropertyList& p) -> ReturnValue {
                        std::string action = ToLower(p["acao"].value<std::string>());
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (action == "cancelar") {
                            timer_deadline_us_ = 0;
                            return std::string("{\"ok\":true}");
                        }
                        if (action == "status") {
                            if (timer_deadline_us_ == 0) {
                                return std::string("{\"ativo\":false}");
                            }
                            return "{\"ativo\":true,\"falta\":\"" +
                                   Clock(timer_deadline_us_ - esp_timer_get_time()) + "\"}";
                        }
                        int64_t seconds = p["minutos"].value<int>() * 60LL + p["segundos"].value<int>();
                        if (seconds <= 0) {
                            return std::string("{\"ok\":false,\"erro\":\"Informe minutos ou segundos.\"}");
                        }
                        timer_deadline_us_ = esp_timer_get_time() + seconds * 1000000LL;
                        std::string label = p["texto"].value<std::string>();
                        timer_label_ = label.empty() ? "Timer" : label.substr(0, 40);
                        return "{\"ok\":true,\"duracao\":\"" + Clock(seconds * 1000000LL) + "\"}";
                    });
        mcp.AddTool("self.stopwatch",
                    "Cronômetro; o tempo aparece grande na tela enquanto roda. acao='iniciar' (ou continuar), "
                    "'pausar' (diz o tempo marcado), 'ler' (tempo atual) ou 'zerar'. Só use quando o usuário pedir "
                    "CRONÔMETRO explicitamente. NUNCA use para jogos: contar/pegar estrelinhas é self.game "
                    "jogo='estrelas'.",
                    PropertyList({Property("acao", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        std::string action = ToLower(p["acao"].value<std::string>());
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (action == "iniciar" || action == "continuar") {
                            if (!stopwatch_running_) {
                                stopwatch_running_ = true;
                                stopwatch_start_us_ = esp_timer_get_time();
                            }
                        } else if (action == "pausar" || action == "parar") {
                            stopwatch_accum_us_ = StopwatchElapsedLocked();
                            stopwatch_running_ = false;
                        } else if (action == "zerar") {
                            stopwatch_running_ = false;
                            stopwatch_accum_us_ = 0;
                        }
                        return "{\"rodando\":" + std::string(stopwatch_running_ ? "true" : "false") +
                               ",\"tempo\":\"" + Clock(StopwatchElapsedLocked()) + "\"}";
                    });
    }

    static std::string ToLower(std::string s) {
        for (auto& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    std::string AddFromTool(const std::string& label, int minutes, const std::string& at, bool daily) {
        if (!AstralClock::IsSynced()) {
            return "{\"ok\":false,\"erro\":\"O relogio ainda nao foi acertado pela internet. Tente em um minuto.\"}";
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (static_cast<int>(alarms_.size()) >= kMaxAlarms) {
                return "{\"ok\":false,\"erro\":\"Limite de 12 lembretes. Cancele algum antes.\"}";
            }
        }
        time_t when = 0;
        int hour = 0, minute = 0;
        if (!at.empty() && sscanf(at.c_str(), "%d:%d", &hour, &minute) == 2 && hour >= 0 && hour < 24 &&
            minute >= 0 && minute < 60) {
            when = NextAt(hour, minute);
        } else if (minutes > 0) {
            when = time(nullptr) + minutes * 60;
            daily = false;
        } else {
            return "{\"ok\":false,\"erro\":\"Informe em_minutos ou horario HH:MM.\"}";
        }
        std::string text = label.empty() ? "Lembrete" : label.substr(0, 60);
        int id = Add(text, when, daily);
        return "{\"ok\":true,\"id\":" + std::to_string(id) + ",\"quando\":\"" + LocalText(when) +
               "\",\"todo_dia\":" + (daily ? "true" : "false") + "}";
    }

    std::string ListJson() {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "agora", AstralClock::IsSynced() ? LocalText(time(nullptr)).c_str()
                                                                        : "relogio nao acertado");
        cJSON* list = cJSON_AddArrayToObject(root, "lembretes");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& a : alarms_) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id", a.id);
                cJSON_AddStringToObject(item, "texto", a.label.c_str());
                cJSON_AddStringToObject(item, "quando", LocalText(a.when).c_str());
                cJSON_AddBoolToObject(item, "todo_dia", a.daily);
                cJSON_AddItemToArray(list, item);
            }
        }
        char* text = cJSON_PrintUnformatted(root);
        std::string out(text);
        cJSON_free(text);
        cJSON_Delete(root);
        return out;
    }
};

#endif  // _ASTRAL_ALARMS_H_
