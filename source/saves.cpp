#include "saves.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sys/stat.h>
#include <utility>

#include "fsutil.hpp"
#include "i18n.hpp"
#include "sort_key.h"
#include "zipx.hpp"

namespace saves {

namespace {

std::string bounded(const char* data, size_t size) {
    size_t len = 0;
    while (len < size && data[len] != '\0') ++len;
    return std::string(data, len);
}

// Caches the last control data read: list() looks up one title at a time and
// restore reuses the same entry for its journal size.
NacpStruct* controlFor(u64 applicationId) {
    static NsApplicationControlData control;
    static u64 cachedId = 0;
    static bool cached = false;
    if (cached && cachedId == applicationId) return &control.nacp;

    u64 actualSize = 0;
    Result rc = nsGetApplicationControlData(
        NsApplicationControlSource_Storage, applicationId, &control,
        sizeof(control), &actualSize);
    cachedId = applicationId;
    cached = R_SUCCEEDED(rc);
    return cached ? &control.nacp : nullptr;
}

std::string lookupTitle(u64 applicationId) {
    NacpStruct* nacp = controlFor(applicationId);
    if (!nacp) return "Unknown";

    NacpLanguageEntry* language = nullptr;
    if (R_SUCCEEDED(nacpGetLanguageEntry(nacp, &language)) && language) {
        return bounded(language->name, sizeof(language->name));
    }
    return "Unknown";
}

// Fallback for saves whose filesystem reports no journal size (device saves
// such as Animal Crossing), mirroring JKSV's NACP lookup.
s64 journalFromNacp(u64 applicationId, u8 type) {
    const NacpStruct* nacp = controlFor(applicationId);
    if (!nacp) return 0;

    if (type == FsSaveDataType_Device) {
        if (nacp->device_save_data_journal_size > 0) {
            return static_cast<s64>(nacp->device_save_data_journal_size);
        }
        return static_cast<s64>(nacp->device_save_data_journal_size_max);
    }
    // BCAT has no dedicated journal field; JKSV uses the delivery cache size.
    if (type == FsSaveDataType_Bcat) {
        return static_cast<s64>(nacp->bcat_delivery_cache_storage_size);
    }
    if (nacp->user_account_save_data_journal_size > 0) {
        return static_cast<s64>(nacp->user_account_save_data_journal_size);
    }
    return static_cast<s64>(nacp->user_account_save_data_journal_size_max);
}

std::vector<std::pair<AccountUid, std::string>> loadUsers() {
    std::vector<std::pair<AccountUid, std::string>> users;
    AccountUid uids[ACC_USER_LIST_SIZE] = {};
    s32 total = 0;
    if (R_FAILED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &total))) {
        return users;
    }

    for (s32 i = 0; i < total; ++i) {
        std::string nickname = "User";
        AccountProfile profile = {};
        if (R_SUCCEEDED(accountGetProfile(&profile, uids[i]))) {
            AccountProfileBase base = {};
            if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &base))) {
                std::string value = bounded(base.nickname, sizeof(base.nickname));
                if (!value.empty()) nickname = value;
            }
            accountProfileClose(&profile);
        }
        users.emplace_back(uids[i], nickname);
    }
    return users;
}

bool sameUid(const AccountUid& lhs, const AccountUid& rhs) {
    return lhs.uid[0] == rhs.uid[0] && lhs.uid[1] == rhs.uid[1];
}

std::string findUser(const std::vector<std::pair<AccountUid, std::string>>& users,
                     const AccountUid& uid) {
    for (const auto& item : users) {
        if (sameUid(item.first, uid)) return item.second;
    }
    return "User";
}

bool isAllZero(const AccountUid& uid) {
    return uid.uid[0] == 0 && uid.uid[1] == 0;
}

}  // namespace

std::string hexId(u64 id) {
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016llX",
             static_cast<unsigned long long>(id));
    return std::string(buffer);
}

std::string timestampNow() {
    u64 timestamp = 0;
    TimeCalendarTime calendar = {};
    TimeCalendarAdditionalInfo info = {};
    if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &timestamp)) ||
        R_FAILED(timeToCalendarTimeWithMyRule(timestamp, &calendar, &info))) {
        return "19700101-000000";
    }

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%04u%02u%02u-%02u%02u%02u",
             calendar.year, calendar.month, calendar.day, calendar.hour,
             calendar.minute, calendar.second);
    return std::string(buffer);
}

// JKSV's timestamp suffix: "YYYY.MM.DD @ HH.MM.SS".
bool isJksvStamp(const std::string& text) {
    if (text.size() != 21) return false;
    if (text[4] != '.' || text[7] != '.' || text[10] != ' ' ||
        text[11] != '@' || text[12] != ' ' || text[15] != '.' ||
        text[18] != '.') {
        return false;
    }
    for (size_t i = 0; i < 21; ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 11 || i == 12 || i == 15 ||
            i == 18) {
            continue;
        }
        if (!isdigit(static_cast<unsigned char>(text[i]))) return false;
    }
    return true;
}

