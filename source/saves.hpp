#pragma once

#include <switch.h>

#include <functional>
#include <string>
#include <vector>

struct SaveEntry {
    u64 application_id = 0;
    AccountUid uid = {};
    u64 save_data_id = 0;
    u64 system_save_data_id = 0;
    u64 owner_id = 0;
    u64 size = 0;
    u8 space = 0;
    u8 type = 0;
    u8 rank = 0;
    u16 index = 0;
    std::string title;
    std::string user;
};

struct BackupInfo {
    int count = 0;
    std::string latestZip;    // absolute path to the newest .zip
    std::string latestStamp;  // "20261116-201533"
    std::string latestHuman;  // "2026-11-16 20:15:33"
    std::string iconPath;     // <backup dir>/icon.jpg when present
};

namespace saves {

// One restorable archive found on the SD card.
struct BackupFile {
    std::string path;
    std::string stamp;   // file name without .zip
    std::string human;   // "2026-11-16 20:15:33" when the name looks like a stamp
    std::string source;  // "local", "JKSV" or "cloud"
    std::string owner;   // Username prefix, empty for older archives.
    std::string note;    // User-written note from <backup dir>/meta.txt.
    bool autoSnapshot = false;  // created automatically before a restore
};

std::vector<SaveEntry> list();
std::string hexId(u64 id);
std::string timestampNow();

// "20261116-201533" -> "2026-11-16 20:15:33"
std::string humanStamp(const std::string& name);

// Metadata (content hash and user note) is stored in
// <backup dir>/meta.txt, one line per archive. Notes are local-only.
bool setNote(const std::string& zipPath, const std::string& note);
void removeMeta(const std::string& zipPath);

BackupInfo backupInfo(const SaveEntry& entry, const std::string& root);

// Local and optionally JKSV archives for one game, newest first.
std::vector<BackupFile> listBackups(const SaveEntry& entry,
                                    const std::string& root,
                                    bool includeJksv);

// Deletes older local archives of the same user, keeping the newest `keep`
// normal backups and `keep` automatic snapshots. 0 leaves everything alone.
int pruneBackups(const SaveEntry& entry, const std::string& root, int keep);

// NACP icon (JPEG) of an installed application.
bool loadIcon(u64 applicationId, std::vector<unsigned char>& out);

// Writes <root>/<TitleID>/<prefix><username>_<stamp>.zip (JKSV-compatible
// archive) and refreshes the icon cache. Returns the archive path.
// `onProgress` receives the pack percentage; `cancelled` aborts the backup.
// When the save is byte-identical to the newest backup of the same user and
// `unchanged` is given, no new archive is written: *unchanged is set and
// outZip points at the existing archive. Automatic snapshots always pack.
bool backup(const SaveEntry& entry, const std::string& root, std::string& outZip,
            std::string& err,
            const std::function<void(int)>& onProgress = {},
            const std::function<bool()>& cancelled = {},
            const std::string& namePrefix = "", bool* unchanged = nullptr);

// Restores any JKSV / XYS zip. `.nx_save_meta.bin` is skipped and
// used to extend the save container when needed. `onProgress` receives the
// percentage of data written.
bool restore(const SaveEntry& entry, const std::string& zipPath, std::string& err,
             const std::function<void(int)>& onProgress = {});

// Path of the newest local backup, used when uploading.
bool zipForUpload(const SaveEntry& entry, const std::string& root,
                  std::string& zipPath, std::string& err);

// Moves a downloaded zip into the local library under <stamp>.zip.
bool importZip(const SaveEntry& entry, const std::string& root,
               const std::string& sourceZip, const std::string& stamp,
               std::string& outZip, std::string& err);

}  // namespace saves
