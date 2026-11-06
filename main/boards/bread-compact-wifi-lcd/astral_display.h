#ifndef _ASTRAL_DISPLAY_H_
#define _ASTRAL_DISPLAY_H_

#include "display/lcd_display.h"

#include <initializer_list>
#include <string>
#include <atomic>

LV_FONT_DECLARE(font_astral_digits_72);
LV_IMAGE_DECLARE(astral_icon_food);
LV_IMAGE_DECLARE(astral_icon_energy);
LV_IMAGE_DECLARE(astral_icon_mood);
LV_IMAGE_DECLARE(astral_icon_health);

// Astral Boy: o rosto ocupa a tela toda, então nada de faixas brancas por cima.
// Textos ficam escuros, direto sobre a pele branca, e a legenda ganha altura
// suficiente para a fonte atual (que pode ser trocada pelo pacote de assets).
// Também mostra um relógio grande no centro (timer e cronômetro).
class AstralLcdDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;

    // Relógio grande no centro: só dígitos e ":" (fonte Fredoka 72 px embutida).
    // Durante um minigame o relógio grande fica escondido (não cobre o jogo)
    void SetGameMode(bool on) {
        game_mode_ = on;
        if (on) {
            HideBigClock();
        }
    }

    void ShowBigClock(const std::string& text) {
        if (game_mode_.load()) {
            HideBigClock();
            return;
        }
        DisplayLockGuard lock(this);
        if (big_clock_ == nullptr) {
            big_clock_ = lv_obj_create(lv_screen_active());
            lv_obj_set_size(big_clock_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(big_clock_, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(big_clock_, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(big_clock_, 24, 0);
            lv_obj_set_style_border_width(big_clock_, 5, 0);
            lv_obj_set_style_border_color(big_clock_, lv_color_hex(0x1E90EB), 0);
            lv_obj_set_style_pad_hor(big_clock_, 12, 0);
            lv_obj_set_style_pad_ver(big_clock_, 2, 0);
            lv_obj_set_scrollbar_mode(big_clock_, LV_SCROLLBAR_MODE_OFF);
            lv_obj_remove_flag(big_clock_, LV_OBJ_FLAG_SCROLLABLE);
            big_clock_label_ = lv_label_create(big_clock_);
            lv_obj_set_style_text_font(big_clock_label_, &font_astral_digits_72, 0);
            lv_obj_set_style_text_color(big_clock_label_, lv_color_hex(0x1A1824), 0);
            lv_obj_center(big_clock_label_);
            lv_obj_align(big_clock_, LV_ALIGN_CENTER, 0, 8);
        }
        lv_label_set_text(big_clock_label_, text.c_str());
        lv_obj_remove_flag(big_clock_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(big_clock_);
    }

    void HideBigClock() {
        DisplayLockGuard lock(this);
        if (big_clock_ != nullptr) {
            lv_obj_add_flag(big_clock_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Balão de aviso no alto da tela (ex.: "desmaiei, me dá comida"). Texto vazio esconde.
    void SetBalloon(const std::string& text) {
        DisplayLockGuard lock(this);
        if (text.empty()) {
            if (balloon_ != nullptr) {
                lv_obj_add_flag(balloon_, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }
        if (balloon_ == nullptr) {
            // Uma linha só, rolando, no mesmo lugar das falas (logo acima do painel): o rosto fica livre.
            balloon_ = lv_obj_create(lv_screen_active());
            const lv_font_t* font = lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN);
            lv_obj_set_size(balloon_, LV_HOR_RES - 8, font->line_height + 10);
            lv_obj_set_style_bg_color(balloon_, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(balloon_, LV_OPA_10, 0);  // quase transparente
            lv_obj_set_style_radius(balloon_, 10, 0);
            lv_obj_set_style_border_width(balloon_, 0, 0);
            lv_obj_set_style_pad_all(balloon_, 0, 0);
            lv_obj_set_scrollbar_mode(balloon_, LV_SCROLLBAR_MODE_OFF);
            lv_obj_remove_flag(balloon_, LV_OBJ_FLAG_SCROLLABLE);
            balloon_label_ = lv_label_create(balloon_);
            lv_obj_set_width(balloon_label_, lv_pct(96));
            lv_label_set_long_mode(balloon_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_style_text_align(balloon_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(balloon_label_, lv_color_hex(0xD12F3A), 0);
            lv_obj_center(balloon_label_);
            lv_obj_align(balloon_, LV_ALIGN_BOTTOM_MID, 0, -kPetPanelReserve);
        }
        std::string line = text;  // as quebras de linha viram separador (é uma linha só)
        for (size_t pos; (pos = line.find('\n')) != std::string::npos;) {
            line.replace(pos, 1, " - ");
        }
        lv_label_set_text(balloon_label_, line.c_str());
        lv_obj_remove_flag(balloon_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(balloon_);
    }

    // Painel do tamagotchi: 4 ícones com barrinhas (0-100) na parte de baixo da tela.
    // Valores na ordem: fome (100 = cheio), energia, humor, saúde.
    void SetPetPanel(const int values[4], bool visible) {
        DisplayLockGuard lock(this);
        if (!visible) {
            if (pet_panel_ != nullptr) {
                lv_obj_add_flag(pet_panel_, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }
        if (pet_panel_ == nullptr) {
            CreatePetPanel();
        }
        for (int i = 0; i < 4; i++) {
            int v = values[i] < 0 ? 0 : (values[i] > 100 ? 100 : values[i]);
            lv_obj_set_width(pet_fill_[i], kBarWidth * v / 100);
            uint32_t color = v >= 60 ? 0x2EAE4F : (v >= 30 ? 0xF2B90F : 0xE8323C);
            lv_obj_set_style_bg_color(pet_fill_[i], lv_color_hex(color), 0);
        }
        lv_obj_remove_flag(pet_panel_, LV_OBJ_FLAG_HIDDEN);
    }

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        ApplyAstralStyle();
    }

    void SetTheme(Theme* theme) override {
        SpiLcdDisplay::SetTheme(theme);
        ApplyAstralStyle();
    }

    // O servidor manda "% self.ferramenta..." quando a IA usa uma ferramenta: não mostrar na legenda.
    void SetChatMessage(const char* role, const char* content) override {
        if (content != nullptr && content[0] == '%') {
            return;
        }
        SpiLcdDisplay::SetChatMessage(role, content);
    }

private:
    static constexpr int kBarWidth = 30;
    static constexpr int kPetPanelReserve = 26;  // espaço do painel do tamagotchi embaixo da tela

    lv_obj_t* big_clock_ = nullptr;
    lv_obj_t* big_clock_label_ = nullptr;
    std::atomic<bool> game_mode_{false};
    lv_obj_t* balloon_ = nullptr;
    lv_obj_t* balloon_label_ = nullptr;
    lv_obj_t* pet_panel_ = nullptr;
    lv_obj_t* pet_fill_[4] = {};

    static lv_obj_t* PlainBox(lv_obj_t* parent) {
        lv_obj_t* obj = lv_obj_create(parent);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
        lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        return obj;
    }

    void CreatePetPanel() {
        const lv_image_dsc_t* icons[4] = {&astral_icon_food, &astral_icon_energy, &astral_icon_mood,
                                          &astral_icon_health};
        pet_panel_ = PlainBox(lv_screen_active());
        lv_obj_set_size(pet_panel_, LV_HOR_RES, 22);
        lv_obj_set_flex_flow(pet_panel_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pet_panel_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(pet_panel_, LV_ALIGN_BOTTOM_MID, 0, -3);
        for (int i = 0; i < 4; i++) {
            lv_obj_t* item = PlainBox(pet_panel_);
            lv_obj_set_size(item, 18 + 3 + kBarWidth + 2, 20);
            lv_obj_t* icon = lv_image_create(item);
            lv_image_set_src(icon, icons[i]);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_t* track = lv_obj_create(item);
            lv_obj_set_size(track, kBarWidth + 2, 9);
            lv_obj_set_style_pad_all(track, 1, 0);
            lv_obj_set_style_radius(track, 5, 0);
            lv_obj_set_style_bg_color(track, lv_color_hex(0xE4E2EA), 0);
            lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(track, 1, 0);
            lv_obj_set_style_border_color(track, lv_color_hex(0x1A1824), 0);
            lv_obj_set_scrollbar_mode(track, LV_SCROLLBAR_MODE_OFF);
            lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_align(track, LV_ALIGN_RIGHT_MID, 0, 0);
            pet_fill_[i] = lv_obj_create(track);
            lv_obj_set_size(pet_fill_[i], kBarWidth, 5);
            lv_obj_set_style_radius(pet_fill_[i], 3, 0);
            lv_obj_set_style_border_width(pet_fill_[i], 0, 0);
            lv_obj_set_style_bg_opa(pet_fill_[i], LV_OPA_COVER, 0);
            lv_obj_align(pet_fill_[i], LV_ALIGN_LEFT_MID, 0, 0);
        }
    }

    void ApplyAstralStyle() {
        DisplayLockGuard lock(this);
        if (top_bar_ == nullptr) {
            return;
        }
        const lv_color_t ink = lv_color_hex(0x1A1824);

        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        if (bottom_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
            const lv_font_t* font = lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN);
            lv_obj_set_height(bottom_bar_, font->line_height + 10);
            // fica logo acima do painel do tamagotchi (22 px + margem)
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -kPetPanelReserve);
        }
        for (lv_obj_t* label : {network_label_, status_label_, notification_label_, mute_label_,
                                battery_label_, chat_message_label_}) {
            if (label != nullptr) {
                lv_obj_set_style_text_color(label, ink, 0);
            }
        }
    }
};

#endif  // _ASTRAL_DISPLAY_H_