// XYS timestamp prefix: "YYYYMMDD-HHMMSS".
bool hasLocalStamp(const std::string& text) {
    if (text.size() < 15 || text[8] != '-') return false;
    for (size_t i = 0; i < 15; ++i) {
        if (i != 8 && !isdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
    }
    return true;
}

std::string ownerFromStamp(const std::string& stamp) {
    // Automatic snapshots add an "AUTO_" marker before the username. JKSV
    // names its automatic backups "AUTO - <user> - <date>".
    std::string body = stamp;
    if (body.rfind("AUTO_", 0) == 0) {
        body = body.substr(5);
    } else if (body.rfind("AUTO - ", 0) == 0) {
        body = body.substr(7);
    }

    // Local archives are "<user>_YYYYMMDD-HHMMSS".
    const size_t separator = body.rfind('_');
    if (separator != std::string::npos && separator > 0) {
        if (hasLocalStamp(body.substr(separator + 1))) {
            return body.substr(0, separator);
        }
    }

    // JKSV archives are "<user> - YYYY.MM.DD @ HH.MM.SS".
    const size_t jksv = body.rfind(" - ");
    if (jksv != std::string::npos && jksv > 0) {
        if (isJksvStamp(body.substr(jksv + 3))) return body.substr(0, jksv);
    }
    return "";
}

std::string displayOwner(const std::string& owner) {
    // ownerName() escapes at most once, and names that already begin with
    // "U_" are escaped as well, so one leading "U_" is always the escape.
    return owner.rfind("U_", 0) == 0 ? owner.substr(2) : owner;
}

std::string humanStamp(const std::string& folder) {
    if (folder.size() >= 21) {
        const std::string tail = folder.substr(folder.size() - 21);
        if (isJksvStamp(tail)) {
            return tail.substr(0, 4) + "-" + tail.substr(5, 2) + "-" +
                   tail.substr(8, 2) + " " + tail.substr(13, 2) + ":" +
                   tail.substr(16, 2) + ":" + tail.substr(19, 2);
        }
    }

    for (size_t pos = 0; pos + 15 <= folder.size(); ++pos) {
        if (!hasLocalStamp(folder.substr(pos, 15))) continue;
        return folder.substr(pos, 4) + "-" + folder.substr(pos + 4, 2) + "-" +
               folder.substr(pos + 6, 2) + " " + folder.substr(pos + 9, 2) +
               ":" + folder.substr(pos + 11, 2) + ":" +
               folder.substr(pos + 13, 2);
    }
    return folder;
}

std::vector<SaveEntry> list(bool includeBcat) {
    std::vector<SaveEntry> entries;

    FsSaveDataInfoReader reader;
    if (R_FAILED(fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_All))) {
        return entries;
    }

    const auto users = loadUsers();
    std::map<u64, std::string> titleCache;

    FsSaveDataInfo info;
    s64 total = 0;
    while (R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, &info, 1, &total)) &&
           total > 0) {
        const bool device = info.save_data_type == FsSaveDataType_Device;
        const bool bcat = info.save_data_type == FsSaveDataType_Bcat;
        if (info.save_data_type != FsSaveDataType_Account && !device &&
            !(bcat && includeBcat)) {
            continue;
        }
        if (info.save_data_space_id != FsSaveDataSpaceId_User &&
            info.save_data_space_id != FsSaveDataSpaceId_SdUser) {
            continue;
        }
        if (info.save_data_index != 0) continue;
        if (info.application_id == 0) continue;
        // Device saves are shared by the whole console and carry no user id.
        // BCAT belongs to the title as well, so neither maps to an account.
        if (!device && !bcat && isAllZero(info.uid)) continue;

        SaveEntry entry;
        entry.application_id = info.application_id;
        entry.uid = info.uid;
        entry.save_data_id = info.save_data_id;
        entry.system_save_data_id = info.system_save_data_id;
        entry.owner_id = 0;
        entry.size = info.size;
        entry.space = info.save_data_space_id;
        entry.type = info.save_data_type;
        entry.rank = info.save_data_rank;
        entry.index = info.save_data_index;
        entry.user = (device || bcat) ? "" : findUser(users, info.uid);

        auto cached = titleCache.find(entry.application_id);
        if (cached != titleCache.end()) {
            entry.title = cached->second;
        } else {
            entry.title = lookupTitle(entry.application_id);
            titleCache[entry.application_id] = entry.title;
        }
        entry.sortKey = xys::makeSortKey(entry.title);
        entries.push_back(entry);
    }
    fsSaveDataInfoReaderClose(&reader);

    std::sort(entries.begin(), entries.end(),
              [](const SaveEntry& lhs, const SaveEntry& rhs) {
                  if (lhs.sortKey != rhs.sortKey) return lhs.sortKey < rhs.sortKey;
                  if (lhs.user != rhs.user) return lhs.user < rhs.user;
                  // Device and BCAT both have an empty user, so keep account ->
                  // device -> BCAT order stable within one title.
                  const auto rankOf = [](const SaveEntry& entry) {
                      if (entry.type == FsSaveDataType_Device) return 1;
                      if (entry.type == FsSaveDataType_Bcat) return 2;
                      return 0;
                  };
                  if (rankOf(lhs) != rankOf(rhs)) return rankOf(lhs) < rankOf(rhs);
                  if (lhs.uid.uid[0] != rhs.uid.uid[0]) {
                      return lhs.uid.uid[0] < rhs.uid.uid[0];
                  }
                  return lhs.uid.uid[1] < rhs.uid.uid[1];
              });
    return entries;
}

