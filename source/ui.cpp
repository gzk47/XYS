#include "ui.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "i18n.hpp"

namespace ui {
namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
TTF_Font* g_fonts[3] = {nullptr, nullptr, nullptr};  // 18 / 22 / 30

std::map<std::string, SDL_Texture*> g_textCache;
std::map<u64, SDL_Texture*> g_iconCache;
SDL_Texture* g_aboutIcon = nullptr;
std::string g_aboutIconPath;

const SDL_Color kBg = {244, 246, 250, 255};
const SDL_Color kPanel = {255, 255, 255, 255};
const SDL_Color kSelected = {211, 227, 252, 255};
const SDL_Color kAccent = {37, 99, 235, 255};
const SDL_Color kText = {17, 24, 39, 255};
const SDL_Color kSub = {107, 114, 128, 255};
const SDL_Color kRed = {220, 38, 38, 255};
const SDL_Color kLine = {226, 232, 240, 255};
const SDL_Color kPlaceholder = {229, 231, 235, 255};

void fill(int x, int y, int w, int h, SDL_Color color) {
    SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(g_renderer, &rect);
}

void outline(int x, int y, int w, int h, SDL_Color color) {
    fill(x, y, w, 1, color);
    fill(x, y + h - 1, w, 1, color);
    fill(x, y, 1, h, color);
    fill(x + w - 1, y, 1, h, color);
}

SDL_Texture* makeText(const std::string& text, int fontIndex, SDL_Color color) {
    if (text.empty()) return nullptr;
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "%d:%u,%u,%u:", fontIndex, color.r, color.g,
             color.b);
    const std::string key = std::string(prefix) + text;

    auto found = g_textCache.find(key);
    if (found != g_textCache.end()) return found->second;

    SDL_Surface* surface =
        TTF_RenderUTF8_Blended(g_fonts[fontIndex], text.c_str(), color);
    if (!surface) return nullptr;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(g_renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture) return nullptr;

    if (g_textCache.size() > 300) {
        for (auto& item : g_textCache) SDL_DestroyTexture(item.second);
        g_textCache.clear();
    }
    g_textCache[key] = texture;
    return texture;
}

int textWidth(const std::string& text, int fontIndex) {
    int width = 0;
    int height = 0;
    if (!text.empty()) TTF_SizeUTF8(g_fonts[fontIndex], text.c_str(), &width, &height);
    return width;
}

void drawText(const std::string& text, int x, int y, int fontIndex, SDL_Color color) {
    SDL_Texture* texture = makeText(text, fontIndex, color);
    if (!texture) return;
    int width = 0;
    int height = 0;
    SDL_QueryTexture(texture, nullptr, nullptr, &width, &height);
    SDL_Rect dst = {x, y, width, height};
    SDL_RenderCopy(g_renderer, texture, nullptr, &dst);
}

void drawTextRight(const std::string& text, int right, int y, int fontIndex,
                   SDL_Color color) {
    drawText(text, right - textWidth(text, fontIndex), y, fontIndex, color);
}

void drawTextCentered(const std::string& text, int centerX, int y, int fontIndex,
                      SDL_Color color) {
    drawText(text, centerX - textWidth(text, fontIndex) / 2, y, fontIndex, color);
}

std::string fitText(const std::string& text, int fontIndex, int maxWidth) {
    if (maxWidth <= 0 || text.empty()) return "";
    if (textWidth(text, fontIndex) <= maxWidth) return text;

    const int ellipsisWidth = textWidth("…", fontIndex);
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char lead = static_cast<unsigned char>(text[i]);
        size_t length = 1;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
        }
        if (i + length > text.size()) break;

        const std::string candidate = out + text.substr(i, length);
        if (textWidth(candidate, fontIndex) + ellipsisWidth > maxWidth) break;
        out = candidate;
        i += length;
    }
    return out + "…";
}

