#pragma once

#include <string>
#include <vector>

#include "saves.hpp"

namespace ui {

struct Row {
    u64 appId = 0;
    std::string title;
    std::string user;
    bool bcat = false;
    double sizeMb = 0.0;
    int backups = 0;
    std::string latest;    // human readable time, empty when there is none
    std::string iconPath;  // icon.jpg stored inside the latest backup
};

struct PickRow {
    std::string time;    // "2026-11-16 22:37:00" or the raw file name
    std::string source;  // "本地" / "JKSV" / "云端"
};

enum class Screen { Main, Menu, Settings, About };

struct FieldRow {
    std::string label;
    std::string value;
    bool secret = false;
    std::string emptyValue;
};

struct State {
    std::vector<Row> rows;
    int cursor = 0;
    std::string status;
    bool sdOk = true;
    bool cloud = false;

    // Backup picker overlay.
    bool picking = false;
    std::string pickTitle;
    std::vector<PickRow> picks;
    int pickCursor = 0;
    std::string pickHint;  // optional override of the default hint

    // Menu / settings / about screens.
    Screen screen = Screen::Main;
    std::vector<std::string> menuItems;
    int menuCursor = 0;
    std::vector<FieldRow> fields;
    int fieldCursor = 0;
    std::string fieldExample;
    std::string version;
    std::string author;
    std::string aboutIconPath;

    // Confirmation dialog (restore / delete / backup).
    bool prompt = false;
    std::string promptTitle;
    std::string promptMessage;
    std::string promptHint;

    // Restore / background job progress, -1 when idle.
    int progress = -1;
    std::string progressTitle;
    std::string progressStatus;
    std::string progressHint;
};

bool init();
void shutdown();
// Reason for the last init() failure; empty once init() succeeds.
const char* lastError();
void draw(const State& state);
void invalidateIcons();

}  // namespace ui