namespace {

#pragma pack(push, 1)
struct NxSaveMeta {
    u32 magic;
    u8 revision;
    u64 application_id;
    AccountUid uid;
    u64 system_save_data_id;
    u8 save_data_type;
    u8 save_data_rank;
    u16 save_data_index;
    u64 owner_id;
    u64 timestamp;
    u32 flags;
    s64 save_data_size;
    s64 journal_size;
    u64 commit_id;
    u8 save_data_space_id;
};
#pragma pack(pop)

static_assert(sizeof(NxSaveMeta) == 86, "save meta must stay 86 bytes");

// XYS writes its own magic; JKSV archives still carry theirs, so both values
// are accepted when reading.
constexpr u32 kMagicXys = 0x00535958;   // "XYS\0"
constexpr u32 kMagicJksv = 0x56534B4A;  // "JKSV"
const char* const kNxSaveMetaName = ".nx_save_meta.bin";

bool endsWithZip(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return ext == ".zip";
}

std::string stripZip(const std::string& name) {
    return endsWithZip(name) ? name.substr(0, name.size() - 4) : name;
}

// Archives are prefixed with the owner name. Device and BCAT archives use the
// reserved "Device_"/"BCAT_" prefixes and only belong to their own row; they
// must never be mixed with account archives of the same title.
enum class StampKind { Account, Device, Bcat, Unknown };

StampKind stampKind(const std::string& name) {
    std::string base = name;
    if (base.rfind("AUTO_", 0) == 0) {
        base = base.substr(5);
    } else if (base.rfind("AUTO - ", 0) == 0) {
        base = base.substr(7);
    }
    // Our own archives use "<owner>_<stamp>"; JKSV uses "<owner> - <date>" and
    // writes the reserved owner names "Device"/"BCAT" for device and BCAT data.
    if (base.rfind("BCAT_", 0) == 0 || base.rfind("BCAT - ", 0) == 0) {
        return StampKind::Bcat;
    }
    if (base.rfind("Device_", 0) == 0 || base.rfind("Device - ", 0) == 0) {
        return StampKind::Device;
    }
    // Recognised account layouts answer without opening the archive.
    const size_t separator = base.rfind('_');
    if (separator != std::string::npos && separator > 0 &&
        hasLocalStamp(base.substr(separator + 1))) {
        return StampKind::Account;
    }
    const size_t jksv = base.rfind(" - ");
    if (jksv != std::string::npos && jksv > 0 &&
        isJksvStamp(base.substr(jksv + 3))) {
        return StampKind::Account;
    }
    // Renamed archives: the caller may read the metadata to classify them.
    return StampKind::Unknown;
}

StampKind entryKind(u8 type) {
    const auto saveType = static_cast<FsSaveDataType>(type);
    if (saveType == FsSaveDataType_Bcat) return StampKind::Bcat;
    if (saveType == FsSaveDataType_Device) return StampKind::Device;
    return StampKind::Account;
}

long long modifiedTime(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return 0;
    return static_cast<long long>(info.st_mtime);
}

bool readNxMeta(const std::string& zipPath, NxSaveMeta& meta) {
    std::vector<unsigned char> data;
    std::string err;
    if (!zipx::readEntry(zipPath, kNxSaveMetaName, data, err)) return false;
    if (data.size() < sizeof(NxSaveMeta)) return false;
    std::memcpy(&meta, data.data(), sizeof(meta));
    return meta.magic == kMagicXys || meta.magic == kMagicJksv;
}

// Renamed archives lose their type prefix, so read the save type from the
// metadata instead. Archives without usable metadata stay account saves, which
// is what all of them looked like before the types were split.
StampKind kindFromMeta(const std::string& zipPath) {
    NxSaveMeta meta = {};
    if (!readNxMeta(zipPath, meta)) return StampKind::Account;
    return entryKind(meta.save_data_type);
}

NxSaveMeta buildNxMeta(const SaveEntry& entry) {
    NxSaveMeta meta = {};
    meta.magic = kMagicXys;
    meta.revision = 1;
    meta.application_id = entry.application_id;
    meta.uid = entry.uid;
    meta.system_save_data_id = entry.system_save_data_id;
    meta.save_data_type = entry.type;
    meta.save_data_rank = entry.rank;
    meta.save_data_index = entry.index;
    meta.owner_id = entry.owner_id;
    meta.save_data_size = static_cast<s64>(entry.size);
    meta.save_data_space_id = entry.space;

    FsSaveDataExtraData extra = {};
    if (R_SUCCEEDED(fsReadSaveDataFileSystemExtraData(
            &extra, sizeof(extra), entry.save_data_id))) {
        meta.owner_id = extra.owner_id;
        meta.timestamp = extra.timestamp;
        meta.flags = extra.flags;
        meta.save_data_size = extra.data_size;
        meta.journal_size = extra.journal_size;
        meta.commit_id = extra.commit_id;
    }
    return meta;
}

std::string readSmallText(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return "";
    char buffer[256] = {};
    const size_t got = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    return std::string(buffer, got);
}

bool writeFile(const std::string& path, const std::vector<unsigned char>& data) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool ok = data.empty() ||
                    fwrite(data.data(), 1, data.size(), file) == data.size();
    fclose(file);
    return ok;
}

// Backup metadata lives in <backup dir>/meta.txt:
//   <content hash>\t<note>\t<file name>
// One line per archive keeps the backup directory limited to the ZIPs plus
// this single file instead of two sidecars per archive.
struct MetaLine {
    std::string hash;
    std::string note;
};

