#include "ui.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <switch.h>

#include <algorithm>
#include <cmath>
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
TTF_Font* g_fonts[4] = {nullptr, nullptr, nullptr, nullptr};  // 20 / 24 / 32 / 22

std::map<std::string, SDL_Texture*> g_textCache;
std::map<u64, SDL_Texture*> g_iconCache;
SDL_Texture* g_aboutIcon = nullptr;
std::string g_aboutIconPath;
std::string g_lastError;

bool fail(const char* reason) {
    g_lastError = reason;
    return false;
}

// Fonts come from the console's shared font service instead of the ROM.
PlFontData g_sharedFont = {};
bool g_plReady = false;

const SDL_Color kBg = {241, 244, 249, 255};
const SDL_Color kPanel = {255, 255, 255, 255};
const SDL_Color kSelected = {235, 243, 255, 255};
const SDL_Color kSelectedLine = {205, 223, 252, 255};
const SDL_Color kPillFill = {238, 245, 255, 255};
const SDL_Color kPillLine = {214, 229, 253, 255};
const SDL_Color kAccent = {37, 99, 235, 255};
const SDL_Color kText = {15, 23, 42, 255};
const SDL_Color kSub = {100, 116, 139, 255};
const SDL_Color kMuted = {148, 163, 184, 255};
const SDL_Color kRed = {220, 38, 38, 255};
const SDL_Color kGreen = {16, 185, 129, 255};
const SDL_Color kLine = {226, 232, 240, 255};
const SDL_Color kHair = {238, 242, 247, 255};
const SDL_Color kPlaceholder = {229, 231, 235, 255};
const SDL_Color kTrackOff = {233, 238, 245, 255};
const SDL_Color kTrackOffLine = {219, 227, 238, 255};
const SDL_Color kKeyEdge = {209, 217, 228, 255};
const SDL_Color kKeyShadow = {226, 232, 240, 255};
const SDL_Color kScrim = {15, 23, 42, 150};

void fill(int x, int y, int w, int h, SDL_Color color) {
    SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(g_renderer, &rect);
}

// Cache of anti-aliased rounded-rectangle textures, keyed by geometry and colour.
std::map<std::string, SDL_Texture*> g_shapeCache;

// Signed distance to the rounded-rectangle edge in pixels; negative inside.
double roundedDistance(double px, double py, double w, double h, double r) {
    const double qx = std::abs(px - w / 2) - (w / 2 - r);
    const double qy = std::abs(py - h / 2) - (h / 2 - r);
    const double ax = std::max(qx, 0.0);
    const double ay = std::max(qy, 0.0);
    return std::sqrt(ax * ax + ay * ay) + std::min(std::max(qx, qy), 0.0) - r;
}

SDL_Texture* roundedTexture(int w, int h, int r, SDL_Color color) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%d,%d,%d,%u,%u,%u,%u", w, h, r, color.r,
             color.g, color.b, color.a);
    const std::string key(prefix);
    auto found = g_shapeCache.find(key);
    if (found != g_shapeCache.end()) return found->second;

    SDL_Surface* surface =
        SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return nullptr;

    SDL_LockSurface(surface);
    auto* pixels = static_cast<Uint32*>(surface->pixels);
    const int stride = surface->pitch / 4;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double sd {roundedDistance(x + 0.5, y + 0.5, w, h, r)};
            const double coverage {std::min(std::max(0.5 - sd, 0.0), 1.0)};
            const Uint32 alpha {
                static_cast<Uint32>(coverage * color.a + 0.5)};
            pixels[y * stride + x] =
                SDL_MapRGBA(surface->format, color.r, color.g, color.b, alpha);
        }
    }
    SDL_UnlockSurface(surface);

    SDL_Texture* texture = SDL_CreateTextureFromSurface(g_renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture) return nullptr;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

    if (g_shapeCache.size() > 200) {
        for (auto& item : g_shapeCache) SDL_DestroyTexture(item.second);
        g_shapeCache.clear();
    }
    g_shapeCache[key] = texture;
    return texture;
}

void fillRounded(int x, int y, int w, int h, int r, SDL_Color color) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) {
        fill(x, y, w, h, color);
        return;
    }
    SDL_Texture* texture = roundedTexture(w, h, r, color);
    if (!texture) return;
    SDL_Rect dst = {x, y, w, h};
    SDL_RenderCopy(g_renderer, texture, nullptr, &dst);
}

