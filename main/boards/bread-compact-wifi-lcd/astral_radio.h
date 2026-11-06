#ifndef _ASTRAL_RADIO_H_
#define _ASTRAL_RADIO_H_

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "mcp_server.h"

#include <cJSON.h>
#include <esp_ae_rate_cvt.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <esp_aac_dec.h>
#include <esp_mp3_dec.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// Astral Boy: rádio pela internet.
// A IA chama self.radio.play com um gênero ou nome; a placa procura no radio-browser.info
// (catálogo aberto e gratuito, com rádios do mundo todo), escolhe uma estação MP3/AAC e toca.
// A música só toca com o aparelho em repouso: a conversa é encerrada antes de começar,
// e apertar o botão para conversar (ou pedir para parar) desliga a rádio.
class AstralRadio {
public:
    AstralRadio() {
        esp_mp3_dec_register();
        esp_aac_dec_register();
        esp_audio_simple_dec_register_default();
        xTaskCreatePinnedToCoreWithCaps([](void* arg) { static_cast<AstralRadio*>(arg)->TaskLoop(); },
                                        "astral_radio", 12 * 1024, this, 3, &task_, 1,
                                        MALLOC_CAP_SPIRAM);
        RegisterTools();
    }

    void Stop() { session_++; }

private:
    static constexpr const char* TAG = "AstralRadio";
    static constexpr float kGain = 0.7f;              // rádios costumam vir bem altas
    static constexpr int kChunkMs = 100;              // tamanho de cada pedaço na fila de som
    static constexpr int kWaitIdleMs = 25000;         // espera a conversa acabar antes de tocar
    static constexpr const char* kApiHosts[] = {"de1.api.radio-browser.info", "de2.api.radio-browser.info",
                                                "fi1.api.radio-browser.info"};

    struct Station {
        std::string name;
        std::string url;
        std::string country;
        std::string codec;
        int bitrate = 0;
    };

    TaskHandle_t task_ = nullptr;
    std::mutex mutex_;
    Station requested_;
    Station playing_;
    std::atomic<uint32_t> session_{0};  // muda a cada pedido de tocar/parar
    std::atomic<bool> active_{false};

    // ---------- ferramentas para a IA ----------