std::string dirOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string nameOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::map<std::string, MetaLine> readMetaDir(const std::string& dir,
                                            bool* valid = nullptr) {
    std::map<std::string, MetaLine> entries;
    if (valid) *valid = true;
    if (dir.empty()) {
        if (valid) *valid = false;
        return entries;
    }

    FILE* file = fopen(fsx::join(dir, "meta.txt").c_str(), "rb");
    if (!file) {
        if (valid) *valid = false;
        return entries;
    }

    char buffer[512];
    while (fgets(buffer, sizeof(buffer), file)) {
        std::string line(buffer);
        while (!line.empty() &&
               (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        const size_t first = line.find('\t');
        const size_t second = first == std::string::npos
                                  ? std::string::npos
                                  : line.find('\t', first + 1);
        const std::string name = second == std::string::npos
                                     ? std::string()
                                     : line.substr(second + 1);
        if (second == std::string::npos || name.empty()) {
            if (valid) *valid = false;
            continue;
        }

        MetaLine item;
        item.hash = line.substr(0, first);
        item.note = line.substr(first + 1, second - first - 1);
        entries[name] = item;
    }
    if (valid && ferror(file)) *valid = false;
    fclose(file);
    return entries;
}

bool writeMetaDir(const std::string& dir,
                  const std::map<std::string, MetaLine>& entries) {
    if (dir.empty()) return false;

    std::string text;
    for (const auto& entry : entries) {
        if (entry.second.hash.empty() && entry.second.note.empty()) continue;
        text += entry.second.hash;
        text.push_back('\t');
        text += entry.second.note;
        text.push_back('\t');
        text += entry.first;
        text.push_back('\n');
    }

    const std::string path = fsx::join(dir, "meta.txt");
    if (text.empty()) {
        std::remove(path.c_str());
        return true;
    }
    return writeFile(path, std::vector<unsigned char>(text.begin(), text.end()));
}

MetaLine metaFor(const std::string& zipPath) {
    const auto entries = readMetaDir(dirOf(zipPath));
    const auto it = entries.find(nameOf(zipPath));
    return it == entries.end() ? MetaLine{} : it->second;
}

bool writeMetaEntry(const std::string& zipPath, const std::string& hash,
                    const std::string& note) {
    const std::string dir = dirOf(zipPath);
    if (dir.empty()) return false;

    auto entries = readMetaDir(dir);
    const std::string name = nameOf(zipPath);
    MetaLine item = entries[name];
    if (!hash.empty()) item.hash = hash;
    item.note = note;
    if (item.hash.empty() && item.note.empty()) {
        entries.erase(name);
    } else {
        entries[name] = item;
    }
    return writeMetaDir(dir, entries);
}

std::string hashFor(const std::string& zipPath) {
    const MetaLine meta = metaFor(zipPath);
    if (!meta.hash.empty()) return meta.hash;
    return readSmallText(zipPath + ".hash");  // legacy sidecar
}

FsSaveDataAttribute attrFor(const SaveEntry& entry) {
    FsSaveDataAttribute attr = {};
    attr.application_id = entry.application_id;
    attr.save_data_type = entry.type;
    // BCAT is identified by title alone; JKSV leaves the owner fields zeroed.
    if (entry.type == FsSaveDataType_Bcat) return attr;
    attr.uid = entry.uid;
    attr.system_save_data_id = entry.system_save_data_id;
    attr.save_data_rank = entry.rank;
    attr.save_data_index = entry.index;
    return attr;
}

Result mountSave(const char* name, const SaveEntry& entry, bool readOnly) {
    const FsSaveDataAttribute attr = attrFor(entry);
    FsFileSystem filesystem = {};
    const FsSaveDataSpaceId space = static_cast<FsSaveDataSpaceId>(entry.space);
    Result rc = readOnly
                    ? fsOpenReadOnlySaveDataFileSystem(&filesystem, space, &attr)
                    : fsOpenSaveDataFileSystem(&filesystem, space, &attr);
    // BCAT does not always accept the read-only open; JKSV mounts it writable.
    if (R_FAILED(rc) && readOnly && entry.type == FsSaveDataType_Bcat) {
        rc = fsOpenSaveDataFileSystem(&filesystem, space, &attr);
    }
    if (R_FAILED(rc)) return rc;

    if (fsdevMountDevice(name, filesystem) < 0) {
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    return 0;
}

struct Candidate {
    std::string path;
    std::string source;
    long long mtime = 0;
};

std::string localDir(const SaveEntry& entry, const std::string& root) {
    return fsx::join(root, hexId(entry.application_id));
}

// Account nicknames become archive prefixes, so they must not collide with the
// reserved owners ("Device", "BCAT"), the "AUTO_" snapshot marker, the "BCAT_"
// type test or the dot-file filter. "U_" itself is escaped too, which keeps the
// encoding reversible for displayOwner().
bool reservedOwnerName(const std::string& name) {
    return name == "Device" || name == "BCAT" ||
           name.rfind("BCAT_", 0) == 0 || name.rfind("AUTO_", 0) == 0 ||
           name.rfind("U_", 0) == 0 || name[0] == '.';
}

std::string ownerName(const SaveEntry& entry) {
    std::string name;
    if (entry.type == FsSaveDataType_Bcat) {
        name = "BCAT";
    } else {
        name = entry.user.empty() ? "Device" : entry.user;
    }
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    if (name.empty()) return "User";
    if (entry.type == FsSaveDataType_Account && !entry.user.empty() &&
        reservedOwnerName(name)) {
        name = "U_" + name;
    }
    return name;
}

bool newerStamp(const std::string& lhs, const std::string& rhs) {
    const std::string leftTime = humanStamp(lhs);
    const std::string rightTime = humanStamp(rhs);
    return leftTime != rightTime ? leftTime > rightTime : lhs > rhs;
}

size_t utf8Decode(const std::string& text, size_t pos, u32& codepoint) {
    const unsigned char lead = static_cast<unsigned char>(text[pos]);
    if (lead < 0x80) {
        codepoint = lead;
        return 1;
    }

    size_t length = 0;
    u32 value = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        value = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        value = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        value = lead & 0x07;
    } else {
        codepoint = lead;
        return 1;
    }
    if (pos + length > text.size()) {
        codepoint = lead;
        return 1;
    }
    for (size_t i = 1; i < length; ++i) {
        value = (value << 6) |
                (static_cast<unsigned char>(text[pos + i]) & 0x3F);
    }
    codepoint = value;
    return length;
}

// Mirrors JKSV's util::safeString(): the folder is the cleaned title when every
// character is ASCII, otherwise JKSV falls back to the 16-digit title ID.
std::string jksvSafeTitle(const std::string& title) {
    std::string out;
    for (size_t i = 0; i < title.size();) {
        u32 ch = 0;
        i += utf8Decode(title, i, ch);
        if (ch == 0xE9) {  // 'é', JKSV folds it to 'e'
            out += 'e';
        } else if (ch == 0x2122 || ch == 0xA9 || ch == 0xAE) {  // ™ © ®
            out += ' ';
        } else if (ch == ',' || ch == '/' || ch == '\\' || ch == '<' ||
                   ch == '>' || ch == ':' || ch == '"' || ch == '|' ||
                   ch == '?' || ch == '*') {
            out += ' ';
        } else if (ch > 0x7F) {
            return "";
        } else {
            out += static_cast<char>(ch);
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    return out;
}

// Every ZIP inside a JKSV game folder belongs to that folder's game; JKSV does
// not tag its archives, so the folder itself is the only identifier.
void scanJksvDir(const std::string& dir, int depth,
                 std::vector<Candidate>& out) {
    std::vector<std::string> names;
    if (!fsx::listDir(dir, names)) return;
    for (const auto& name : names) {
        const std::string path = fsx::join(dir, name);
        if (fsx::isDir(path)) {
            if (depth > 0) scanJksvDir(path, depth - 1, out);
        } else if (endsWithZip(name)) {
            out.push_back({path, "JKSV", modifiedTime(path)});
        }
    }
}

void scanJksv(const SaveEntry& entry, std::vector<Candidate>& out) {
    const std::string root = "sdmc:/JKSV";
    const std::string byId = fsx::join(root, hexId(entry.application_id));
    scanJksvDir(byId, 1, out);

    const std::string safe = jksvSafeTitle(entry.title);
    if (safe.empty()) return;
    const std::string byName = fsx::join(root, safe);
    if (byName != byId) scanJksvDir(byName, 1, out);
}

void scanLocal(const std::string& dir, int depth,
               std::vector<Candidate>& out) {
    std::vector<std::string> names;
    if (!fsx::listDir(dir, names)) return;
    for (const auto& name : names) {
        const std::string path = fsx::join(dir, name);
        if (fsx::isDir(path)) {
            // Depth 1 also reads the short-lived UID directories from the
            // previous release, so users do not need to move old backups.
            if (depth > 0) scanLocal(path, depth - 1, out);
        } else if (endsWithZip(name)) {
            // Dot files are editor leftovers or the ".tmp-" archive of an
            // interrupted backup; neither is a usable backup.
            if (name[0] == '.') continue;
            out.push_back({path, "local", modifiedTime(path)});
        }
    }
}

std::vector<Candidate> localCandidates(const SaveEntry& entry,
                                       const std::string& root) {
    std::vector<Candidate> all;
    scanLocal(localDir(entry, root), 1, all);

    const StampKind kind = entryKind(entry.type);
    std::vector<Candidate> out;
    out.reserve(all.size());
    for (auto& candidate : all) {
        const size_t slash = candidate.path.find_last_of('/');
        const std::string name =
            stripZip(candidate.path.substr(slash == std::string::npos
                                              ? 0
                                              : slash + 1));
        StampKind fileKind = stampKind(name);
        if (fileKind == StampKind::Unknown) {
            fileKind = kindFromMeta(candidate.path);
        }
        if (fileKind != kind) continue;
        out.push_back(std::move(candidate));
    }
    return out;
}

// A backup that was killed before its rename leaves ".tmp-*.zip" behind.
// Remove those leftovers so they are never shown or uploaded.
void removeStaleTemps(const std::string& dir) {
    std::vector<std::string> names;
    if (!fsx::listDir(dir, names)) return;
    for (const auto& name : names) {
        if (name.rfind(".tmp-", 0) != 0 || !endsWithZip(name)) continue;
        std::remove(fsx::join(dir, name).c_str());
    }
}

}  // namespace

bool stampMatchesType(const std::string& stamp, FsSaveDataType type) {
    // Cloud listings can only be judged by name; an unrecognised name is
    // treated as an account archive.
    StampKind kind = stampKind(stamp);
    if (kind == StampKind::Unknown) kind = StampKind::Account;
    return kind == entryKind(type);
}

BackupInfo backupInfo(const SaveEntry& entry, const std::string& root) {
    BackupInfo info;
    std::vector<Candidate> candidates = localCandidates(entry, root);
    if (candidates.empty()) return info;

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& lhs, const Candidate& rhs) {
                  if (lhs.mtime != rhs.mtime) return lhs.mtime > rhs.mtime;
                  return lhs.path > rhs.path;
              });
    info.count = static_cast<int>(candidates.size());
    info.latestZip = candidates.front().path;
    const size_t slash = info.latestZip.find_last_of('/');
    info.latestStamp = stripZip(info.latestZip.substr(slash + 1));
    info.latestHuman = humanStamp(info.latestStamp);

    std::string icon = fsx::join(localDir(entry, root), "icon.jpg");
    if (!fsx::exists(icon) && !info.latestZip.empty()) {
        const std::string parent = info.latestZip.substr(0, slash);
        icon = fsx::join(parent, "icon.jpg");
    }
    if (fsx::exists(icon)) info.iconPath = icon;
    return info;
}

std::vector<BackupFile> listBackups(const SaveEntry& entry,
                                    const std::string& root,
                                    bool includeJksv) {
    std::vector<Candidate> candidates = localCandidates(entry, root);
    if (includeJksv) scanJksv(entry, candidates);

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& lhs, const Candidate& rhs) {
                  if (lhs.mtime != rhs.mtime) return lhs.mtime > rhs.mtime;
                  return lhs.path > rhs.path;
              });

    const auto localMeta = readMetaDir(localDir(entry, root));

    std::vector<BackupFile> files;
    files.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        BackupFile file;
        file.path = candidate.path;
        const size_t slash = candidate.path.find_last_of('/');
        file.stamp = stripZip(candidate.path.substr(slash + 1));
        file.human = humanStamp(file.stamp);
        file.source = candidate.source;
        file.owner = ownerFromStamp(file.stamp);
        if (candidate.source == "local") {
            const auto it = localMeta.find(file.stamp + ".zip");
            if (it != localMeta.end()) {
                file.note = it->second.note;
            } else {
                file.note = readSmallText(candidate.path + ".note");
            }
        }
        file.autoSnapshot = file.stamp.rfind("AUTO_", 0) == 0 ||
                            file.stamp.rfind("AUTO - ", 0) == 0;
        // Keep every row to its own save type. Names answer for our own and
        // JKSV's default archives; renamed ones fall back to the metadata so a
        // renamed device or BCAT archive still lands in the right row.
        StampKind kind = stampKind(file.stamp);
        if (kind == StampKind::Unknown) kind = kindFromMeta(candidate.path);
        if (kind != entryKind(entry.type)) continue;
        files.push_back(file);
    }
    return files;
}

