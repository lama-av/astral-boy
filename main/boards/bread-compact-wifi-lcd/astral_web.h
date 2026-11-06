#ifndef _ASTRAL_WEB_H_
#define _ASTRAL_WEB_H_

#include "board.h"
#include "mcp_server.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

// Astral Boy: consultas grátis na internet, sem conta nem chave, como ferramentas para a IA:
//   - Wikipédia em português (resumo do artigo)
//   - Previsão do tempo do Open-Meteo (geocodificação + previsão, fuso de São Paulo)
//   - Manchetes do Google Notícias Brasil (reserva: G1)
// Tudo roda na própria placa; o texto devolvido é curto para a IA resumir em voz.
class AstralWeb {
public:
    AstralWeb() { RegisterTools(); }

private:
    static constexpr const char* TAG = "AstralWeb";
    static constexpr const char* kUserAgent = "AstralBoy/1.0 (companheiro de mesa pessoal; ESP32)";

    // ---------- utilidades ----------

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

    // GET com até 3 redirecionamentos; devolve o corpo (vazio se falhar).
    static std::string HttpGet(std::string url) {
        for (int redirects = 0; redirects < 4; redirects++) {
            auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
            http->SetTimeout(10000);
            http->SetHeader("User-Agent", kUserAgent);
            if (auto opened = http->Open("GET", url); !opened) {
                ESP_LOGW(TAG, "Falhou %s: %s", url.c_str(), opened.error().ToString().c_str());
                return "";
            }
            auto status = http->GetStatusCode();
            if (!status) {
                http->Close();
                return "";
            }
            if (*status >= 300 && *status < 400) {
                url = http->GetResponseHeader("Location");
                http->Close();
                if (url.empty()) {
                    return "";
                }
                continue;
            }
            if (*status != 200) {
                ESP_LOGW(TAG, "%s respondeu %d", url.c_str(), *status);
                http->Close();
                return "";
            }
            std::string body = http->ReadAll();
            http->Close();
            return body;
        }
        return "";
    }

    static std::string Truncate(const std::string& s, size_t max) {
        if (s.size() <= max) {
            return s;
        }
        size_t cut = s.rfind(' ', max);
        // não corta no meio de um caractere UTF-8
        while (cut != std::string::npos && cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
            cut--;
        }
        return s.substr(0, cut == std::string::npos ? max : cut) + "...";
    }

    static std::string ToJson(cJSON* root) {
        char* text = cJSON_PrintUnformatted(root);
        std::string out = text != nullptr ? text : "{}";
        cJSON_free(text);
        cJSON_Delete(root);
        return out;
    }