void card(int x, int y, int w, int h, int r) {
    fillRounded(x, y, w, h, r, kLine);
    fillRounded(x + 1, y + 1, w - 2, h - 2, r - 1, kPanel);
}

void pill(int x, int y, int w, int h, SDL_Color fillColor, SDL_Color lineColor) {
    fillRounded(x, y, w, h, h / 2, lineColor);
    fillRounded(x + 1, y + 1, w - 2, h - 2, (h - 2) / 2, fillColor);
}

// Anti-aliased inverse of a rounded rectangle: transparent inside, opaque
// outside. Used to round off the corners of square icon textures.
SDL_Texture* invertRoundedTexture(int size, int r, SDL_Color color) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "inv:%d,%d,%u,%u,%u,%u", size, r, color.r,
             color.g, color.b, color.a);
    const std::string key(prefix);
    auto found = g_shapeCache.find(key);
    if (found != g_shapeCache.end()) return found->second;

    SDL_Surface* surface =
        SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return nullptr;

    SDL_LockSurface(surface);
    auto* pixels = static_cast<Uint32*>(surface->pixels);
    const int stride = surface->pitch / 4;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double sd {roundedDistance(x + 0.5, y + 0.5, size, size, r)};
            const double inside {std::min(std::max(0.5 - sd, 0.0), 1.0)};
            const Uint32 alpha {
                static_cast<Uint32>((1.0 - inside) * color.a + 0.5)};
            pixels[y * stride + x] =
                SDL_MapRGBA(surface->format, color.r, color.g, color.b, alpha);
        }
    }
    SDL_UnlockSurface(surface);

    SDL_Texture* texture = SDL_CreateTextureFromSurface(g_renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture) return nullptr;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

    if (g_shapeCache.size() > 200) {
        for (auto& item : g_shapeCache) SDL_DestroyTexture(item.second);
        g_shapeCache.clear();
    }
    g_shapeCache[key] = texture;
    return texture;
}

// Masks the four corners of a texture with the surface behind it.
void maskCorners(int x, int y, int size, int r, SDL_Color bg) {
    SDL_Texture* texture = invertRoundedTexture(size, r, bg);
    if (!texture) return;
    SDL_Rect dst = {x, y, size, size};
    SDL_RenderCopy(g_renderer, texture, nullptr, &dst);
}

double roundedBoxDistance(double px, double py, double hx, double hy, double r) {
    const double dx = std::abs(px) - (hx - r);
    const double dy = std::abs(py) - (hy - r);
    const double ax = std::max(dx, 0.0);
    const double ay = std::max(dy, 0.0);
    return std::sqrt(ax * ax + ay * ay) + std::min(std::max(dx, dy), 0.0) - r;
}

enum class MenuIcon { Gear, Info, Power };

// Signed distance from (x, y) to each menu icon, centred on (0, 0). Every
// shape is analytic so the rasteriser can anti-alias the edges.
double menuIconDistance(MenuIcon icon, double x, double y) {
    const double len = std::sqrt(x * x + y * y);
    if (icon == MenuIcon::Gear) {
        double d = len - 8.4;
        for (int k = 0; k < 8; ++k) {
            const double a = k * 0.7853981633974483;
            const double u = x * std::cos(a) + y * std::sin(a);
            const double v = -x * std::sin(a) + y * std::cos(a);
            d = std::min(d, roundedBoxDistance(u - 10.0, v, 2.2, 2.0, 0.8));
        }
        return std::max(d, 3.4 - len);
    }
    if (icon == MenuIcon::Info) {
        const double ring = std::abs(len - 9.0) - 1.0;
        const double dot = std::sqrt(x * x + (y + 4.4) * (y + 4.4)) - 1.6;
        const double stem = roundedBoxDistance(x, y - 2.2, 1.6, 4.0, 1.2);
        return std::min(ring, std::min(dot, stem));
    }
    const double ring = std::abs(len - 8.8) - 1.0;
    const double notch = roundedBoxDistance(x, y + 8.8, 3.6, 2.6, 0.8);
    const double arc = std::max(ring, -notch);
    const double bar = roundedBoxDistance(x, y + 4.5, 1.4, 6.5, 1.4);
    return std::min(arc, bar);
}