std::string getNote(const std::string& zipPath) {
    const std::string note = metaFor(zipPath).note;
    return note.empty() ? readSmallText(zipPath + ".note") : note;
}

bool setNote(const std::string& zipPath, const std::string& note) {
    const std::string dir = dirOf(zipPath);
    if (dir.empty()) return false;

    auto entries = readMetaDir(dir);
    const std::string name = nameOf(zipPath);
    MetaLine item = entries[name];
    if (item.hash.empty()) item.hash = readSmallText(zipPath + ".hash");
    item.note = note;
    if (item.hash.empty() && item.note.empty()) {
        entries.erase(name);
    } else {
        entries[name] = item;
    }

    const bool ok = writeMetaDir(dir, entries);
    if (ok) {
        // Fold any legacy sidecars into meta.txt.
        std::remove((zipPath + ".hash").c_str());
        std::remove((zipPath + ".note").c_str());
    }
    return ok;
}

void removeMeta(const std::string& zipPath) {
    const std::string dir = dirOf(zipPath);
    if (!dir.empty()) {
        auto entries = readMetaDir(dir);
        entries.erase(nameOf(zipPath));
        writeMetaDir(dir, entries);
    }
    std::remove((zipPath + ".hash").c_str());
    std::remove((zipPath + ".note").c_str());
}

