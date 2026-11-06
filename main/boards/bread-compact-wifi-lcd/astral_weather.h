#ifndef _ASTRAL_WEATHER_H_
#define _ASTRAL_WEATHER_H_

#include "application.h"
#include "astral_pet.h"
#include "board.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <string>

// Astral Boy: o tempo de verdade lá fora muda o rosto dele (sem conta nem chave):
//  - local aproximado pelo IP da internet (ip-api.com, de graça)
//  - tempo agora pelo Open-Meteo, a cada 30 min
// chuva = guarda-chuva, frio = gorro, sol de dia = óculos escuros.
class AstralWeather {
public:
    explicit AstralWeather(AstralPet* pet) : pet_(pet) {
        xTaskCreatePinnedToCoreWithCaps([](void* arg) { static_cast<AstralWeather*>(arg)->TaskLoop(); },
                                        "astral_weather", 10 * 1024, this, 2, nullptr, 1, MALLOC_CAP_SPIRAM);
    }

private:
    static constexpr const char* TAG = "AstralWeather";
    static constexpr int kColdC = 17;  // abaixo disso (sensação) ele sente frio

    AstralPet* pet_;
    double lat_ = 0, lon_ = 0;
    bool located_ = false;

    static std::string Get(const std::string& url) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        http->SetTimeout(10000);
        if (auto opened = http->Open("GET", url); !opened) {
            return "";
        }
        auto status = http->GetStatusCode();
        if (!status || *status != 200) {
            http->Close();
            return "";
        }
        std::string body = http->ReadAll();
        http->Close();
        return body;
    }

    bool Locate() {
        cJSON* root = cJSON_Parse(Get("http://ip-api.com/json/?fields=status,city,lat,lon").c_str());
        cJSON* lat = cJSON_GetObjectItem(root, "lat");
        cJSON* lon = cJSON_GetObjectItem(root, "lon");
        cJSON* city = cJSON_GetObjectItem(root, "city");
        bool ok = cJSON_IsNumber(lat) && cJSON_IsNumber(lon);
        if (ok) {
            lat_ = lat->valuedouble;
            lon_ = lon->valuedouble;
            ESP_LOGI(TAG, "Local aproximado: %s", cJSON_IsString(city) ? city->valuestring : "?");
        }
        cJSON_Delete(root);
        return ok;
    }

    void Update() {
        char url[256];
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
                 "&current=apparent_temperature,weather_code,is_day&timezone=auto",
                 lat_, lon_);
        cJSON* root = cJSON_Parse(Get(url).c_str());
        cJSON* current = cJSON_GetObjectItem(root, "current");
        cJSON* temp = cJSON_GetObjectItem(current, "apparent_temperature");
        cJSON* code = cJSON_GetObjectItem(current, "weather_code");
        cJSON* day = cJSON_GetObjectItem(current, "is_day");
        if (cJSON_IsNumber(temp) && cJSON_IsNumber(code)) {
            int c = code->valueint;
            int t = static_cast<int>(temp->valuedouble + 0.5);
            bool rain = (c >= 51 && c <= 67) || (c >= 80 && c <= 82) || c >= 95;
            bool sunny = c <= 1 && cJSON_IsNumber(day) && day->valueint == 1;
            int kind = rain ? AstralPet::kWeatherRain
                     : t < kColdC ? AstralPet::kWeatherCold
                     : sunny ? AstralPet::kWeatherSun
                     : AstralPet::kWeatherNone;
            ESP_LOGI(TAG, "Tempo: codigo %d, sensacao %d C, dia %d -> %d", c, t,
                     cJSON_IsNumber(day) ? day->valueint : -1, kind);
            pet_->SetWeather(kind, t);
        }
        cJSON_Delete(root);
    }

    void TaskLoop() {
        vTaskDelay(pdMS_TO_TICKS(30000));  // espera o Wi-Fi e o servidor no começo
        while (true) {
            auto state = Application::GetInstance().GetDeviceState();
            bool online = state == kDeviceStateIdle || state == kDeviceStateListening ||
                          state == kDeviceStateSpeaking;
            if (online) {
                if (!located_) {
                    located_ = Locate();
                }
                if (located_) {
                    Update();
                }
            }
            vTaskDelay(pdMS_TO_TICKS((located_ ? 30 : 2) * 60 * 1000));
        }
    }
};

#endif  // _ASTRAL_WEATHER_H_