// Rasterises a menu icon once per icon/size/colour combination.
SDL_Texture* menuIconTexture(MenuIcon icon, int size, SDL_Color color) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "menu:%d,%d,%u,%u,%u,%u",
             static_cast<int>(icon), size, color.r, color.g, color.b, color.a);
    const std::string key(prefix);
    auto found = g_shapeCache.find(key);
    if (found != g_shapeCache.end()) return found->second;

    SDL_Surface* surface =
        SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return nullptr;

    const double center = size / 2.0;
    SDL_LockSurface(surface);
    auto* pixels = static_cast<Uint32*>(surface->pixels);
    const int stride = surface->pitch / 4;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double sd = menuIconDistance(
                icon, x + 0.5 - center, y + 0.5 - center);
            const double coverage {std::min(std::max(0.5 - sd, 0.0), 1.0)};
            const Uint32 alpha {
                static_cast<Uint32>(coverage * color.a + 0.5)};
            pixels[y * stride + x] =
                SDL_MapRGBA(surface->format, color.r, color.g, color.b, alpha);
        }
    }
    SDL_UnlockSurface(surface);

    SDL_Texture* texture = SDL_CreateTextureFromSurface(g_renderer, surface);
    SDL_FreeSurface(surface);
    if (!texture) return nullptr;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

    if (g_shapeCache.size() > 200) {
        for (auto& item : g_shapeCache) SDL_DestroyTexture(item.second);
        g_shapeCache.clear();
    }
    g_shapeCache[key] = texture;
    return texture;
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

void drawTextCenteredVC(const std::string& text, int centerX, int centerY,
                        int fontIndex, SDL_Color color) {
    const int h = TTF_FontHeight(g_fonts[fontIndex]);
    drawTextCentered(text, centerX, centerY - h / 2, fontIndex, color);
}

void keycap(int x, int y, int w, int h, const std::string& key) {
    fillRounded(x, y + 1, w, h, 8, kKeyShadow);
    fillRounded(x, y, w, h, 8, kKeyEdge);
    fillRounded(x + 1, y + 1, w - 2, h - 2, 7, kPanel);
    drawTextCenteredVC(key, x + w / 2, y + h / 2, 0, kSub);
}

struct KeyHint {
    std::string key;
    std::string label;
};

std::vector<KeyHint> splitHints(const std::string& raw) {
    static const char* kKnown[] = {"上下", "左右", "Left/Right", "D-pad", "ZL",
                                   "ZR"};
    std::vector<KeyHint> out;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t sep = raw.find("        ", pos);
        std::string item =
            raw.substr(pos, sep == std::string::npos ? std::string::npos
                                                     : sep - pos);
        while (!item.empty() && item.back() == ' ') item.pop_back();

        KeyHint hint;
        for (const char* known : kKnown) {
            const std::string key(known);
            if (item.compare(0, key.size(), key) == 0) {
                size_t start = key.size();
                while (start < item.size() && item[start] == ' ') ++start;
                // A bare key such as "Left/Right" has no label of its own;
                // leave it as plain text instead of an empty keycap.
                if (start >= item.size()) break;
                hint.key = key;
                hint.label = item.substr(start);
                break;
            }
        }
        if (hint.key.empty()) {
            const size_t space = item.find(' ');
            // Only short leading tokens ("A", "ZL", "-") are keycaps; keep
            // phrases such as "Left/Right page" as a single plain label.
            if (space != std::string::npos && space <= 3) {
                hint.key = item.substr(0, space);
                hint.label = item.substr(space + 1);
            } else {
                hint.label = item;
            }
        }
        if (!hint.label.empty()) out.push_back(hint);
        if (sep == std::string::npos) break;
        pos = sep + 8;
    }
    return out;
}

constexpr int kHintKeyHeight = 28;

int keycapWidth(const std::string& key) {
    const int width = textWidth(key, 0) + 14;
    return width < kHintKeyHeight ? kHintKeyHeight : width;
}