void drawTextWrapped(const std::string& text, int x, int y, int maxWidth,
                     int fontIndex, SDL_Color color, int lineHeight) {
    // Split into words. ASCII words travel together so English breaks between
    // words, while CJK glyphs stay individual and join without spaces.
    std::vector<std::string> words;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\n') {
            words.push_back("\n");
            ++i;
            continue;
        }
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            size_t end = i;
            while (end < text.size() && text[end] != ' ' && text[end] != '\n') {
                ++end;
            }
            words.push_back(text.substr(i, end - i));
            i = end;
            continue;
        }

        size_t length = 1;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
        }
        if (i + length > text.size()) break;
        words.push_back(text.substr(i, length));
        i += length;
    }

    std::string line;
    for (const std::string& word : words) {
        if (word == "\n") {
            if (!line.empty()) drawText(line, x, y, fontIndex, color);
            y += lineHeight;
            line.clear();
            continue;
        }
        const bool wordAscii = static_cast<unsigned char>(word[0]) < 0x80;
        const bool lineAscii =
            !line.empty() && static_cast<unsigned char>(line.back()) < 0x80;
        const std::string next = line + (wordAscii && lineAscii ? " " : "") + word;
        if (!line.empty() && textWidth(next, fontIndex) > maxWidth) {
            drawText(line, x, y, fontIndex, color);
            y += lineHeight;
            line = word;
        } else {
            line = next;
        }
    }
    if (!line.empty()) drawText(line, x, y, fontIndex, color);
}

SDL_Texture* iconFor(const Row& row) {
    auto found = g_iconCache.find(row.appId);
    if (found != g_iconCache.end()) return found->second;

    SDL_Texture* texture = nullptr;
    if (!row.iconPath.empty()) {
        SDL_Surface* surface = IMG_Load(row.iconPath.c_str());
        if (surface) {
            texture = SDL_CreateTextureFromSurface(g_renderer, surface);
            SDL_FreeSurface(surface);
        }
    }
    if (!texture) {
        std::vector<unsigned char> bytes;
        if (saves::loadIcon(row.appId, bytes)) {
            SDL_RWops* ops =
                SDL_RWFromConstMem(bytes.data(), static_cast<int>(bytes.size()));
            if (ops) {
                SDL_Surface* surface = IMG_Load_RW(ops, 1);
                if (surface) {
                    texture = SDL_CreateTextureFromSurface(g_renderer, surface);
                    SDL_FreeSurface(surface);
                }
            }
        }
    }
    g_iconCache[row.appId] = texture;
    return texture;
}

SDL_Texture* fileTexture(const std::string& path) {
    if (path.empty()) return nullptr;
    if (path == g_aboutIconPath) return g_aboutIcon;

    if (g_aboutIcon) SDL_DestroyTexture(g_aboutIcon);
    g_aboutIcon = nullptr;
    g_aboutIconPath = path;

    SDL_Surface* surface = IMG_Load(path.c_str());
    if (surface) {
        g_aboutIcon = SDL_CreateTextureFromSurface(g_renderer, surface);
        SDL_FreeSurface(surface);
    }
    return g_aboutIcon;
}

void drawIcon(SDL_Texture* texture, int x, int y, int size,
              const std::string& title) {
    if (texture) {
        SDL_Rect dst = {x, y, size, size};
        SDL_RenderCopy(g_renderer, texture, nullptr, &dst);
        return;
    }

    fill(x, y, size, size, kPlaceholder);
    std::string letter = "?";
    for (unsigned char c : title) {
        if (c >= 'A' && c <= 'Z') {
            letter.assign(1, static_cast<char>(c));
            break;
        }
        if (c >= 'a' && c <= 'z') {
            letter.assign(1, static_cast<char>(c - 32));
            break;
        }
    }
    int width = 0;
    int height = 0;
    TTF_SizeUTF8(g_fonts[2], letter.c_str(), &width, &height);
    drawText(letter, x + (size - width) / 2, y + (size - height) / 2, 2, kSub);
}

}  // namespace