    void RegisterTools() {
        // Uma ferramenta só (com 'acao'): o servidor aceita no máximo 32 ferramentas no total.
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.radio",
                    "Rádio pela internet no alto-falante. acao='tocar': toca uma rádio; 'busca' é um gênero "
                    "(rock, mpb, samba, jazz, lofi, sertanejo, pop, clássica, notícias) ou o nome da rádio, "
                    "'pais' é o código de 2 letras (BR para brasileiras) ou vazio para qualquer país; depois "
                    "avise em uma frase curta qual rádio vai tocar (a conversa é encerrada e a música começa). "
                    "acao='parar': para a rádio. acao='qual': diz o que está tocando.",
                    PropertyList({Property("acao", kPropertyTypeString),
                                  Property("busca", kPropertyTypeString, std::string("")),
                                  Property("pais", kPropertyTypeString, std::string(""))}),
                    [this](const PropertyList& p) -> ReturnValue {
                        std::string action = ToLower(p["acao"].value<std::string>());
                        if (action == "parar") {
                            Stop();
                            return std::string("{\"ok\":true}");
                        }
                        if (action == "qual") {
                            return NowPlaying();
                        }
                        return Play(p["busca"].value<std::string>(), p["pais"].value<std::string>());
                    });
    }

    std::string NowPlaying() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_.load()) {
            return "{\"tocando\":false}";
        }
        return "{\"tocando\":true,\"radio\":\"" + JsonSafe(playing_.name) + "\",\"pais\":\"" +
               JsonSafe(playing_.country) + "\"}";
    }

    std::string Play(const std::string& query, const std::string& country) {
        if (task_ == nullptr) {
            return "{\"ok\":false,\"erro\":\"O tocador de radio nao iniciou.\"}";
        }
        Station station;
        if (!FindStation(query, country, station)) {
            return "{\"ok\":false,\"erro\":\"Nenhuma radio encontrada para '" + JsonSafe(query) +
                   "'. Tente outro genero ou nome.\"}";
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested_ = station;
        }
        session_++;
        xTaskNotifyGive(task_);
        char buf[384];
        snprintf(buf, sizeof(buf),
                 "{\"ok\":true,\"radio\":\"%s\",\"pais\":\"%s\",\"formato\":\"%s %dkbps\"}",
                 JsonSafe(station.name).c_str(), JsonSafe(station.country).c_str(), station.codec.c_str(),
                 station.bitrate);
        return std::string(buf);
    }

    // ---------- busca no radio-browser.info ----------

    static std::string UrlEncode(const std::string& s) {
        std::string out;
        char hex[4];
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += static_cast<char>(c);
            } else {
                snprintf(hex, sizeof(hex), "%%%02X", c);
                out += hex;
            }
        }
        return out;
    }

    static std::string JsonSafe(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"' || c == '\\') {
                out += '\'';
            } else if (static_cast<unsigned char>(c) >= 0x20) {
                out += c;
            }
        }
        return out;
    }

    static std::string ToLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    bool FindStation(const std::string& query, const std::string& country, Station& out) {
        std::string q = ToLower(query);
        std::string filter = country.empty() ? "" : "&countrycode=" + UrlEncode(country);
        // Primeiro como gênero (tag), depois como nome da rádio.
        for (const char* field : {"tag", "name"}) {
            std::string path = std::string("/json/stations/search?") + field + "=" + UrlEncode(q) + filter +
                               "&hidebroken=true&order=clickcount&reverse=true&limit=20";
            for (const char* host : kApiHosts) {
                std::string body;
                if (!HttpGetText(std::string("https://") + host + path, body)) {
                    continue;
                }
                if (PickStation(body, out)) {
                    return true;
                }
                break;  // o servidor respondeu, só não achou nada: tenta o outro campo
            }
        }
        return false;
    }

    static bool PickStation(const std::string& json, Station& out) {
        cJSON* root = cJSON_Parse(json.c_str());
        if (!cJSON_IsArray(root)) {
            cJSON_Delete(root);
            return false;
        }
        bool found = false;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            auto str = [item](const char* key) {
                cJSON* v = cJSON_GetObjectItem(item, key);
                return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
            };
            cJSON* hls = cJSON_GetObjectItem(item, "hls");
            if (cJSON_IsNumber(hls) && hls->valueint != 0) {
                continue;
            }
            std::string codec = str("codec");
            if (codec != "MP3" && codec != "AAC" && codec != "AAC+") {
                continue;
            }
            std::string url = str("url_resolved");
            if (url.empty()) {
                url = str("url");
            }
            std::string lower = ToLower(url);
            if (url.empty() || lower.find(".m3u") != std::string::npos || lower.find(".pls") != std::string::npos) {
                continue;
            }
            cJSON* br = cJSON_GetObjectItem(item, "bitrate");
            out.name = str("name");
            out.url = url;
            out.country = str("country");
            out.codec = codec;
            out.bitrate = cJSON_IsNumber(br) ? br->valueint : 0;
            found = true;
            break;
        }
        cJSON_Delete(root);
        return found;
    }

    static bool HttpGetText(const std::string& url, std::string& body) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        http->SetTimeout(8000);
        http->SetHeader("User-Agent", "AstralBoy/1.0");
        if (auto opened = http->Open("GET", url); !opened) {
            ESP_LOGW(TAG, "Busca falhou em %s: %s", url.c_str(), opened.error().ToString().c_str());
            return false;
        }
        auto status = http->GetStatusCode();
        if (!status || *status != 200) {
            http->Close();
            return false;
        }
        body = http->ReadAll();
        http->Close();
        return !body.empty();
    }

    // ---------- tocador ----------

    void TaskLoop() {
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            uint32_t session = session_.load();
            Station station;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                station = requested_;
            }
            if (!WaitForIdle(session)) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                playing_ = station;
            }
            active_ = true;
            ESP_LOGI(TAG, "Tocando %s (%s)", station.name.c_str(), station.url.c_str());
            // Se a transmissão cair, tenta reconectar algumas vezes antes de desistir.
            for (int attempt = 0; attempt < 5 && StillWanted(session); attempt++) {
                if (attempt > 0) {
                    ESP_LOGW(TAG, "Reconectando (%d/4)", attempt);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                StreamStation(station, session);
            }
            active_ = false;
            ESP_LOGI(TAG, "Rádio parada");
        }
    }

    // Espera a IA terminar de falar e encerra a conversa, para tocar com o microfone desligado.
    bool WaitForIdle(uint32_t session) {
        auto& app = Application::GetInstance();
        bool closed_chat = false;
        for (int waited = 0; waited < kWaitIdleMs; waited += 200) {
            if (session_.load() != session) {
                return false;
            }
            DeviceState state = app.GetDeviceState();
            if (state == kDeviceStateIdle) {
                return true;
            }
            if (state == kDeviceStateListening && !closed_chat && app.GetAudioService().IsPlaybackIdle()) {
                app.ToggleChatState();  // no modo "ouvindo", alternar encerra a conversa
                closed_chat = true;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        ESP_LOGW(TAG, "A conversa não terminou a tempo; rádio cancelada");
        return false;
    }

    bool StillWanted(uint32_t session) {
        return session_.load() == session &&
               Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    }

    void StreamStation(const Station& station, uint32_t session) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        std::string url = station.url;
        bool opened_ok = false;
        for (int redirects = 0; redirects < 4; redirects++) {
            http = Board::GetInstance().GetNetwork()->CreateHttp(0);
            http->SetTimeout(10000);
            http->SetHeader("User-Agent", "AstralBoy/1.0");
            if (auto opened = http->Open("GET", url); !opened) {
                ESP_LOGW(TAG, "Não abriu %s: %s", url.c_str(), opened.error().ToString().c_str());
                return;
            }
            auto status = http->GetStatusCode();
            if (!status) {
                return;
            }
            if (*status >= 300 && *status < 400) {
                url = http->GetResponseHeader("Location");
                http->Close();
                if (url.empty()) {
                    return;
                }
                continue;
            }
            if (*status != 200) {
                ESP_LOGW(TAG, "Rádio respondeu %d", *status);
                http->Close();
                return;
            }
            opened_ok = true;
            break;
        }
        if (!opened_ok) {
            return;
        }

        std::string content_type = ToLower(http->GetResponseHeader("Content-Type"));
        bool is_aac = content_type.find("aac") != std::string::npos ||
                      (content_type.find("mpeg") == std::string::npos && station.codec != "MP3");
        esp_audio_simple_dec_cfg_t cfg = {};
        cfg.dec_type = is_aac ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        esp_audio_simple_dec_handle_t decoder = nullptr;
        if (esp_audio_simple_dec_open(&cfg, &decoder) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Decodificador não abriu");
            http->Close();
            return;
        }

        auto codec = Board::GetInstance().GetAudioCodec();
        const int out_rate = codec->output_sample_rate();
        const size_t chunk_samples = out_rate * kChunkMs / 1000;
        auto& audio = Application::GetInstance().GetAudioService();

        constexpr size_t kInSize = 4096;
        auto* in = static_cast<uint8_t*>(heap_caps_malloc(kInSize, MALLOC_CAP_SPIRAM));
        std::vector<uint8_t> out(8192);
        std::vector<int16_t> mono;
        std::vector<int16_t> resampled;
        std::vector<int16_t> chunk;
        chunk.reserve(chunk_samples * 2);
        esp_ae_rate_cvt_handle_t resampler = nullptr;
        uint32_t resampler_rate = 0;

        while (in != nullptr && StillWanted(session)) {
            auto got = http->Read(reinterpret_cast<char*>(in), kInSize);
            if (!got || *got <= 0) {
                ESP_LOGW(TAG, "Transmissão terminou");
                break;
            }
            esp_audio_simple_dec_raw_t raw = {};
            raw.buffer = in;
            raw.len = *got;
            while (raw.len > 0 && StillWanted(session)) {
                esp_audio_simple_dec_out_t frame = {};
                frame.buffer = out.data();
                frame.len = out.size();
                esp_audio_err_t ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    out.resize(frame.needed_size);
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    break;  // pedaço estragado: descarta e segue com o próximo
                }
                if (frame.decoded_size > 0) {
                    esp_audio_simple_dec_info_t info = {};
                    esp_audio_simple_dec_get_info(decoder, &info);
                    int channels = std::max<int>(1, info.channel);
                    size_t samples = frame.decoded_size / sizeof(int16_t) / channels;
                    auto* pcm = reinterpret_cast<int16_t*>(out.data());
                    mono.resize(samples);
                    for (size_t i = 0; i < samples; i++) {
                        int sum = 0;
                        for (int c = 0; c < channels; c++) {
                            sum += pcm[i * channels + c];
                        }
                        mono[i] = static_cast<int16_t>(sum / channels * kGain);
                    }
                    const std::vector<int16_t>* ready = &mono;
                    if (static_cast<int>(info.sample_rate) != out_rate && info.sample_rate > 0) {
                        if (resampler == nullptr || resampler_rate != info.sample_rate) {
                            if (resampler != nullptr) {
                                esp_ae_rate_cvt_close(resampler);
                                resampler = nullptr;
                            }
                            esp_ae_rate_cvt_cfg_t rc = {};
                            rc.src_rate = info.sample_rate;
                            rc.dest_rate = out_rate;
                            rc.channel = 1;
                            rc.bits_per_sample = ESP_AUDIO_BIT16;
                            rc.complexity = 2;
                            rc.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                            esp_ae_rate_cvt_open(&rc, &resampler);
                            resampler_rate = info.sample_rate;
                        }
                        if (resampler != nullptr) {
                            uint32_t max_out = 0;
                            esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono.size(), &max_out);
                            resampled.resize(max_out);
                            uint32_t produced = max_out;
                            esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t)mono.data(), mono.size(),
                                                    (esp_ae_sample_t)resampled.data(), &produced);
                            resampled.resize(produced);
                            ready = &resampled;
                        }
                    }
                    chunk.insert(chunk.end(), ready->begin(), ready->end());
                    while (chunk.size() >= chunk_samples && StillWanted(session)) {
                        std::vector<int16_t> piece(chunk.begin(), chunk.begin() + chunk_samples);
                        chunk.erase(chunk.begin(), chunk.begin() + chunk_samples);
                        while (StillWanted(session) && !audio.PushPcmToPlaybackQueue(std::move(piece), 300)) {
                        }
                    }
                }
                if (raw.consumed == 0 && frame.decoded_size == 0) {
                    break;  // decodificador precisa de mais dados
                }
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
            }
        }

        if (resampler != nullptr) {
            esp_ae_rate_cvt_close(resampler);
        }
        esp_audio_simple_dec_close(decoder);
        heap_caps_free(in);
        http->Close();
    }
};

#endif  // _ASTRAL_RADIO_H_