// Draws "[key] label" pairs starting at x; returns the x after the last item.
int drawHintRow(const std::string& raw, int x, int y, int gap) {
    const std::vector<KeyHint> hints = splitHints(raw);
    const int textY = y + (kHintKeyHeight - TTF_FontHeight(g_fonts[0])) / 2;
    for (size_t i = 0; i < hints.size(); ++i) {
        const KeyHint& hint = hints[i];
        if (hint.key.empty()) {
            drawText(hint.label, x, textY, 0, kSub);
            x += textWidth(hint.label, 0) + gap;
            continue;
        }
        const int keyW = keycapWidth(hint.key);
        keycap(x, y, keyW, kHintKeyHeight, hint.key);
        drawText(hint.label, x + keyW + 8, textY, 0, kSub);
        x += keyW + 8 + textWidth(hint.label, 0) + gap;
    }
    return x;
}

int hintRowWidth(const std::string& raw, int gap) {
    const std::vector<KeyHint> hints = splitHints(raw);
    int width = 0;
    for (size_t i = 0; i < hints.size(); ++i) {
        const KeyHint& hint = hints[i];
        if (hint.key.empty()) {
            width += textWidth(hint.label, 0);
        } else {
            width += keycapWidth(hint.key) + 8 + textWidth(hint.label, 0);
        }
        if (i + 1 < hints.size()) width += gap;
    }
    return width;
}

// Picks the widest gap that still keeps a hint row inside maxWidth.
int hintGapFor(const std::string& raw, int maxWidth) {
    const int gaps[] = {28, 24, 16, 12};
    for (int i = 0; i < 4; ++i) {
        if (hintRowWidth(raw, gaps[i]) <= maxWidth) return gaps[i];
    }
    return 8;
}

// Thin single-key hint, e.g. "B 返回", centred on a given x.
void drawKeyHintCentered(const std::string& raw, int centerX, int y) {
    const std::vector<KeyHint> hints = splitHints(raw);
    if (hints.empty()) return;
    const KeyHint& hint = hints.front();
    if (hint.key.empty()) {
        drawTextCentered(hint.label, centerX, y, 0, kSub);
        return;
    }
    const int keyW = keycapWidth(hint.key);
    const int total = keyW + 8 + textWidth(hint.label, 0);
    const int x = centerX - total / 2;
    keycap(x, y, keyW, kHintKeyHeight, hint.key);
    drawText(hint.label, x + keyW + 8,
             y + (kHintKeyHeight - TTF_FontHeight(g_fonts[0])) / 2, 0, kSub);
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

bool init() {
    g_lastError.clear();
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return fail("SDL_Init failed");

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    g_window = SDL_CreateWindow("XYS - XinYue Save", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, kWidth, kHeight,
                                SDL_WINDOW_SHOWN);
    if (!g_window) return fail("SDL_CreateWindow failed");

    g_renderer = SDL_CreateRenderer(
        g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) {
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_renderer) return fail("SDL_CreateRenderer failed");
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);

    if (TTF_Init() != 0) return fail("TTF_Init failed");

    // Load the UI font from the console's shared font service so the ROM stays
    // small. Chinese uses the matching shared font, Korean the KO one, and the
    // standard font covers Japanese plus Latin, Greek and Cyrillic.
    PlSharedFontType fontType = PlSharedFontType_Standard;
    if (i18n::isSimplified()) {
        fontType = PlSharedFontType_ChineseSimplified;
    } else if (i18n::isTraditional()) {
        fontType = PlSharedFontType_ChineseTraditional;
    } else if (i18n::isKorean()) {
        fontType = PlSharedFontType_KO;
    }
    if (R_FAILED(plInitialize(PlServiceType_User))) {
        return fail("plInitialize failed");
    }
    g_plReady = true;
    Result rc = plGetSharedFontByType(&g_sharedFont, fontType);
    if (R_FAILED(rc) && fontType != PlSharedFontType_Standard) {
        rc = plGetSharedFontByType(&g_sharedFont, PlSharedFontType_Standard);
    }
    if (R_FAILED(rc) || g_sharedFont.address == nullptr ||
        g_sharedFont.size == 0) {
        return fail("plGetSharedFontByType failed");
    }

    const int sizes[4] = {20, 24, 32, 22};
    for (int i = 0; i < 4; ++i) {
        SDL_RWops* src =
            SDL_RWFromConstMem(g_sharedFont.address, g_sharedFont.size);
        if (!src) return fail("SDL_RWFromConstMem failed");
        g_fonts[i] = TTF_OpenFontRW(src, 1, sizes[i]);
        if (!g_fonts[i]) return fail("TTF_OpenFontRW failed");
    }

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
    if (g_plReady) {
        plExit();
        g_plReady = false;
    }
    IMG_Quit();

    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    g_renderer = nullptr;
    g_window = nullptr;
    SDL_Quit();
}