bool readCloudNotes(const std::string& metaPath, BackupNotes& notes) {
    notes.clear();
    bool valid = false;
    const auto entries = readMetaDir(dirOf(metaPath), &valid);
    if (!valid) return false;
    for (const auto& entry : entries) {
        if (!entry.second.note.empty()) notes[entry.first] = entry.second.note;
    }
    return true;
}

bool setCloudNote(const std::string& metaPath, const std::string& zipName,
                  const std::string& note) {
    const std::string dir = dirOf(metaPath);
    if (dir.empty() || zipName.empty()) return false;
    auto entries = readMetaDir(dir);
    MetaLine item = entries[zipName];
    item.note = note;
    if (item.hash.empty() && item.note.empty()) {
        entries.erase(zipName);
    } else {
        entries[zipName] = item;
    }
    return writeMetaDir(dir, entries);
}

bool removeCloudNote(const std::string& metaPath, const std::string& zipName) {
    const std::string dir = dirOf(metaPath);
    if (dir.empty() || zipName.empty()) return false;
    auto entries = readMetaDir(dir);
    entries.erase(zipName);
    return writeMetaDir(dir, entries);
}

int pruneBackups(const SaveEntry& entry, const std::string& root, int keep) {
    if (keep <= 0) return 0;

    std::vector<BackupFile> normal;
    std::vector<BackupFile> snapshots;
    for (const auto& file : listBackups(entry, root, false)) {
        // JKSV archives are read-only and belong to another tool.
        if (file.source != "local" || file.owner != ownerName(entry)) continue;
        (file.autoSnapshot ? snapshots : normal).push_back(file);
    }

    int removed = 0;
    for (auto* group : {&normal, &snapshots}) {
        std::sort(group->begin(), group->end(),
                  [](const BackupFile& lhs, const BackupFile& rhs) {
                      return lhs.stamp > rhs.stamp;
                  });
        for (size_t i = static_cast<size_t>(keep); i < group->size(); ++i) {
            const std::string& path = group->at(i).path;
            std::remove(path.c_str());
            removeMeta(path);
            ++removed;
        }
    }
    return removed;
}

