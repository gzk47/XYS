#pragma once

#include <functional>
#include <string>
#include <vector>

namespace zipx {

struct Extra {
    std::string name;
    std::vector<unsigned char> data;
};

// Result of inspecting an archive without extracting it.
struct Summary {
    unsigned fileCount = 0;
    unsigned long long totalBytes = 0;
};

// Packs every file below dir into zipPath. Entry names are relative to dir, so
// the archive root matches JKSV save backups. `extras` are written first.
// `onProgress` receives the percentage of source bytes packed so far and
// `cancelled` may abort the pack; a cancelled pack is deleted.
bool create(const std::string& dir, const std::string& zipPath,
            const std::vector<Extra>& extras, std::string& err,
            const std::function<void(int)>& onProgress = {},
            const std::function<bool()>& cancelled = {});

// Reads the archive index and rejects unsafe entry names. Call before wiping a
// save so an empty or malformed backup cannot destroy the existing data.
bool inspect(const std::string& zipPath, const std::string& skipName,
             Summary& out, std::string& err);

// Extracts all entries into destDir, skipping `skipName`. Rejects absolute and
// parent-traversing names. When journalSize > 0, `commit` is invoked whenever
// the amount of data written since the last commit would exceed it. Save data
// filesystems reject writes that overflow their journal. `onProgress` receives
// the percentage of uncompressed bytes written so far. Every entry is checked
// against its CRC before the function reports success.
bool extract(const std::string& zipPath, const std::string& destDir,
             const std::string& skipName, long long journalSize,
             const std::function<bool()>& commit, std::string& err,
             const std::function<void(int)>& onProgress = {});

// Reads a single entry by name. Returns false when the entry is missing.
bool readEntry(const std::string& zipPath, const std::string& name,
               std::vector<unsigned char>& out, std::string& err);

// Content fingerprint of a directory tree: relative names, sizes and CRC32 of
// every file, in sorted order. Two trees with the same hash hold the same
// data. Used to skip backups when nothing changed.
bool hashDir(const std::string& dir, std::string& outHash, std::string& err,
             const std::function<bool()>& cancelled = {});

}  // namespace zipx