const char* lastError() { return g_lastError.c_str(); }

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
    const int headerCenter = 6 + 80 / 2;
    const int titleFontH = TTF_FontHeight(g_fonts[2]);
    card(16, 6, 1248, 80, 16);

    const auto statusPill = [&](int right, int y, const std::string& label,
                                SDL_Color dot) {
        const int w = textWidth(label, 0) + 42;
        pill(right - w, y, w, 28, kPillFill, kPillLine);
        fillRounded(right - w + 12, y + 11, 6, 6, 3, dot);
        drawTextCenteredVC(label, right - w + 24 + textWidth(label, 0) / 2,
                           y + 14, 0, kSub);
    };

    const auto drawHeaderTitle = [&](const std::string& title) {
        drawText(title, 40, headerCenter - titleFontH / 2, 2, kText);
    };

    drawHeaderTitle("XYS");
    const int subtitleH = TTF_FontHeight(g_fonts[1]);
    drawText(fitText(text.subtitle, 1, 420), 156, headerCenter - subtitleH / 2,
             1, kMuted);
    fill(140, headerCenter - subtitleH / 2 + 2, 1, subtitleH - 4, kLine);
    statusPill(1240, 16, state.cloud ? text.cloudOn : text.cloudOff,
               state.cloud ? kAccent : kMuted);
    statusPill(1240, 50, state.sdOk ? text.sdOk : text.sdMissing,
               state.sdOk ? kGreen : kRed);

    // Save list.
    card(16, 95, 783, 536, 16);

    const int rowHeight = 88;
    const int visibleRows = 6;
    int first = state.cursor - visibleRows / 2;
    if (first < 0) first = 0;
    const int maxFirst = static_cast<int>(state.rows.size()) - visibleRows;
    if (first > maxFirst) first = maxFirst;
    if (first < 0) first = 0;

    if (state.rows.empty()) {
        drawTextCentered(text.noSaves, 404, 340, 1, kSub);
    } else {
        for (int i = 0; i < visibleRows; ++i) {
            const int index = first + i;
            if (index >= static_cast<int>(state.rows.size())) break;

            const Row& row = state.rows[index];
            const int y = 104 + i * rowHeight;
            const bool selected = index == state.cursor;
            if (selected) {
                fillRounded(24, y - 2, 767, 84, 12, kSelectedLine);
                fillRounded(25, y - 1, 765, 82, 11, kSelected);
                fillRounded(26, y + 12, 3, 56, 2, kAccent);
            } else if (i > 0 && index - 1 != state.cursor) {
                fill(108, y - 3, 659, 1, kHair);
            }

            drawIcon(iconFor(row), 40, y + 12, 56, row.title);
            maskCorners(40, y + 12, 56, 13, selected ? kSelected : kPanel);
            drawText(fitText(row.title, 1, 420), 108, y + 12, 1, kText);

            char meta[64];
            snprintf(meta, sizeof(meta), "%s · %.1f MB", row.user.c_str(),
                     row.sizeMb);
            drawText(fitText(meta, 0, 420), 108, y + 46, 0, kSub);

            if (row.backups > 0) {
                const std::string count =
                    i18n::format(text.backupsFmt, row.backups);
                const int w = textWidth(count, 0) + 22;
                pill(767 - w, y + 12, w, 28, kPillFill, kPillLine);
                drawTextCenteredVC(count, 767 - w / 2, y + 26, 0, kAccent);
                drawTextRight(fitText(row.latest, 0, 247), 767, y + 46, 0,
                              kMuted);
            } else {
                drawTextRight(text.noBackup, 767, y + 12, 0, kMuted);
            }
        }
    }

    // Preview panel.
    card(808, 95, 456, 536, 16);

    if (!state.rows.empty()) {
        const Row& row = state.rows[state.cursor];
        drawIcon(iconFor(row), 918, 144, 236, row.title);
        maskCorners(918, 144, 236, 26, kPanel);

        std::string title = fitText(row.title, 1, row.bcat ? 300 : 400);
        if (row.bcat) {
            const std::string chip = text.bcatLabel;
            const int chipW = textWidth(chip, 0) + 22;
            const int start =
                1036 - (textWidth(title, 1) + 10 + chipW) / 2;
            drawText(title, start, 414, 1, kText);
            const int chipX = start + textWidth(title, 1) + 10;
            pill(chipX, 410, chipW, 28, kPillFill, kPillLine);
            drawTextCenteredVC(chip, chipX + chipW / 2, 424, 0, kAccent);
        } else {
            drawTextCentered(title, 1036, 414, 1, kText);
        }

        fill(872, 456, 328, 1, kHair);

        std::string latestValue = row.backups > 0 ? row.latest : text.noBackup;
        const std::string countValue =
            i18n::format(text.backupsFmt, row.backups);
        const std::string idValue = saves::hexId(row.appId);
        const std::string labels[3] = {text.latestLabel, text.backupsLabel,
                                       text.titleIdLabel};
        std::string values[3] = {latestValue, countValue, idValue};
        for (int i = 0; i < 3; ++i) {
            const int y = 484 + i * 38;
            drawText(labels[i], 872, y, 0, kMuted);
            drawTextRight(fitText(values[i], 0, 260), 1200, y, 0,
                          i == 1 ? kAccent : kSub);
        }
    }

    // Footer. Kept on the list screen; a modal dims it together with the rest
    // of the background instead of leaving the bottom row empty.
    card(16, 640, 1248, 76, 14);
    drawHintRow(text.hints, 32, 650, 28);
    drawText(fitText(state.status, 0, 1200), 32, 686, 0, kAccent);

    if (state.picking) {
        fill(0, 0, kWidth, kHeight, kScrim);
        card(260, 120, 760, 480, 18);
        drawTextCentered(fitText(state.pickTitle, 1, 680), 640, 136, 1, kText);
        drawTextCentered(text.pickTitle, 640, 168, 0, kSub);
        fill(260, 192, 760, 1, kHair);

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
                    fillRounded(276, y - 2, 728, 48, 10, kSelectedLine);
                    fillRounded(277, y - 1, 726, 46, 9, kSelected);
                    fillRounded(278, y + 8, 3, 28, 2, kAccent);
                }
                drawText(fitText(pick.time, 1, 540), 296, y + 5, 1, kText);
                drawTextRight(pick.source, 984, y + 10, 0, kSub);
            }
        }

        drawTextCentered(fitText(state.status, 0, 700), 640, 508, 0, kAccent);
        const std::string pickHint =
            state.pickHint.empty() ? text.pickHint : state.pickHint;
        const int pickGap = hintGapFor(pickHint, 712);
        drawHintRow(pickHint,
                    640 - hintRowWidth(pickHint, pickGap) / 2, 550, pickGap);
    }

    if (state.screen == Screen::Menu) {
        fill(0, 0, kWidth, kHeight, kScrim);
        const int menuTop = (kHeight - 374) / 2;
        card(300, menuTop, 680, 374, 20);

        drawText(text.menuTitle, 340, menuTop + 22, 2, kText);
        const std::string version = "v" + state.version;
        drawTextRight(version, 940, menuTop + 34, 0, kMuted);
        fill(340, menuTop + 70, 600, 1, kHair);

        const int itemHeight = 68;
        for (int i = 0; i < static_cast<int>(state.menuItems.size()); ++i) {
            const int y = menuTop + 84 + i * itemHeight;
            const bool selected = i == state.menuCursor;
            if (selected) {
                fillRounded(320, y - 1, 640, 60, 12, kSelectedLine);
                fillRounded(321, y, 638, 58, 11, kSelected);
                fillRounded(322, y + 12, 3, 34, 2, kAccent);
            }

            fillRounded(340, y + 11, 36, 36, 10,
                        selected ? kAccent : kPillFill);
            static const MenuIcon kIcons[3] = {MenuIcon::Gear, MenuIcon::Info,
                                               MenuIcon::Power};
            SDL_Texture* icon =
                menuIconTexture(kIcons[i % 3], 36, selected ? kPanel : kAccent);
            if (icon) {
                SDL_Rect dst = {340, y + 11, 36, 36};
                SDL_RenderCopy(g_renderer, icon, nullptr, &dst);
            }

            drawText(fitText(state.menuItems[i], 1, 460), 396, y + 17, 1,
                     selected ? kText : kSub);
        }

        const int dividerY = menuTop + 84 + 3 * itemHeight - 6;
        fill(340, dividerY, 600, 1, kHair);
        const int menuGap = hintGapFor(text.menuHint, 600);
        drawHintRow(text.menuHint,
                    640 - hintRowWidth(text.menuHint, menuGap) / 2,
                    dividerY + 16, menuGap);
    } else if (state.screen == Screen::Settings) {
        fill(0, 0, kWidth, kHeight, kBg);
        card(16, 6, 1248, 80, 16);
        drawHeaderTitle(text.settingsTitle);
        statusPill(1240, 32, state.cloud ? text.cloudOn : text.cloudOff,
                   state.cloud ? kAccent : kMuted);

        card(160, 95, 960, 536, 16);

        const int fieldHeight = 57;
        const int fieldTop = 104;
        const int fieldX = 336;
        const int fieldRight = 1072;

        const auto group = [&](int y, const char* label) {
            const int cy = y + 1 + TTF_FontHeight(g_fonts[0]) / 2;
            fillRounded(189, cy - 4, 8, 8, 4, kAccent);
            drawText(label, 206, y + 1, 0, kAccent);
            const int lineX = 206 + textWidth(label, 0) + 12;
            fill(lineX, cy, 296 - lineX, 1, kPillLine);
        };
        group(fieldTop + 2, text.settingsCloudGroup);
        group(fieldTop + 3 * fieldHeight + 2, text.settingsLocalGroup);

        const int fieldCount = static_cast<int>(state.fields.size());
        for (int i = 0; i < fieldCount; ++i) {
            const FieldRow& field = state.fields[i];
            const int y = fieldTop + i * fieldHeight;
            const bool selected = i == state.fieldCursor;
            if (selected) {
                fillRounded(320, y - 2, 784, 56, 10, kSelectedLine);
                fillRounded(321, y - 1, 782, 54, 9, kSelected);
                fillRounded(322, y + 11, 3, 34, 2, kAccent);
            } else if (i != 0 && i != 3) {
                fill(fieldX, y - 2, fieldRight - fieldX, 1, kHair);
            }

            const bool isSwitch = i >= 4 && i <= 7;
            if (isSwitch) {
                drawText(field.label, fieldX, y + 16, 0, kText);
                const int tx = 1024;
                const int ty = y + 16;
                if (field.value == text.settingsOn) {
                    fillRounded(tx, ty, 48, 24, 12, kAccent);
                    fillRounded(tx + 27, ty + 3, 18, 18, 9, kPanel);
                } else {
                    fillRounded(tx, ty, 48, 24, 12, kTrackOffLine);
                    fillRounded(tx + 1, ty + 1, 46, 22, 11, kTrackOff);
                    fillRounded(tx + 3, ty + 3, 18, 18, 9, kPanel);
                }
            } else {
                drawText(field.label, fieldX, y + 4, 0, kSub);

                int valueWidth = fieldRight - fieldX;
                if (selected && !state.fieldExample.empty()) {
                    const int labelRight =
                        fieldX + textWidth(field.label, 0) + 24;
                    drawTextRight(
                        fitText(state.fieldExample, 0, fieldRight - labelRight),
                        fieldRight, y + 4, 0, kMuted);
                }

                std::string value = field.value;
                if (field.secret) {
                    value.assign(value.size() > 40 ? 40 : value.size(), '*');
                }
                if (value.empty()) {
                    const std::string placeholder =
                        field.emptyValue.empty() ? text.notSet
                                                 : field.emptyValue;
                    drawText(fitText(placeholder, 0, valueWidth), fieldX,
                             y + 26, 0, kMuted);
                } else {
                    drawText(fitText(value, 3, valueWidth), fieldX, y + 24, 3,
                             kText);
                }
            }

            const char* switchHint = nullptr;
            if (i == 4) switchHint = text.hintSnapshot;
            if (i == 5) switchHint = text.hintSound;
            if (i == 6) switchHint = text.hintJksv;
            if (selected && switchHint) {
                const int labelRight = fieldX + textWidth(field.label, 0) + 24;
                drawTextRight(fitText(switchHint, 0, 1000 - labelRight), 1000,
                              y + 16, 0, kMuted);
            } else if (i == 7 && selected) {
                const int labelRight = fieldX + textWidth(field.label, 0) + 24;
                const int right = 1000;
                drawTextRight(fitText(text.exampleBcat, 0, right - labelRight),
                              right, y + 16, 0, kMuted);
            }
        }

        card(16, 640, 1248, 76, 14);
        drawHintRow(text.settingsHint, 32, 650, 40);
        drawText(fitText(state.status, 0, 1200), 32, 686, 0, kAccent);
    } else if (state.screen == Screen::About) {
        fill(0, 0, kWidth, kHeight, kBg);
        card(16, 6, 1248, 80, 16);
        drawHeaderTitle(text.aboutTitle);

        card(160, 95, 960, 536, 16);

        const int iconSize = 160;
        const int iconX = 200;
        const int iconY = 128;
        SDL_Texture* icon = fileTexture(state.aboutIconPath);
        if (icon) {
            SDL_Rect dst = {iconX, iconY, iconSize, iconSize};
            SDL_RenderCopy(g_renderer, icon, nullptr, &dst);
            maskCorners(iconX, iconY, iconSize, 32, kPanel);
        } else {
            fillRounded(iconX, iconY, iconSize, iconSize, 32, kPlaceholder);
            drawTextCentered("XYS", iconX + iconSize / 2,
                             iconY + iconSize / 2 - 18, 2, kSub);
        }

        drawText("XYS", 392, 138, 2, kText);

        drawText(i18n::format(text.aboutVersionFmt, state.version.c_str()), 394,
                 191, 0, kMuted);

        drawText(i18n::format(text.aboutAuthorFmt, state.author.c_str()), 394,
                 228, 0, kMuted);
        drawText(text.aboutRepo, 394, 265, 0, kAccent);

        drawText(fitText(text.aboutIntro, 1, 880), 200, 344, 1, kText);
        drawText(fitText(text.aboutIntro2, 0, 880), 200, 390, 0, kSub);
        drawText(fitText(text.aboutLibs, 0, 880), 200, 426, 0, kSub);

        fill(200, 508, 880, 1, kHair);
        drawKeyHintCentered(text.aboutHint, 640, 558);
    }

    if (state.prompt) {
        fill(0, 0, kWidth, kHeight, kScrim);
        card(320, 236, 640, 248, 18);
        // Long game names would run past the dialog edges, so drop to the
        // smaller font before truncating the title.
        int titleFont = 2;
        if (textWidth(state.promptTitle, titleFont) > 560) titleFont = 1;
        drawTextCentered(fitText(state.promptTitle, titleFont, 560), 640,
                         titleFont == 2 ? 258 : 264, titleFont, kText);
        drawTextWrapped(state.promptMessage, 368, 314, 544, 0, kSub, 28);
        fill(368, 392, 544, 1, kHair);
        const int promptGap = hintGapFor(state.promptHint, 544);
        drawHintRow(state.promptHint,
                    640 - hintRowWidth(state.promptHint, promptGap) / 2, 408,
                    promptGap);
    }

    if (state.progress >= 0) {
        fill(0, 0, kWidth, kHeight, kScrim);
        card(340, 254, 600, 212, 18);
        drawText(state.progressTitle, 368, 278, 1, kText);

        int percent = state.progress;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        char label[16];
        snprintf(label, sizeof(label), "%d%%", percent);
        drawTextRight(label, 912, 284, 0, kMuted);

        if (!state.progressStatus.empty()) {
            drawText(fitText(state.progressStatus, 0, 544), 368, 314, 0, kSub);
        }

        const int barX = 368;
        const int barY = 354;
        const int barWidth = 544;
        const int barHeight = 12;
        fillRounded(barX, barY, barWidth, barHeight, 6, kPillLine);
        fillRounded(barX + 1, barY + 1, barWidth - 2, barHeight - 2, 5,
                    kTrackOff);
        if (percent > 0) {
            fillRounded(barX, barY, barWidth * percent / 100, barHeight, 6,
                        kAccent);
        }

        const std::string hint =
            state.progressHint.empty() ? text.progressHint : state.progressHint;
        const int hintGap = hintGapFor(hint, 544);
        drawHintRow(hint, 640 - hintRowWidth(hint, hintGap) / 2, 410, hintGap);
    }

    SDL_RenderPresent(g_renderer);
}

}  // namespace ui