bool loadIcon(u64 applicationId, std::vector<unsigned char>& out) {
    static NsApplicationControlData control;
    u64 actualSize = 0;
    if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage,
                                             applicationId, &control,
                                             sizeof(control), &actualSize))) {
        return false;
    }

    const unsigned char* data = control.icon;
    const size_t capacity = sizeof(control.icon);
    if (capacity < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;

    size_t size = capacity;
    for (size_t i = capacity - 1; i > 1; --i) {
        if (data[i] == 0xD9 && data[i - 1] == 0xFF) {
            size = i + 1;
            break;
        }
    }
    out.assign(data, data + size);
    return true;
}

bool backup(const SaveEntry& entry, const std::string& root, std::string& outZip,
            std::string& err,
            const std::function<void(int)>& onProgress,
            const std::function<bool()>& cancelled,
            const std::string& namePrefix, bool* unchanged) {
    if (unchanged) *unchanged = false;
    const std::string base = localDir(entry, root);
    if (!fsx::mkdirs(base)) {
        err = "cannot create " + base;
        return false;
    }
    removeStaleTemps(base);

    const std::string stamp = namePrefix + ownerName(entry) + "_" + timestampNow();
    outZip = fsx::join(base, stamp + ".zip");
    for (int suffix = 1; fsx::exists(outZip); ++suffix) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "-%d", suffix);
        outZip = fsx::join(base, stamp + buffer + ".zip");
    }

    const std::string tmp = fsx::join(base, ".tmp-" + stamp + ".zip");
    std::remove(tmp.c_str());

    Result rc = mountSave("ssave", entry, true);
    if (R_FAILED(rc)) {
        err = i18n::str().openSaveFailed;
        return false;
    }

    // Fingerprint the mounted save before packing. When the newest archive of
    // the same user and the same kind already holds these bytes, skip writing
    // a duplicate. Normal backups compare with normal backups, pre-restore
    // snapshots compare with earlier snapshots.
    const bool autoSnapshot = !namePrefix.empty();
    std::string latestSameUser;
    for (const auto& file : listBackups(entry, root, false)) {
        if (file.source == "local" && file.autoSnapshot == autoSnapshot &&
            file.owner == ownerName(entry)) {
            latestSameUser = file.path;
            break;
        }
    }

    std::string fingerprint;
    std::string hashErr;
    if (zipx::hashDir("ssave:/", fingerprint, hashErr, cancelled) &&
        !latestSameUser.empty()) {
        const std::string previous = hashFor(latestSameUser);
        if (!previous.empty() && previous == fingerprint) {
            fsdevUnmountDevice("ssave");
            outZip = latestSameUser;
            if (unchanged) *unchanged = true;
            return true;
        }
    }

    NxSaveMeta meta = buildNxMeta(entry);
    zipx::Extra extra;
    extra.name = kNxSaveMetaName;
    extra.data.resize(sizeof(meta));
    std::memcpy(extra.data.data(), &meta, sizeof(meta));

    const bool packed =
        zipx::create("ssave:/", tmp, {extra}, err, onProgress, cancelled);
    fsdevUnmountDevice("ssave");

    if (!packed) {
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), outZip.c_str()) != 0) {
        err = "cannot finalize backup archive";
        std::remove(tmp.c_str());
        return false;
    }

    // An archive holding only our metadata restores to nothing and would fail
    // the download check later, so reject it while backing up.
    zipx::Summary summary;
    std::string inspectErr;
    if (!zipx::inspect(outZip, kNxSaveMetaName, summary, inspectErr) ||
        summary.fileCount == 0) {
        std::remove(outZip.c_str());
        err = "save data is empty";
        return false;
    }

    if (!fingerprint.empty()) {
        writeMetaEntry(outZip, fingerprint, "");
    }

    std::vector<unsigned char> icon;
    if (loadIcon(entry.application_id, icon)) {
        writeFile(fsx::join(base, "icon.jpg"), icon);
    }
    return true;
}