    static std::string Error(const std::string& message) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "erro", message.c_str());
        return ToJson(root);
    }

    // ---------- Wikipédia ----------

    std::string Wikipedia(const std::string& topic) {
        std::string search = HttpGet("https://pt.wikipedia.org/w/rest.php/v1/search/title?limit=1&q=" +
                                     UrlEncode(topic));
        cJSON* root = cJSON_Parse(search.c_str());
        cJSON* pages = cJSON_GetObjectItem(root, "pages");
        cJSON* first = cJSON_IsArray(pages) ? cJSON_GetArrayItem(pages, 0) : nullptr;
        cJSON* key = cJSON_GetObjectItem(first, "key");
        std::string page_key = cJSON_IsString(key) ? key->valuestring : "";
        cJSON_Delete(root);
        if (page_key.empty()) {
            return Error("Nao achei artigo na Wikipedia sobre '" + topic + "'.");
        }

        std::string summary = HttpGet("https://pt.wikipedia.org/api/rest_v1/page/summary/" + UrlEncode(page_key));
        root = cJSON_Parse(summary.c_str());
        cJSON* title = cJSON_GetObjectItem(root, "title");
        cJSON* extract = cJSON_GetObjectItem(root, "extract");
        if (!cJSON_IsString(extract)) {
            cJSON_Delete(root);
            return Error("Nao consegui ler o resumo da Wikipedia.");
        }
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "artigo", cJSON_IsString(title) ? title->valuestring : page_key.c_str());
        cJSON_AddStringToObject(out, "resumo", Truncate(extract->valuestring, 900).c_str());
        cJSON_AddStringToObject(out, "fonte", "Wikipedia em portugues");
        cJSON_Delete(root);
        return ToJson(out);
    }

    // ---------- tempo (Open-Meteo) ----------

    static const char* WeatherText(int code) {
        if (code == 0) return "ceu limpo";
        if (code <= 2) return "parcialmente nublado";
        if (code == 3) return "nublado";
        if (code == 45 || code == 48) return "neblina";
        if (code >= 51 && code <= 57) return "garoa";
        if (code >= 61 && code <= 67) return "chuva";
        if (code >= 71 && code <= 77) return "neve";
        if (code >= 80 && code <= 82) return "pancadas de chuva";
        if (code >= 85 && code <= 86) return "pancadas de neve";
        if (code >= 95) return "tempestade com trovoadas";
        return "tempo indefinido";
    }

    std::string Weather(const std::string& city) {
        std::string geo = HttpGet("https://geocoding-api.open-meteo.com/v1/search?count=1&language=pt&format=json&name=" +
                                  UrlEncode(city));
        cJSON* root = cJSON_Parse(geo.c_str());
        cJSON* results = cJSON_GetObjectItem(root, "results");
        cJSON* place = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : nullptr;
        cJSON* lat = cJSON_GetObjectItem(place, "latitude");
        cJSON* lon = cJSON_GetObjectItem(place, "longitude");
        if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
            cJSON_Delete(root);
            return Error("Nao achei a cidade '" + city + "'.");
        }
        cJSON* name = cJSON_GetObjectItem(place, "name");
        cJSON* admin = cJSON_GetObjectItem(place, "admin1");
        std::string place_name = cJSON_IsString(name) ? name->valuestring : city;
        if (cJSON_IsString(admin)) {
            place_name += std::string(", ") + admin->valuestring;
        }
        char url[400];
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                 "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m"
                 "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
                 "sunrise,sunset"
                 "&timezone=America%%2FSao_Paulo&forecast_days=3",
                 lat->valuedouble, lon->valuedouble);
        cJSON_Delete(root);

        std::string body = HttpGet(url);
        root = cJSON_Parse(body.c_str());
        cJSON* current = cJSON_GetObjectItem(root, "current");
        cJSON* daily = cJSON_GetObjectItem(root, "daily");
        if (!cJSON_IsObject(current) || !cJSON_IsObject(daily)) {
            cJSON_Delete(root);
            return Error("Nao consegui a previsao do tempo agora.");
        }
        auto num = [](cJSON* obj, const char* key) {
            cJSON* v = cJSON_GetObjectItem(obj, key);
            return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
        };
        auto at = [](cJSON* obj, const char* key, int i) {
            cJSON* arr = cJSON_GetObjectItem(obj, key);
            cJSON* v = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, i) : nullptr;
            return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
        };

        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "local", place_name.c_str());
        cJSON* now = cJSON_AddObjectToObject(out, "agora");
        cJSON_AddNumberToObject(now, "temperatura_c", num(current, "temperature_2m"));
        cJSON_AddNumberToObject(now, "sensacao_c", num(current, "apparent_temperature"));
        cJSON_AddNumberToObject(now, "umidade_pct", num(current, "relative_humidity_2m"));
        cJSON_AddNumberToObject(now, "vento_kmh", num(current, "wind_speed_10m"));
        cJSON_AddStringToObject(now, "tempo", WeatherText(static_cast<int>(num(current, "weather_code"))));
        cJSON* days = cJSON_AddArrayToObject(out, "proximos_dias");
        const char* labels[] = {"hoje", "amanha", "depois de amanha"};
        for (int i = 0; i < 3; i++) {
            cJSON* d = cJSON_CreateObject();
            cJSON_AddStringToObject(d, "dia", labels[i]);
            cJSON_AddStringToObject(d, "tempo", WeatherText(static_cast<int>(at(daily, "weather_code", i))));
            cJSON_AddNumberToObject(d, "min_c", at(daily, "temperature_2m_min", i));
            cJSON_AddNumberToObject(d, "max_c", at(daily, "temperature_2m_max", i));
            cJSON_AddNumberToObject(d, "chance_chuva_pct", at(daily, "precipitation_probability_max", i));
            for (const auto& [key, label] : {std::pair{"sunrise", "nascer_do_sol"}, std::pair{"sunset", "por_do_sol"}}) {
                cJSON* arr = cJSON_GetObjectItem(daily, key);
                cJSON* v = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, i) : nullptr;
                if (cJSON_IsString(v) && strlen(v->valuestring) >= 16) {
                    cJSON_AddStringToObject(d, label, std::string(v->valuestring).substr(11, 5).c_str());
                }
            }
            cJSON_AddItemToArray(days, d);
        }
        cJSON_AddStringToObject(out, "fonte", "Open-Meteo");
        cJSON_Delete(root);
        return ToJson(out);
    }

    // ---------- notícias (RSS) ----------

    static std::string DecodeEntities(std::string s) {
        const std::pair<const char*, const char*> entities[] = {
            {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}};
        for (const auto& [from, to] : entities) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, strlen(from), to);
                pos += strlen(to);
            }
        }
        if (s.rfind("<![CDATA[", 0) == 0 && s.size() > 12) {
            s = s.substr(9, s.size() - 12);
        }
        return s;
    }

    // Títulos dos primeiros <item> de um RSS (sem baixar mais do que o necessário de memória).
    static std::vector<std::string> RssTitles(const std::string& xml, size_t max_items) {
        std::vector<std::string> titles;
        size_t pos = 0;
        while (titles.size() < max_items) {
            size_t item = xml.find("<item", pos);
            if (item == std::string::npos) break;
            size_t start = xml.find("<title>", item);
            size_t end = xml.find("</title>", start);
            if (start == std::string::npos || end == std::string::npos) break;
            std::string title = DecodeEntities(xml.substr(start + 7, end - start - 7));
            titles.push_back(Truncate(title, 160));
            pos = end;
        }
        return titles;
    }

    std::string News(const std::string& topic) {
        std::string url = topic.empty()
            ? "https://news.google.com/rss?hl=pt-BR&gl=BR&ceid=BR:pt-419"
            : "https://news.google.com/rss/search?hl=pt-BR&gl=BR&ceid=BR:pt-419&q=" + UrlEncode(topic);
        std::string source = "Google Noticias";
        std::string xml = HttpGet(url);
        if (xml.empty() && topic.empty()) {
            xml = HttpGet("https://g1.globo.com/rss/g1/");
            source = "G1";
        }
        auto titles = RssTitles(xml, 6);
        if (titles.empty()) {
            return Error("Nao consegui buscar noticias agora.");
        }
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        if (!topic.empty()) {
            cJSON_AddStringToObject(out, "tema", topic.c_str());
        }
        cJSON* list = cJSON_AddArrayToObject(out, "manchetes");
        for (const auto& t : titles) {
            cJSON_AddItemToArray(list, cJSON_CreateString(t.c_str()));
        }
        cJSON_AddStringToObject(out, "fonte", source.c_str());
        return ToJson(out);
    }

    // ---------- pacote Astral ----------

    // minúsculas e sem acento (só os acentos do português), para comparar nomes
    static std::string Normalize(std::string s) {
        for (auto& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const std::pair<const char*, const char*> accents[] = {
            {"á", "a"}, {"à", "a"}, {"â", "a"}, {"ã", "a"}, {"é", "e"}, {"ê", "e"}, {"í", "i"},
            {"ó", "o"}, {"ô", "o"}, {"õ", "o"}, {"ú", "u"}, {"ç", "c"},
            {"Á", "a"}, {"É", "e"}, {"Í", "i"}, {"Ó", "o"}, {"Ú", "u"}, {"Ç", "c"}};
        for (const auto& [from, to] : accents) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, strlen(from), to);
            }
        }
        return s;
    }

    std::string Horoscope(const std::string& sign) {
        const std::pair<const char*, const char*> signs[] = {
            {"aries", "aries"}, {"touro", "taurus"}, {"gemeos", "gemini"}, {"cancer", "cancer"},
            {"leao", "leo"}, {"virgem", "virgo"}, {"libra", "libra"}, {"escorpiao", "scorpio"},
            {"sagitario", "sagittarius"}, {"capricornio", "capricorn"}, {"aquario", "aquarius"},
            {"peixes", "pisces"}};
        std::string wanted = Normalize(sign);
        std::string english;
        for (const auto& [pt, en] : signs) {
            if (wanted.find(pt) != std::string::npos || wanted.find(en) != std::string::npos) {
                english = en;
                break;
            }
        }
        if (english.empty()) {
            return Error("Signo nao reconhecido: '" + sign + "'.");
        }
        std::string body = HttpGet("https://ohmanda.com/api/horoscope/" + english + "/");
        cJSON* root = cJSON_Parse(body.c_str());
        cJSON* text = cJSON_GetObjectItem(root, "horoscope");
        if (!cJSON_IsString(text)) {
            cJSON_Delete(root);
            return Error("Nao consegui o horoscopo agora.");
        }
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "signo", sign.c_str());
        cJSON_AddStringToObject(out, "horoscopo_em_ingles", Truncate(text->valuestring, 700).c_str());
        cJSON_AddStringToObject(out, "instrucao", "Traduza e conte em portugues, de forma curta e divertida.");
        cJSON_Delete(root);
        return ToJson(out);
    }

    // Fase da lua calculada na própria placa (sem internet): idade da lua desde uma lua nova conhecida.
    std::string Moon() {
        const double kSynodic = 29.530588853;
        const double kNewMoon2000 = 947182440.0;  // 06/01/2000 18:14 UTC
        time_t now = time(nullptr);
        double age = std::fmod((static_cast<double>(now) - kNewMoon2000) / 86400.0, kSynodic);
        if (age < 0) {
            age += kSynodic;
        }
        double illumination = (1 - std::cos(2 * M_PI * age / kSynodic)) / 2 * 100;
        const char* phase = age < 1.85    ? "lua nova"
                            : age < 5.54  ? "lua crescente"
                            : age < 9.23  ? "quarto crescente"
                            : age < 12.92 ? "crescente gibosa (quase cheia)"
                            : age < 16.61 ? "lua cheia"
                            : age < 20.30 ? "minguante gibosa"
                            : age < 23.99 ? "quarto minguante"
                            : age < 27.68 ? "lua minguante"
                                          : "lua nova";
        auto date_in = [now](double days) {
            time_t t = now + static_cast<time_t>(days * 86400);
            struct tm tm_local;
            localtime_r(&t, &tm_local);
            char buf[16];
            strftime(buf, sizeof(buf), "%d/%m", &tm_local);
            return std::string(buf);
        };
        double to_full = std::fmod(kSynodic / 2 - age + kSynodic, kSynodic);
        double to_new = kSynodic - age;
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "fase", phase);
        cJSON_AddNumberToObject(out, "iluminacao_pct", static_cast<int>(illumination + 0.5));
        cJSON_AddNumberToObject(out, "idade_dias", static_cast<int>(age * 10) / 10.0);
        cJSON_AddStringToObject(out, "proxima_cheia", date_in(to_full).c_str());
        cJSON_AddStringToObject(out, "proxima_nova", date_in(to_new).c_str());
        return ToJson(out);
    }

    std::string Currency() {
        std::string body = HttpGet("https://economia.awesomeapi.com.br/json/last/USD-BRL,EUR-BRL,BTC-BRL");
        cJSON* root = cJSON_Parse(body.c_str());
        if (!cJSON_IsObject(root)) {
            cJSON_Delete(root);
            return Error("Nao consegui as cotacoes agora.");
        }
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        const std::pair<const char*, const char*> pairs[] = {
            {"USDBRL", "dolar"}, {"EURBRL", "euro"}, {"BTCBRL", "bitcoin"}};
        for (const auto& [key, label] : pairs) {
            cJSON* item = cJSON_GetObjectItem(root, key);
            cJSON* bid = cJSON_GetObjectItem(item, "bid");
            cJSON* pct = cJSON_GetObjectItem(item, "pctChange");
            if (!cJSON_IsString(bid)) {
                continue;
            }
            cJSON* c = cJSON_AddObjectToObject(out, label);
            cJSON_AddNumberToObject(c, "reais", atof(bid->valuestring));
            if (cJSON_IsString(pct)) {
                cJSON_AddNumberToObject(c, "variacao_dia_pct", atof(pct->valuestring));
            }
        }
        cJSON_AddStringToObject(out, "fonte", "AwesomeAPI");
        cJSON_Delete(root);
        return ToJson(out);
    }

    std::string Holidays() {
        time_t now = time(nullptr);
        struct tm tm_local;
        localtime_r(&now, &tm_local);
        char today[16];
        strftime(today, sizeof(today), "%Y-%m-%d", &tm_local);
        int year = tm_local.tm_year + 1900;
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "hoje", today);
        cJSON* list = cJSON_AddArrayToObject(out, "proximos_feriados");
        int found = 0;
        for (int y = year; y <= year + 1 && found < 3; y++) {
            std::string body = HttpGet("https://brasilapi.com.br/api/feriados/v1/" + std::to_string(y));
            cJSON* root = cJSON_Parse(body.c_str());
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, root) {
                cJSON* date = cJSON_GetObjectItem(item, "date");
                cJSON* name = cJSON_GetObjectItem(item, "name");
                if (!cJSON_IsString(date) || !cJSON_IsString(name) || strcmp(date->valuestring, today) < 0) {
                    continue;
                }
                cJSON* h = cJSON_CreateObject();
                cJSON_AddStringToObject(h, "data", date->valuestring);
                cJSON_AddStringToObject(h, "nome", name->valuestring);
                cJSON* weekday = cJSON_GetObjectItem(item, "weekday");
                if (cJSON_IsString(weekday)) {
                    cJSON_AddStringToObject(h, "dia_da_semana", weekday->valuestring);
                }
                cJSON_AddItemToArray(list, h);
                if (++found >= 3) {
                    break;
                }
            }
            cJSON_Delete(root);
        }
        if (found == 0) {
            cJSON_Delete(out);
            return Error("Nao consegui os feriados agora.");
        }
        cJSON_AddStringToObject(out, "fonte", "BrasilAPI (feriados nacionais)");
        return ToJson(out);
    }

    std::string Recipe() {
        std::string body = HttpGet("https://www.themealdb.com/api/json/v1/1/random.php");
        cJSON* root = cJSON_Parse(body.c_str());
        cJSON* meals = cJSON_GetObjectItem(root, "meals");
        cJSON* meal = cJSON_IsArray(meals) ? cJSON_GetArrayItem(meals, 0) : nullptr;
        if (meal == nullptr) {
            cJSON_Delete(root);
            return Error("Nao consegui uma receita agora.");
        }
        auto str = [meal](const std::string& key) {
            cJSON* v = cJSON_GetObjectItem(meal, key.c_str());
            return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
        };
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "prato", str("strMeal").c_str());
        cJSON_AddStringToObject(out, "origem", str("strArea").c_str());
        cJSON* ingredients = cJSON_AddArrayToObject(out, "ingredientes");
        for (int i = 1; i <= 10; i++) {
            std::string name = str("strIngredient" + std::to_string(i));
            if (name.empty()) {
                break;
            }
            std::string measure = str("strMeasure" + std::to_string(i));
            cJSON_AddItemToArray(ingredients, cJSON_CreateString((measure + " " + name).c_str()));
        }
        cJSON_AddStringToObject(out, "modo_de_preparo", Truncate(str("strInstructions"), 600).c_str());
        cJSON_AddStringToObject(out, "instrucao", "Esta em ingles: traduza e conte em portugues de forma resumida.");
        cJSON_Delete(root);
        return ToJson(out);
    }

    std::string Fact() {
        std::string body = HttpGet("https://uselessfacts.jsph.pl/api/v2/facts/random?language=en");
        cJSON* root = cJSON_Parse(body.c_str());
        cJSON* text = cJSON_GetObjectItem(root, "text");
        if (!cJSON_IsString(text)) {
            cJSON_Delete(root);
            return Error("Nao consegui uma curiosidade agora.");
        }
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "curiosidade_em_ingles", text->valuestring);
        cJSON_AddStringToObject(out, "instrucao", "Traduza e conte em portugues, de um jeito divertido.");
        cJSON_Delete(root);
        return ToJson(out);
    }

    // ---------- ferramentas ----------

    void RegisterTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.web.wikipedia",
                    "Busca um assunto na Wikipédia em português e devolve o resumo do artigo. Use para "
                    "perguntas de conhecimento: pessoas, lugares, história, ciência, definições. Resuma em "
                    "poucas frases ao responder.",
                    PropertyList({Property("assunto", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        return Wikipedia(p["assunto"].value<std::string>());
                    });
        mcp.AddTool("self.web.weather",
                    "Previsão do tempo de uma cidade: tempo agora (temperatura, sensação, umidade, vento) e "
                    "dos próximos 3 dias (mín/máx, chance de chuva, nascer e pôr do sol). Use também para "
                    "'que horas o sol nasce/se põe'. Se o dono não disser a cidade, pergunte.",
                    PropertyList({Property("cidade", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        return Weather(p["cidade"].value<std::string>());
                    });
        mcp.AddTool("self.web.news",
                    "Manchetes de notícias de hoje do Brasil (Google Notícias). 'tema' é opcional: deixe vazio "
                    "para as principais ou informe um assunto (ex.: futebol, tecnologia, economia). Leia só "
                    "as mais interessantes, de forma curta.",
                    PropertyList({Property("tema", kPropertyTypeString, std::string(""))}),
                    [this](const PropertyList& p) -> ReturnValue {
                        return News(p["tema"].value<std::string>());
                    });
        // Agrupadas com 'tipo': o servidor aceita no máximo 32 ferramentas no total.
        mcp.AddTool("self.astro",
                    "Coisas astrais. tipo='horoscopo' com 'signo' (ex.: áries, touro, leão): horóscopo do dia, "
                    "vem em inglês, traduza e conte curto e divertido. tipo='lua': fase da lua de hoje (% "
                    "iluminada, próxima cheia e nova), calculada na placa, funciona sem internet.",
                    PropertyList({Property("tipo", kPropertyTypeString),
                                  Property("signo", kPropertyTypeString, std::string(""))}),
                    [this](const PropertyList& p) -> ReturnValue {
                        if (Normalize(p["tipo"].value<std::string>()) == "lua") {
                            return Moon();
                        }
                        return Horoscope(p["signo"].value<std::string>());
                    });
        mcp.AddTool("self.web.info",
                    "Informações rápidas da internet. tipo='cotacao': dólar, euro e bitcoin em reais. "
                    "tipo='feriados': próximos feriados nacionais do Brasil. tipo='receita': uma receita "
                    "aleatória do mundo (em inglês: traduza e resuma). tipo='curiosidade': um fato curioso "
                    "aleatório (em inglês: traduza e conte).",
                    PropertyList({Property("tipo", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        std::string kind = Normalize(p["tipo"].value<std::string>());
                        if (kind == "cotacao") return Currency();
                        if (kind == "feriados") return Holidays();
                        if (kind == "receita") return Recipe();
                        if (kind == "curiosidade") return Fact();
                        return Error("tipo invalido: use cotacao, feriados, receita ou curiosidade");
                    });
    }
};

#endif  // _ASTRAL_WEB_H_
