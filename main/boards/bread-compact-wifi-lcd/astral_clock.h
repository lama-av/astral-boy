#ifndef _ASTRAL_CLOCK_H_
#define _ASTRAL_CLOCK_H_

#include <esp_log.h>
#include <esp_netif_sntp.h>

#include <atomic>
#include <cstdlib>
#include <ctime>

// Astral Boy: hora certa do Brasil.
// O servidor do XiaoZhi acerta o relógio no fuso dele; aqui a placa busca a hora mundial (UTC)
// num servidor NTP e aplica o fuso de Brasília. Alarmes e o tamagotchi só confiam no relógio
// depois dessa sincronização.
class AstralClock {
public:
    static void Init() {
        setenv("TZ", "<-03>3", 1);  // Brasília, sem horário de verão
        tzset();
    }

    // Chamar depois que o Wi-Fi conectar; só inicia uma vez.
    static void StartSync() {
        if (Started().exchange(true)) {
            return;
        }
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        config.sync_cb = [](struct timeval*) {
            if (!Synced().exchange(true)) {
                ESP_LOGI("AstralClock", "Relógio sincronizado (horário de Brasília)");
            }
        };
        esp_netif_sntp_init(&config);
    }

    static bool IsSynced() { return Synced().load(); }

private:
    static std::atomic<bool>& Started() {
        static std::atomic<bool> started{false};
        return started;
    }
    static std::atomic<bool>& Synced() {
        static std::atomic<bool> synced{false};
        return synced;
    }
};

#endif  // _ASTRAL_CLOCK_H_
