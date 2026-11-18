#pragma once

#include <string>

struct Config {
    std::string baseUrl;
    std::string username;
    std::string password;
    std::string backupDir;
    // Keep a snapshot of the current save before every restore.
    bool autoSnapshot = true;
    // Play short UI sounds for moves and confirmations.
    bool sound = true;
    // Include read-only JKSV archives in the backup picker.
    bool readJksv = true;
    // Keep at most this many local archives per game and kind (0 = unlimited).
    int keepBackups = 10;
};

namespace config {

const char* path();
Config load();
bool save(const Config& cfg);
void ensureTemplate();

}  // namespace config