bool init(const char* fontPath) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    g_window = SDL_CreateWindow("XYS - XinYue Save", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, kWidth, kHeight,
                                SDL_WINDOW_SHOWN);
    if (!g_window) return false;

    g_renderer = SDL_CreateRenderer(
        g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) {
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_renderer) return false;
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);

    if (TTF_Init() != 0) return false;
    g_fonts[0] = TTF_OpenFont(fontPath, 20);
    g_fonts[1] = TTF_OpenFont(fontPath, 24);
    g_fonts[2] = TTF_OpenFont(fontPath, 32);
    if (!g_fonts[0] || !g_fonts[1] || !g_fonts[2]) return false;

    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
    SDL_ShowCursor(SDL_DISABLE);
    return true;
}

void shutdown() {
    for (auto& item : g_textCache) SDL_DestroyTexture(item.second);
    g_textCache.clear();
    for (auto& item : g_iconCache) {
        if (item.second) SDL_DestroyTexture(item.second);
    }
    g_iconCache.clear();

    if (g_aboutIcon) SDL_DestroyTexture(g_aboutIcon);
    g_aboutIcon = nullptr;
    g_aboutIconPath.clear();

    for (auto& font : g_fonts) {
        if (font) TTF_CloseFont(font);
        font = nullptr;
    }
    TTF_Quit();
    IMG_Quit();

    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    g_renderer = nullptr;
    g_window = nullptr;
    SDL_Quit();
}

void invalidateIcons() {
    for (auto& item : g_iconCache) {
        if (item.second) SDL_DestroyTexture(item.second);
    }
    g_iconCache.clear();
}