bool restore(const SaveEntry& entry, const std::string& zipPath, std::string& err,
             const std::function<void(int)>& onProgress) {
    NxSaveMeta meta = {};
    const bool hasMeta = readNxMeta(zipPath, meta);
    // UID is informational only: restoring another user's archive is an
    // intentional and supported way to move or share saves.
    if (hasMeta && meta.application_id != entry.application_id) {
        err = "backup belongs to a different game";
        return false;
    }
    // A device or BCAT archive restored into an account save (or the reverse)
    // would land in the wrong container, so trust the metadata over the name.
    if (hasMeta && entryKind(meta.save_data_type) != entryKind(entry.type)) {
        err = "backup type does not match this save";
        return false;
    }

    // Validate the archive before anything is deleted. Without this an empty,
    // truncated or malicious zip would wipe the save and then "succeed".
    zipx::Summary summary;
    if (!zipx::inspect(zipPath, kNxSaveMetaName, summary, err)) return false;
    if (summary.fileCount == 0) {
        err = "backup archive is empty";
        return false;
    }

    // Journal size decides how often writes must be committed. Prefer the live
    // filesystem value, then our own meta, then NACP (device saves report 0).
    s64 journalSize = 0;
    FsSaveDataExtraData extra = {};
    if (R_SUCCEEDED(fsReadSaveDataFileSystemExtraData(
            &extra, sizeof(extra), entry.save_data_id))) {
        journalSize = extra.journal_size;
    }
    if (journalSize <= 0 && hasMeta) journalSize = meta.journal_size;
    if (journalSize <= 0) {
        journalSize = journalFromNacp(entry.application_id, entry.type);
    }

    // Size the save container from the archive contents rather than trusting
    // only our own meta file: JKSV archives have no meta and may be larger.
    s64 needed = static_cast<s64>(summary.totalBytes);
    if (hasMeta && meta.save_data_size > needed) needed = meta.save_data_size;

    const FsSaveDataSpaceId space = static_cast<FsSaveDataSpaceId>(entry.space);
    const FsSaveDataAttribute attr = attrFor(entry);
    FsFileSystem probe = {};
    s64 total = 0;
    bool opened = false;
    bool haveTotal = false;
    if (R_SUCCEEDED(fsOpenSaveDataFileSystem(&probe, space, &attr))) {
        opened = true;
        haveTotal = R_SUCCEEDED(fsFsGetTotalSpace(&probe, "/", &total));
        fsFsClose(&probe);
    }

    const bool needExtend =
        opened && (haveTotal ? needed > total
                             : (hasMeta && meta.save_data_size >
                                               static_cast<s64>(entry.size)));
    if (needExtend) {
        // Headroom keeps a later journal grow from failing the restore.
        constexpr s64 kHeadroom = 5 * 1024 * 1024;
        Result rc = fsExtendSaveDataFileSystem(space, entry.save_data_id,
                                               needed + kHeadroom, journalSize);
        if (R_FAILED(rc)) {
            err = "extend save data failed";
            return false;
        }
    }

    Result rc = mountSave("ssave", entry, false);
    if (R_FAILED(rc)) {
        err = i18n::str().openSaveFailed;
        return false;
    }

    // Device saves (Animal Crossing) may leave every source at zero; commit
    // every 8 MB then so the write transaction never outgrows the journal.
    if (journalSize <= 0) journalSize = 8 * 1024 * 1024;

    const bool cleared = fsx::clearDir("ssave:/", err);
    // Commit the deletions so the freed space is available before the archive
    // is written back.
    if (cleared) fsdevCommitDevice("ssave");

    const bool extracted =
        cleared && zipx::extract(zipPath, "ssave:/", kNxSaveMetaName, journalSize,
                                 [] {
                                     return R_SUCCEEDED(
                                         fsdevCommitDevice("ssave"));
                                 },
                                 err, onProgress);
    if (extracted && R_FAILED(fsdevCommitDevice("ssave"))) {
        err = "final commit failed (save data full?)";
        fsdevUnmountDevice("ssave");
        return false;
    }
    fsdevUnmountDevice("ssave");
    return extracted;
}

bool zipForUpload(const SaveEntry& entry, const std::string& root,
                  std::string& zipPath, std::string& err) {
    const auto files = listBackups(entry, root, false);
    auto newestNormal = [&files](const std::string& owner) -> const BackupFile* {
        const BackupFile* newest = nullptr;
        for (const auto& file : files) {
            if (file.source != "local" || file.autoSnapshot ||
                file.owner != owner) {
                continue;
            }
            if (!newest || newerStamp(file.stamp, newest->stamp)) newest = &file;
        }
        return newest;
    };

    // Prefer the selected user's own newest normal archive. Ownerless archives
    // from older releases remain a fallback, but AUTO snapshots never qualify.
    const BackupFile* newest = newestNormal(ownerName(entry));
    if (!newest) newest = newestNormal("");
    if (newest) {
        zipPath = newest->path;
        return true;
    }
    err = "no local normal backup";
    return false;
}

bool cloudZipForDownload(const SaveEntry& entry,
                         const std::vector<std::string>& names,
                         bool includeLegacy, std::string& name) {
    auto newestNormal = [&names](const std::string& owner,
                                 std::string& selected) {
        bool found = false;
        std::string bestStamp;
        for (const auto& candidate : names) {
            if (candidate.size() < 5 ||
                candidate.substr(candidate.size() - 4) != ".zip") {
                continue;
            }
            const std::string stamp = candidate.substr(0, candidate.size() - 4);
            if (stamp.rfind("AUTO_", 0) == 0 ||
                ownerFromStamp(stamp) != owner) {
                continue;
            }
            if (!found || newerStamp(stamp, bestStamp)) {
                found = true;
                bestStamp = stamp;
                selected = candidate;
            }
        }
        return found;
    };

    if (newestNormal(ownerName(entry), name)) return true;
    // Ownerless archives predate owner prefixes and are plain account saves.
    return includeLegacy && entry.type == FsSaveDataType_Account &&
           newestNormal("", name);
}

bool importZip(const SaveEntry& entry, const std::string& root,
               const std::string& sourceZip, const std::string& stamp,
               std::string& outZip, std::string& err) {
    if (stamp.empty() || stamp.find('/') != std::string::npos ||
        stamp.find("..") != std::string::npos) {
        err = "bad backup name";
        return false;
    }

    // Server error pages can arrive with HTTP 200. Validate the downloaded
    // archive before it is stored as a backup.
    zipx::Summary summary;
    if (!zipx::inspect(sourceZip, kNxSaveMetaName, summary, err)) {
        return false;
    }
    if (summary.fileCount == 0) {
        err = "downloaded file is not a valid backup";
        return false;
    }

    const std::string base = localDir(entry, root);
    if (!fsx::mkdirs(base)) {
        err = "cannot create " + base;
        return false;
    }

    outZip = fsx::join(base, stamp + ".zip");
    for (int suffix = 1; fsx::exists(outZip); ++suffix) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "-%d", suffix);
        outZip = fsx::join(base, stamp + buffer + ".zip");
    }

    if (std::rename(sourceZip.c_str(), outZip.c_str()) != 0) {
        err = "cannot store downloaded backup";
        return false;
    }
    return true;
}

}  // namespace saves