void draw(const State& state) {
    const Strings& text = i18n::str();

    SDL_SetRenderDrawColor(g_renderer, kBg.r, kBg.g, kBg.b, 255);
    SDL_RenderClear(g_renderer);

    // Header.
    fill(0, 0, kWidth, 88, kPanel);
    fill(0, 87, kWidth, 1, kLine);
    // Sit the subtitle on the same bottom line as the XYS wordmark even
    // though both use different font sizes.
    const int titleH = TTF_FontHeight(g_fonts[2]);
    const int subtitleH = TTF_FontHeight(g_fonts[1]);
    drawText("XYS", 32, 16, 2, kText);
    drawText(fitText(text.subtitle, 1, 460), 210, 16 + titleH - subtitleH, 1,
             kSub);
    drawTextRight(state.cloud ? text.cloudOn : text.cloudOff, 1248, 22, 0,
                  state.cloud ? kAccent : kSub);
    drawTextRight(state.sdOk ? text.sdOk : text.sdMissing, 1248, 50, 0,
                  state.sdOk ? kSub : kRed);

    // Save list.
    fill(16, 104, 776, 528, kPanel);
    outline(16, 104, 776, 528, kLine);

    const int rowHeight = 88;
    const int visibleRows = 6;
    int first = state.cursor - visibleRows / 2;
    if (first < 0) first = 0;
    const int maxFirst = static_cast<int>(state.rows.size()) - visibleRows;
    if (first > maxFirst) first = maxFirst;
    if (first < 0) first = 0;

    if (state.rows.empty()) {
        drawTextCentered(text.noSaves, 404, 330, 1, kSub);
    } else {
        for (int i = 0; i < visibleRows; ++i) {
            const int index = first + i;
            if (index >= static_cast<int>(state.rows.size())) break;

            const Row& row = state.rows[index];
            const int y = 108 + i * rowHeight;
            if (index == state.cursor) {
                fill(24, y, 760, 80, kSelected);
                fill(24, y, 4, 80, kAccent);
            }

            drawIcon(iconFor(row), 44, y + 8, 64, row.title);
            drawText(fitText(row.title, 1, 390), 124, y + 8, 1, kText);

            char meta[64];
            snprintf(meta, sizeof(meta), "%s · %.1f MB", row.user.c_str(),
                     row.sizeMb);
            drawText(fitText(meta, 0, 390), 124, y + 44, 0, kSub);

            if (row.backups > 0) {
                drawTextRight(i18n::format(text.backupsFmt, row.backups), 760,
                              y + 10, 0, kAccent);
                drawTextRight(fitText(row.latest, 0, 230), 760, y + 40, 0, kSub);
            } else {
                drawTextRight(text.noBackup, 760, y + 10, 0, kSub);
            }
        }
    }

    // Preview panel.
    fill(808, 104, 456, 528, kPanel);
    outline(808, 104, 456, 528, kLine);

    if (state.rows.empty()) {
        // The list panel already shows the empty state.
    } else {
        const Row& row = state.rows[state.cursor];
        drawIcon(iconFor(row), 908, 128, 256, row.title);

        drawTextCentered(fitText(row.title, 1, 400), 1036, 404, 1, kText);

        if (row.backups > 0) {
            drawTextCentered(i18n::format(text.latestFmt, row.latest.c_str()),
                             1036, 444, 0, kSub);
        } else {
            drawTextCentered(text.noBackup, 1036, 444, 0, kSub);
        }
        drawTextCentered(i18n::format(text.backupsFmt, row.backups), 1036, 474, 0,
                         kAccent);
        drawTextCentered(
            i18n::format(text.titleIdFmt, saves::hexId(row.appId).c_str()), 1036,
            504, 0, kSub);
    }

    // Footer. Hidden while the picker owns the screen so its own hint is the
    // only key guide on display.
    if (!state.picking) {
        fill(16, 640, 1248, 76, kPanel);
        outline(16, 640, 1248, 76, kLine);
        // Centre both lines so the hint does not hug the top border.
        const int hintH = TTF_FontHeight(g_fonts[1]);
        const int hintY =
            640 + (76 - hintH - 8 - TTF_FontHeight(g_fonts[0])) / 2;
        drawText(fitText(text.hints, 1, 1200), 32, hintY, 1, kSub);
        drawText(fitText(state.status, 0, 1200), 32, hintY + hintH + 8, 0,
                 kAccent);
    }

    if (state.picking) {
        fill(0, 0, kWidth, kHeight, {15, 23, 42, 140});
        fill(260, 120, 760, 480, kPanel);
        outline(260, 120, 760, 480, kLine);
        drawTextCentered(fitText(state.pickTitle, 1, 680), 640, 136, 1, kText);
        drawTextCentered(text.pickTitle, 640, 168, 0, kSub);
        fill(260, 192, 760, 1, kLine);

        const int pickVisible = 6;
        const int pickHeight = 52;
        int first = state.pickCursor - pickVisible / 2;
        if (first < 0) first = 0;
        const int maxFirst = static_cast<int>(state.picks.size()) - pickVisible;
        if (first > maxFirst) first = maxFirst;
        if (first < 0) first = 0;

        if (state.picks.empty()) {
            drawTextCentered(text.pickEmpty, 640, 330, 1, kSub);
        } else {
            for (int i = 0; i < pickVisible; ++i) {
                const int index = first + i;
                if (index >= static_cast<int>(state.picks.size())) break;
                const PickRow& pick = state.picks[index];
                const int y = 204 + i * pickHeight;
                if (index == state.pickCursor) {
                    fill(276, y, 728, 44, kSelected);
                    fill(276, y, 4, 44, kAccent);
                }
                drawText(fitText(pick.time, 1, 540), 296, y + 9, 1, kText);
                drawTextRight(pick.source, 984, y + 12, 0, kSub);
            }
        }

        drawTextCentered(fitText(state.status, 0, 700), 640, 508, 0, kAccent);
        drawTextCentered(state.pickHint.empty() ? text.pickHint : state.pickHint,
                         640, 556, 0, kAccent);
    }

    if (state.screen == Screen::Menu) {
        fill(0, 0, kWidth, kHeight, {15, 23, 42, 150});
        fill(280, 110, 720, 500, kPanel);
        outline(280, 110, 720, 500, kLine);
        fill(280, 110, 4, 500, kAccent);
        drawText(text.menuTitle, 320, 140, 2, kText);
        fill(320, 206, 640, 1, kLine);

        const int itemHeight = 56;
        for (int i = 0; i < static_cast<int>(state.menuItems.size()); ++i) {
            const int y = 216 + i * itemHeight;
            if (i == state.menuCursor) {
                fill(304, y, 672, 48, kSelected);
                fill(304, y, 4, 48, kAccent);
            }
            drawText(fitText(state.menuItems[i], 1, 620), 332, y + 10, 1, kText);
        }

        fill(320, 512, 640, 1, kLine);
        drawTextCentered(text.menuHint, 640, 534, 0, kSub);
    } else if (state.screen == Screen::Settings) {
        fill(0, 0, kWidth, kHeight, kBg);
        fill(0, 0, kWidth, 88, kPanel);
        fill(0, 87, kWidth, 1, kLine);
        drawText(text.settingsTitle, 40, 24, 2, kText);
        drawTextRight(state.cloud ? text.cloudOn : text.cloudOff, 1240, 33, 0,
                      state.cloud ? kAccent : kSub);

        fill(160, 112, 960, 520, kPanel);
        outline(160, 112, 960, 520, kLine);

        const int fieldHeight = 70;
        const int fieldTop = 144;
        const int fieldX = 336;
        const int fieldRight = 1088;
        drawText(text.settingsCloudGroup, 200, fieldTop, 0, kAccent);
        drawText(text.settingsLocalGroup, 200, fieldTop + 3 * fieldHeight, 0,
                 kAccent);
        fill(304, fieldTop, 1, 7 * fieldHeight - 8, kLine);

        for (int i = 0; i < static_cast<int>(state.fields.size()); ++i) {
            const FieldRow& field = state.fields[i];
            const int y = fieldTop + i * fieldHeight;
            if (i == state.fieldCursor) {
                fill(320, y - 4, 784, 66, kSelected);
                fill(320, y - 4, 4, 66, kAccent);
            }
            drawText(field.label, fieldX, y, 0, kSub);

            std::string example;
            if (i == state.fieldCursor && !state.fieldExample.empty()) {
                const int labelWidth = textWidth(field.label, 0);
                const int maxWidth = fieldRight - fieldX - labelWidth - 32;
                example = fitText(state.fieldExample, 0, maxWidth);
                const int exampleY =
                    y + (fieldHeight - TTF_FontHeight(g_fonts[0])) / 2;
                drawTextRight(example, fieldRight, exampleY, 0, kSub);
            }

            std::string value = field.value;
            SDL_Color color = kText;
            int valueFont = 1;
            int valueY = y + 28;
            if (value.empty()) {
                value = field.emptyValue.empty() ? text.notSet : field.emptyValue;
                color = kSub;
                // Placeholders are hints, so keep them one step smaller than
                // the values the user actually typed.
                valueFont = 0;
                valueY = y + 32;
            } else if (field.secret) {
                value.assign(value.size() > 40 ? 40 : value.size(), '*');
            }
            int valueWidth = fieldRight - fieldX;
            if (!example.empty()) {
                valueWidth -= textWidth(example, 0) + 24;
            }
            drawText(fitText(value, valueFont, valueWidth), fieldX, valueY,
                     valueFont, color);
        }

        fill(16, 640, 1248, 76, kPanel);
        outline(16, 640, 1248, 76, kLine);
        const int hintH = TTF_FontHeight(g_fonts[1]);
        const int hintY =
            640 + (76 - hintH - 8 - TTF_FontHeight(g_fonts[0])) / 2;
        drawText(text.settingsHint, 32, hintY, 1, kSub);
        drawText(fitText(state.status, 0, 1200), 32, hintY + hintH + 8, 0,
                 kAccent);
    } else if (state.screen == Screen::About) {
        fill(0, 0, kWidth, kHeight, kBg);
        fill(0, 0, kWidth, 88, kPanel);
        fill(0, 87, kWidth, 1, kLine);
        drawText(text.aboutTitle, 40, 24, 2, kText);

        fill(160, 112, 960, 528, kPanel);
        outline(160, 112, 960, 528, kLine);

        const int iconSize = 160;
        const int iconX = 200;
        const int iconY = 144;
        SDL_Texture* icon = fileTexture(state.aboutIconPath);
        if (icon) {
            SDL_Rect dst = {iconX, iconY, iconSize, iconSize};
            SDL_RenderCopy(g_renderer, icon, nullptr, &dst);
        } else {
            fill(iconX, iconY, iconSize, iconSize, kPlaceholder);
            drawTextCentered("XYS", iconX + iconSize / 2, iconY + iconSize / 2 - 15,
                             2, kSub);
        }

        const int infoX = iconX + iconSize + 32;
        drawText("XYS", infoX, iconY + 14, 2, kText);
        drawText(i18n::format(text.aboutVersionFmt, state.version.c_str()),
                 infoX + 2, iconY + 64, 0, kText);
        drawText(i18n::format(text.aboutAuthorFmt, state.author.c_str()),
                 infoX + 2, iconY + 92, 0, kText);
        drawText(text.aboutRepo, infoX + 2, iconY + 120, 0, kAccent);

        int y = 348;
        drawText(fitText(text.aboutIntro, 1, 860), 200, y, 1, kText);
        y += 46;
        drawText(fitText(text.aboutIntro2, 0, 860), 200, y, 0, kSub);
        y += 36;
        drawText(fitText(text.aboutLibs, 0, 860), 200, y, 0, kSub);

        fill(192, 508, 896, 1, kLine);
        drawTextCentered(text.aboutHint, 640, 566, 0, kSub);
    }

    if (state.prompt) {
        fill(0, 0, kWidth, kHeight, {15, 23, 42, 160});
        fill(320, 236, 640, 248, kPanel);
        outline(320, 236, 640, 248, kLine);
        fill(320, 236, 640, 4, kAccent);
        drawTextCentered(state.promptTitle, 640, 262, 2, kText);
        drawTextWrapped(state.promptMessage, 368, 314, 544, 0, kSub, 28);
        drawTextCentered(state.promptHint, 640, 436, 0, kAccent);
    }

    if (state.progress >= 0) {
        fill(0, 0, kWidth, kHeight, {15, 23, 42, 170});
        fill(340, 268, 600, 200, kPanel);
        outline(340, 268, 600, 200, kLine);
        drawTextCentered(state.progressTitle, 640, 290, 1, kText);

        if (!state.progressStatus.empty()) {
            drawTextCentered(fitText(state.progressStatus, 0, 540), 640, 324, 0,
                             kSub);
        }

        const int barX = 390;
        const int barY = 360;
        const int barWidth = 500;
        const int barHeight = 20;
        int percent = state.progress;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        fill(barX, barY, barWidth, barHeight, kPlaceholder);
        if (percent > 0) {
            fill(barX, barY, barWidth * percent / 100, barHeight, kAccent);
        }

        char label[16];
        snprintf(label, sizeof(label), "%d%%", percent);
        drawTextCentered(label, 640, 388, 0, kSub);
        const std::string hint =
            state.progressHint.empty() ? text.progressHint : state.progressHint;
        drawTextCentered(hint, 640, 424, 0, kSub);
    }

    SDL_RenderPresent(g_renderer);
}

}  // namespace ui
