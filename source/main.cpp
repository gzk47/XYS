#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <curl/curl.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "fsutil.hpp"
#include "i18n.hpp"
#include "saves.hpp"
#include "sound.hpp"
#include "ui.hpp"
#include "webdav.hpp"

namespace {

const char* kTmpDir = "sdmc:/switch/xys/tmp";
constexpr int kPageRows = 6;  // visible rows in the lists drawn by ui.cpp

PadState g_pad = {};
Config g_config;
std::string g_backupDir = "sdmc:/switch/xys/backups";
std::vector<SaveEntry> g_saves;
std::vector<BackupInfo> g_backupInfos;
int g_cursor = 0;
std::string g_status;
bool g_hasSd = true;
bool g_picking = false;
int g_pickCursor = 0;
std::string g_pickTitle;
std::vector<saves::BackupFile> g_picks;
bool g_davReady = false;
WebDav g_dav;

// Respects the UI sound switch in the config.
void playSfx(sound::Sfx sfx);

ui::Screen g_screen = ui::Screen::Main;
int g_menuCursor = 0;
int g_fieldCursor = 0;
bool g_quit = false;
bool g_dirty = true;

// Modal confirmations and restore progress.
enum class Prompt {
    None,
    Restore,
    Delete,
    BackupAll,
    BackupOne,
    DeleteCloud,
    UploadAll,
    DownloadAll
};
Prompt g_prompt = Prompt::None;
std::string g_restorePath;
std::string g_cloudDeleteName;
int g_progress = -1;
std::string g_progressTitle;

// Defined below; declared here so job helpers can refresh the UI.
void paint();
void setStatus(const std::string& status);
void reloadSaves();

// Background backup / upload / download. The worker only touches the
// filesystem and the network; SDL and the UI containers stay on the main
// thread.
enum class JobKind { Backup, Upload, Download, CloudList, CloudDelete };

struct Job {
    JobKind kind = JobKind::Backup;
    std::atomic<int> percent{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::mutex mutex;
    std::string status;
    std::string result;
    bool cancelled = false;
    bool ok = false;
    std::vector<std::string> names;  // CloudList results
};

std::unique_ptr<Job> g_job;
std::thread g_jobThread;
int g_lastJobPercent = -1;
u64 g_lastJobPaint = 0;
std::string g_restoreAfterDownload;

void mergeCloudPicks(const std::vector<std::string>& names);
void addCloudPick(const std::string& name);
void addLocalPick(const std::string& path);
void sortPicks();
void runRestore(const std::string& zipPath);

void setJobStatus(Job& job, const std::string& status) {
    std::lock_guard<std::mutex> lock(job.mutex);
    job.status = status;
}

std::string getJobStatus(Job& job) {
    std::lock_guard<std::mutex> lock(job.mutex);
    return job.status;
}

template <typename Work>
void startJob(JobKind kind, Work work) {
    if (g_jobThread.joinable()) g_jobThread.join();
    g_job.reset(new Job());
    g_job->kind = kind;
    Job* job = g_job.get();
    g_lastJobPercent = -1;
    g_lastJobPaint = 0;
    g_jobThread = std::thread([job, work]() {
        work(*job);
        job->done = true;
    });
}

void finishJob() {
    if (!g_job || !g_job->done) return;
    if (g_jobThread.joinable()) g_jobThread.join();

    const JobKind kind = g_job->kind;
    const std::string result = g_job->result;
    const bool cancelled = g_job->cancelled || g_job->cancel.load();
    const bool ok = g_job->ok;
    const std::vector<std::string> names = g_job->names;
    g_job.reset();
    g_lastJobPercent = -1;

    if (kind != JobKind::CloudList) {
        if (cancelled) {
            playSfx(sound::Sfx::Cancel);
        } else if (ok) {
            playSfx(sound::Sfx::Success);
        } else {
            playSfx(sound::Sfx::Error);
        }
    }

    if (kind == JobKind::CloudList) {
        mergeCloudPicks(names);
    } else {
        if (kind == JobKind::CloudDelete && ok && !cancelled && !names.empty()) {
            const std::string path = "cloud:" + names.front();
            g_picks.erase(
                std::remove_if(g_picks.begin(), g_picks.end(),
                               [&path](const saves::BackupFile& pick) {
                                   return pick.path == path;
                               }),
                g_picks.end());
            if (g_picks.empty()) {
                g_picking = false;
            } else if (g_pickCursor >= static_cast<int>(g_picks.size())) {
                g_pickCursor = static_cast<int>(g_picks.size()) - 1;
            }
        }
        // A finished single upload appears immediately as its own cloud row.
        if (kind == JobKind::Upload && ok && !cancelled && g_picking &&
            !names.empty()) {
            addCloudPick(names.front());
            sortPicks();
        }
        // A version downloaded from the picker is now a local backup too.
        if (kind == JobKind::Download && ok && !cancelled && g_picking &&
            !names.empty()) {
            addLocalPick(names.front());
        }
        // The worker never touches the save list, so refresh it here.
        reloadSaves();
    }
    if (!result.empty()) {
        setStatus(result);
    } else {
        paint();
    }

    // A cloud version chosen for restore arrives here once its download
    // finished; the actual restore then runs with the usual snapshot guard.
    if (!g_restoreAfterDownload.empty()) {
        const std::string path = g_restoreAfterDownload;
        g_restoreAfterDownload.clear();
        if (ok && !cancelled) {
            runRestore(path);
            std::remove(path.c_str());
        }
    }
}

ui::State makeState() {
    const Strings& text = i18n::str();

    ui::State state;
    state.cursor = g_cursor;
    state.status = g_status;
    state.sdOk = g_hasSd;
    state.cloud = !g_config.baseUrl.empty();
    state.picking = g_picking;
    state.pickTitle = g_pickTitle;
    state.pickCursor = g_pickCursor;
    state.pickHint = text.pickHint;
    if (g_picking && g_pickCursor >= 0 &&
        g_pickCursor < static_cast<int>(g_picks.size())) {
        const auto& pick = g_picks[g_pickCursor];
        if (pick.source == "local") {
            state.pickHint = text.pickHintUpload;
        } else if (pick.source == "cloud") {
            state.pickHint = text.pickHintDownload;
        }
    }

    state.screen = g_screen;
    state.menuCursor = g_menuCursor;
    state.menuItems = {text.menuSettings, text.menuAbout, text.menuQuit};
    state.fieldCursor = g_fieldCursor;
    state.version = APP_VERSION;
    state.author = APP_AUTHOR;
    state.aboutIconPath = "romfs:/icon.jpg";

    if (g_job) {
        state.progress = g_job->percent.load();
        switch (g_job->kind) {
            case JobKind::Backup:
                state.progressTitle = text.backupTitle;
                break;
            case JobKind::Upload:
                state.progressTitle = text.uploadTitle;
                break;
            case JobKind::CloudList:
                state.progressTitle = text.listingRemote;
                break;
            case JobKind::CloudDelete:
                state.progressTitle = text.cloudDeleteTitle;
                break;
            default:
                state.progressTitle = text.downloadTitle;
                break;
        }
        state.progressStatus =
            g_job->cancel ? text.cancelling : getJobStatus(*g_job);
        state.progressHint = text.cancelHint;
    } else {
        state.progress = g_progress;
        state.progressTitle =
            g_progressTitle.empty() ? text.restoring : g_progressTitle;
        state.progressHint = text.progressHint;
    }
    if (g_prompt != Prompt::None) {
        state.prompt = true;
        state.promptHint = text.promptHint;
        if (g_prompt == Prompt::Restore) {
            state.promptTitle = text.restoreConfirmTitle;
            std::string source;
            if (g_pickCursor >= 0 &&
                g_pickCursor < static_cast<int>(g_picks.size())) {
                const auto& pick = g_picks[g_pickCursor];
                source = pick.source == "local"    ? text.localLabel
                         : pick.source == "cloud"  ? text.cloudLabel
                                                   : pick.source;
                if (!pick.owner.empty()) source += " · " + pick.owner;
                if (pick.autoSnapshot) source += " · " + std::string(text.autoLabel);
            }
            std::string target = text.deviceLabel;
            if (g_cursor >= 0 && g_cursor < static_cast<int>(g_saves.size()) &&
                !g_saves[g_cursor].user.empty()) {
                target = g_saves[g_cursor].user;
            }
            state.promptMessage =
                i18n::format(text.restoreFlowFmt, source.c_str(), target.c_str()) +
                "\n" + text.restoreWarn;
        } else if (g_prompt == Prompt::Delete) {
            state.promptTitle = text.deleteTitle;
            state.promptMessage = text.deleteConfirm;
        } else if (g_prompt == Prompt::DeleteCloud) {
            state.promptTitle = text.cloudDeleteTitle;
            state.promptMessage = text.cloudDeleteConfirm;
        } else if (g_prompt == Prompt::BackupAll) {
            state.promptTitle = text.backupAllTitle;
            state.promptMessage = i18n::format(
                text.backupAllConfirm, static_cast<int>(g_saves.size()));
        } else if (g_prompt == Prompt::UploadAll) {
            state.promptTitle = text.menuUploadAll;
            state.promptMessage = text.uploadAllMessage;
        } else if (g_prompt == Prompt::DownloadAll) {
            state.promptTitle = text.menuDownloadAll;
            state.promptMessage = text.downloadAllMessage;
        } else {
            std::string label;
            if (g_cursor < static_cast<int>(g_saves.size())) {
                const SaveEntry& entry = g_saves[g_cursor];
                label = entry.title + " · " +
                        (entry.user.empty() ? text.deviceLabel : entry.user);
            }
            state.promptTitle =
                i18n::format(text.backupOneTitle, label.c_str());
            state.promptMessage = text.backupOneConfirm;
        }
    }

    switch (g_fieldCursor) {
        case 0:
            state.fieldExample = text.exampleUrl;
            break;
        case 1:
            state.fieldExample = text.exampleUser;
            break;
        case 2:
            state.fieldExample = text.examplePass;
            break;
        case 3:
            state.fieldExample = text.exampleBackupDir;
            break;
        case 4:
            state.fieldExample = text.exampleAutoSnapshot;
            break;
        case 5:
            state.fieldExample = text.exampleSound;
            break;
        default:
            state.fieldExample = text.exampleJksv;
            break;
    }

    state.fields = {
        {text.settingsUrl, g_config.baseUrl, false},
        {text.settingsUser, g_config.username, false},
        {text.settingsPass, g_config.password, true},
        {text.settingsBackupDir, g_config.backupDir, false,
         text.settingsDefaultPath},
        {text.settingsAutoSnapshot,
         g_config.autoSnapshot ? text.settingsOn : text.settingsOff, false},
        {text.settingsSound,
         g_config.sound ? text.settingsOn : text.settingsOff, false},
        {text.settingsJksv,
         g_config.readJksv ? text.settingsOn : text.settingsOff, false},
    };

    state.rows.reserve(g_saves.size());

    state.picks.reserve(g_picks.size());
    for (const auto& pick : g_picks) {
        ui::PickRow row;
        row.time = pick.human.empty() ? pick.stamp : pick.human;
        if (!pick.note.empty()) row.time += " · " + pick.note;
        if (pick.source == "local") {
            row.source = text.localLabel;
        } else if (pick.source == "cloud") {
            row.source = text.cloudLabel;
        } else {
            row.source = pick.source;
        }
        if (!pick.owner.empty()) row.source += " · " + pick.owner;
        if (pick.autoSnapshot) row.source += " · " + std::string(text.autoLabel);
        state.picks.push_back(row);
    }

    for (size_t i = 0; i < g_saves.size(); ++i) {
        const SaveEntry& entry = g_saves[i];
        ui::Row row;
        row.appId = entry.application_id;
        row.title = entry.title;
        row.user = entry.user.empty() ? text.deviceLabel : entry.user;
        row.sizeMb = static_cast<double>(entry.size) / (1024.0 * 1024.0);
        if (i < g_backupInfos.size()) {
            row.backups = g_backupInfos[i].count;
            row.latest = g_backupInfos[i].latestHuman;
            row.iconPath = g_backupInfos[i].iconPath;
        }
        state.rows.push_back(row);
    }
    return state;
}

void paint() {
    ui::draw(makeState());
    g_dirty = false;
}

void setStatus(const std::string& status) {
    g_status = status;
    paint();
}

void playSfx(sound::Sfx sfx) {
    if (g_config.sound) sound::play(sfx);
}

void reloadSaves() {
    g_saves = saves::list();
    g_backupInfos.clear();
    g_backupInfos.reserve(g_saves.size());
    for (const auto& entry : g_saves) {
        g_backupInfos.push_back(saves::backupInfo(entry, g_backupDir));
    }
    ui::invalidateIcons();
    if (g_saves.empty()) {
        g_cursor = 0;
    } else if (g_cursor >= static_cast<int>(g_saves.size())) {
        g_cursor = static_cast<int>(g_saves.size()) - 1;
    }
}

bool ensureCloud() {
    const Strings& text = i18n::str();
    if (g_davReady) return true;
    if (g_config.baseUrl.empty()) {
        setStatus(text.needConfig);
        return false;
    }
    if (!g_dav.configure(g_config)) {
        setStatus(text.invalidUrl);
        return false;
    }
    g_davReady = true;
    return true;
}

void runBackup(bool all) {
    const Strings& text = i18n::str();
    if (g_saves.empty()) {
        setStatus(text.noSaves);
        return;
    }

    std::vector<SaveEntry> entries;
    if (all) {
        entries = g_saves;
    } else {
        entries.push_back(g_saves[g_cursor]);
    }
    const int count = static_cast<int>(entries.size());
    const std::string root = g_backupDir;

    const int keep = g_config.keepBackups;
    startJob(JobKind::Backup, [entries, root, count, keep](Job& job) {
        const Strings& text = i18n::str();
        int succeeded = 0;
        int skipped = 0;
        std::string lastError;

        for (int i = 0; i < count; ++i) {
            if (job.cancel.load()) break;

            setJobStatus(
                job, i18n::format(text.backingUpFmt, i + 1, count,
                                  entries[i].title.c_str()));
            std::string outZip;
            std::string err;
            bool unchanged = false;
            const bool ok = saves::backup(
                entries[i], root, outZip, err,
                [&job, i, count](int percent) {
                    if (percent < 0) percent = 0;
                    if (percent > 100) percent = 100;
                    job.percent = (i * 100 + percent) / count;
                },
                [&job]() { return job.cancel.load(); }, "", &unchanged);
            if (ok && unchanged) {
                ++skipped;
            } else if (ok) {
                ++succeeded;
            } else {
                lastError = err;
            }
            if (ok) saves::pruneBackups(entries[i], root, keep);
        }

        if (job.cancel.load()) {
            job.cancelled = true;
            job.result = text.backupCancelled;
            return;
        }

        job.percent = 100;
        const int failed = count - succeeded - skipped;
        if (count == 1) {
            if (skipped == 1) {
                job.ok = true;
                job.result = text.noChangeSkip;
            } else if (succeeded == 0) {
                job.result =
                    i18n::format(text.backupFailedFmt, i18n::localizeError(lastError).c_str());
            } else {
                job.ok = true;
                job.result = i18n::format(text.backupDoneFmt, succeeded, count);
            }
        } else {
            job.ok = failed == 0;
            job.result = i18n::format(text.syncDoneFmt, succeeded, skipped, failed);
        }
    });
}

void runRestore(const std::string& zipPath) {
    const Strings& text = i18n::str();
    if (g_saves.empty()) {
        setStatus(text.noSaves);
        return;
    }
    const SaveEntry& entry = g_saves[g_cursor];

    // Keep a snapshot of the current save before the archive is extracted over
    // it. When the snapshot fails the restore is aborted so no data is lost.
    if (g_config.autoSnapshot) {
        g_progress = 0;
        g_progressTitle = text.snapshotting;
        setStatus(text.snapshotting);

        std::string snapshotZip;
        std::string snapshotErr;
        int lastSnapshotPercent = -1;
        const bool saved = saves::backup(
            entry, g_backupDir, snapshotZip, snapshotErr,
            [&lastSnapshotPercent](int percent) {
                if (percent == lastSnapshotPercent) return;
                lastSnapshotPercent = percent;
                g_progress = percent;
                paint();
            },
            {}, "AUTO_");
        if (!saved) {
            g_progress = -1;
            g_progressTitle.clear();
            setStatus(
                i18n::format(text.snapshotFailedFmt, i18n::localizeError(snapshotErr).c_str()));
            return;
        }
        saves::pruneBackups(entry, g_backupDir, g_config.keepBackups);
    }

    g_progressTitle = text.restoring;
    g_progress = 0;
    setStatus(text.restoring);
    std::string err;
    int lastPercent = -1;
    const bool restored = saves::restore(
        entry, zipPath, err, [&lastPercent](int percent) {
            if (percent == lastPercent) return;
            lastPercent = percent;
            g_progress = percent;
            paint();
        });
    g_progress = -1;
    g_progressTitle.clear();
    if (!restored) {
        // The automatic snapshot may have been written before the failure.
        reloadSaves();
        playSfx(sound::Sfx::Error);
        setStatus(i18n::format(text.restoreFailedFmt, i18n::localizeError(err).c_str()));
        return;
    }
    reloadSaves();
    playSfx(sound::Sfx::Success);
    const std::string name = zipPath.substr(zipPath.find_last_of('/') + 1);
    setStatus(i18n::format(text.restoreDoneFmt, name.c_str()));
}

// Keeps every remote version as its own row: the list has to make clear which
// copy a button acts on, since "-" uploads a local row and downloads a cloud
// row. The source column tells them apart when the stamp is identical.
void sortPicks() {
    std::sort(g_picks.begin(), g_picks.end(),
              [](const saves::BackupFile& lhs, const saves::BackupFile& rhs) {
                  const std::string lt =
                      lhs.human.empty() ? lhs.stamp : lhs.human;
                  const std::string rt =
                      rhs.human.empty() ? rhs.stamp : rhs.human;
                  if (lt != rt) return lt > rt;
                  if (lhs.source != rhs.source) {
                      return lhs.source == "local";
                  }
                  return lhs.path > rhs.path;
              });
}

void addCloudPick(const std::string& name) {
    if (name.size() < 5 || name.substr(name.size() - 4) != ".zip") return;
    const std::string stamp = name.substr(0, name.size() - 4);
    if (stamp.empty() || stamp.find('/') != std::string::npos ||
        stamp.find("..") != std::string::npos) {
        return;
    }

    bool listed = false;
    for (const auto& pick : g_picks) {
        if (pick.source == "cloud" && pick.stamp == stamp) {
            listed = true;
            break;
        }
    }
    if (listed) return;

    saves::BackupFile file;
    file.path = "cloud:" + name;
    file.stamp = stamp;
    file.human = saves::humanStamp(stamp);
    file.source = "cloud";
    file.autoSnapshot = stamp.rfind("AUTO_", 0) == 0;
    g_picks.push_back(file);
}

// Adds the local row for a freshly downloaded archive without hiding the
// cloud row it came from.
void addLocalPick(const std::string& path) {
    const std::string base = path.substr(path.find_last_of('/') + 1);
    if (base.size() < 5) return;
    const std::string stamp = base.substr(0, base.size() - 4);

    for (const auto& pick : g_picks) {
        if (pick.source == "local" && pick.stamp == stamp) return;
    }

    saves::BackupFile file;
    file.path = path;
    file.stamp = stamp;
    file.human = saves::humanStamp(stamp);
    file.source = "local";
    file.autoSnapshot = stamp.rfind("AUTO_", 0) == 0;
    g_picks.push_back(file);
    sortPicks();
}

void mergeCloudPicks(const std::vector<std::string>& names) {
    for (const auto& name : names) addCloudPick(name);
    sortPicks();
}

void openPicker() {
    const Strings& text = i18n::str();
    if (g_saves.empty()) {
        setStatus(text.noSaves);
        return;
    }
    const SaveEntry entry = g_saves[g_cursor];
    g_picks = saves::listBackups(entry, g_backupDir, g_config.readJksv);
    const bool cloud = !g_config.baseUrl.empty() && ensureCloud();
    if (g_picks.empty() && !cloud) {
        setStatus(text.noLocalBackup);
        return;
    }
    g_pickCursor = 0;
    g_pickTitle = entry.title + " · " +
                  (entry.user.empty() ? text.deviceLabel : entry.user);
    g_picking = true;
    setStatus(text.pickHint);

    if (cloud) {
        const std::string appId = saves::hexId(entry.application_id);
        startJob(JobKind::CloudList, [appId](Job& job) {
            const Strings& text = i18n::str();
            setJobStatus(job, text.listingRemote);
            std::string err;
            auto cancelled = [&job]() { return job.cancel.load(); };
            if (!g_dav.listVersions(appId, job.names, err, cancelled)) {
                job.names.clear();
                if (job.cancel.load()) {
                    job.cancelled = true;
                    return;
                }
                // Keep the local list and just report why cloud is missing.
                job.result = i18n::format(text.listFailedFmt, i18n::localizeError(err).c_str());
                return;
            }
            job.ok = true;
        });
    }
}

// Downloads a remote version chosen in the picker, then hands over to the
// normal restore path (including the pre-restore snapshot) in finishJob().
void runCloudDownload(const std::string& name) {
    const Strings& text = i18n::str();
    if (g_saves.empty()) {
        setStatus(text.noSaves);
        return;
    }
    if (!ensureCloud()) return;
    if (!fsx::mkdirs(kTmpDir)) {
        setStatus(text.tmpDirFailed);
        return;
    }
    const SaveEntry entry = g_saves[g_cursor];
    const std::string appId = saves::hexId(entry.application_id);
    const std::string tmp = std::string(kTmpDir) + "/" + appId + "-restore.zip";
    g_restoreAfterDownload = tmp;
    startJob(JobKind::Download, [appId, name, tmp](Job& job) {
        const Strings& text = i18n::str();
        setJobStatus(job, i18n::format(text.downloadingFmt, name.c_str()));
        std::string err;
        auto cancelled = [&job]() { return job.cancel.load(); };
        if (!g_dav.getFile(appId, name, tmp, err,
                           [&job](int p) { job.percent = p; }, cancelled)) {
            remove(tmp.c_str());
            if (job.cancel.load()) {
                job.cancelled = true;
                job.result = text.downloadCancelled;
            } else {
                job.result = i18n::format(text.downloadFailedFmt, i18n::localizeError(err).c_str());
            }
            return;
        }
        job.percent = 100;
        job.ok = true;
    });
}

// Downloads one remote version chosen in the picker into the local backup
// library without restoring it. finishJob() turns the row into a local one.
void runCloudDownloadToLocal(const std::string& name) {
    const Strings& text = i18n::str();
    if (g_saves.empty()) {
        setStatus(text.noSaves);
        return;
    }
    if (name.size() < 5 || name.substr(name.size() - 4) != ".zip") {
        setStatus(text.badRemoteName);
        return;
    }
    if (!ensureCloud()) return;
    if (!fsx::mkdirs(kTmpDir)) {
        setStatus(text.tmpDirFailed);
        return;
    }

    const SaveEntry entry = g_saves[g_cursor];
    const std::string appId = saves::hexId(entry.application_id);
    const std::string stamp = name.substr(0, name.size() - 4);
    const std::string tmp = std::string(kTmpDir) + "/" + appId + "-save.zip";
    const std::string root = g_backupDir;

    for (const auto& file : saves::listBackups(entry, root, false)) {
        if (file.source == "local" && file.stamp == stamp) {
            setStatus(text.alreadyLocal);
            return;
        }
    }

    // Close the list so the progress overlay and the result are visible.
    g_picking = false;
    startJob(JobKind::Download, [entry, appId, name, stamp, tmp, root](Job& job) {
        const Strings& text = i18n::str();
        auto cancelled = [&job]() { return job.cancel.load(); };
        setJobStatus(job, i18n::format(text.downloadingFmt, name.c_str()));

        std::string err;
        if (!g_dav.getFile(appId, name, tmp, err,
                           [&job](int p) { job.percent = p / 2; }, cancelled)) {
            remove(tmp.c_str());
            if (job.cancel.load()) {
                job.cancelled = true;
                job.result = text.downloadCancelled;
            } else {
                job.result = i18n::format(text.downloadFailedFmt,
                                          i18n::localizeError(err).c_str());
            }
            return;
        }

        setJobStatus(job, text.extracting);
        std::string outZip;
        const bool ok = saves::importZip(entry, root, tmp, stamp, outZip, err);
        remove(tmp.c_str());
        if (!ok) {
            job.result = i18n::format(text.extractFailedFmt,
                                      i18n::localizeError(err).c_str());
            return;
        }
        job.percent = 100;
        job.ok = true;
        job.names.push_back(outZip);
        job.result = i18n::format(text.downloadedToFmt, outZip.c_str());
    });
}

void deleteSelectedBackup() {
    const Strings& text = i18n::str();
    if (g_picks.empty()) return;

    const std::string path = g_picks[g_pickCursor].path;
    const std::string name = path.substr(path.find_last_of('/') + 1);
    g_prompt = Prompt::None;

    std::string err;
    if (!fsx::removeFile(path, err)) {
        setStatus(i18n::format(text.deleteFailedFmt, i18n::localizeError(err).c_str()));
        return;
    }
    std::remove(path.c_str());
    saves::removeMeta(path);

    // Keep the remote versions already merged into the picker: listBackups()
    // only knows local and JKSV archives.
    std::vector<std::string> cloudNames;
    for (const auto& pick : g_picks) {
        if (pick.source == "cloud" && pick.path.rfind("cloud:", 0) == 0) {
            cloudNames.push_back(pick.path.substr(6));
        }
    }

    g_picks = saves::listBackups(g_saves[g_cursor], g_backupDir,
                                 g_config.readJksv);
    mergeCloudPicks(cloudNames);
    if (g_picks.empty()) {
        g_picking = false;
    } else if (g_pickCursor >= static_cast<int>(g_picks.size())) {
        g_pickCursor = static_cast<int>(g_picks.size()) - 1;
    }
    reloadSaves();
    setStatus(i18n::format(text.deletedFmt, name.c_str()));
}

// Deletes the selected remote version. The picker row is removed in
// finishJob() so the worker never touches UI containers.
void runCloudDelete(const std::string& name) {
    const Strings& text = i18n::str();
    if (g_saves.empty()) {
        setStatus(text.noSaves);
        return;
    }
    if (!ensureCloud()) return;

    const std::string appId = saves::hexId(g_saves[g_cursor].application_id);
    startJob(JobKind::CloudDelete, [appId, name](Job& job) {
        const Strings& text = i18n::str();
        setJobStatus(job, i18n::format(text.deletingRemoteFmt, name.c_str()));
        std::string err;
        auto cancelled = [&job]() { return job.cancel.load(); };
        if (!g_dav.removeVersion(appId, name, err, cancelled)) {
            if (job.cancel.load()) {
                job.cancelled = true;
                job.result = text.deleteCancelled;
                return;
            }
            job.result =
                i18n::format(text.cloudDeleteFailedFmt,
                             i18n::localizeError(err).c_str());
            return;
        }
        job.names.push_back(name);
        job.percent = 100;
        job.ok = true;
        job.result = i18n::format(text.cloudDeletedFmt, name.c_str());
    });
}

void applyBackupDir() {
    g_backupDir = "sdmc:/switch/xys/backups";
    std::string dir = g_config.backupDir;
    if (dir.empty()) return;
    if (dir.find(':') == std::string::npos) {
        dir = (dir[0] == '/') ? "sdmc:" + dir : "sdmc:/" + dir;
    }
    while (dir.size() > 6 && dir.back() == '/') {
        dir.pop_back();
    }
    g_backupDir = dir;
}

std::string stripBlank(const std::string& s) {
    const size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string askText(const std::string& guide, const std::string& initial,
                    bool password) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return initial;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, guide.c_str());
    if (!initial.empty()) swkbdConfigSetInitialText(&kbd, initial.c_str());
    if (password) swkbdConfigSetPasswordFlag(&kbd, true);

    char out[512] = {0};
    const Result rc = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return initial;
    return stripBlank(std::string(out));
}

void editSetting() {
    const Strings& text = i18n::str();

    // The snapshot, sound and JKSV switches have no text value: A toggles them.
    if (g_fieldCursor >= 4 && g_fieldCursor <= 6) {
        if (g_fieldCursor == 4) {
            g_config.autoSnapshot = !g_config.autoSnapshot;
        } else if (g_fieldCursor == 5) {
            g_config.sound = !g_config.sound;
        } else {
            g_config.readJksv = !g_config.readJksv;
        }
        if (!config::save(g_config)) {
            setStatus(i18n::format(text.settingsSaveFailed, config::path()));
            return;
        }
        setStatus(text.settingsSaved);
        return;
    }

    if (g_fieldCursor < 0 || g_fieldCursor > 3) return;

    std::string guide;
    std::string* target = nullptr;
    bool password = false;

    switch (g_fieldCursor) {
        case 0:
            guide = std::string(text.guideUrl) + "\n" + text.exampleUrl;
            target = &g_config.baseUrl;
            break;
        case 1:
            guide = std::string(text.guideUser) + "\n" + text.exampleUser;
            target = &g_config.username;
            break;
        case 2:
            guide = std::string(text.guidePass) + "\n" + text.examplePass;
            target = &g_config.password;
            password = true;
            break;
        default:
            guide = std::string(text.guideBackupDir) + "\n" + text.exampleBackupDir;
            target = &g_config.backupDir;
            break;
    }

    const std::string value = askText(guide, *target, password);
    if (value == *target) {
        paint();
        return;
    }
    *target = value;

    if (!config::save(g_config)) {
        setStatus(i18n::format(text.settingsSaveFailed, config::path()));
        return;
    }

    g_davReady = false;
    applyBackupDir();
    reloadSaves();
    setStatus(text.settingsSaved);
}

// Edits the note of the selected backup. Notes live in the per-title
// meta.txt; an empty note removes the entry when no hash is stored.
void editBackupNote() {
    const Strings& text = i18n::str();
    if (g_picks.empty()) return;
    auto& pick = g_picks[g_pickCursor];
    const std::string value = askText(text.guideNote, pick.note, false);
    if (value == pick.note) {
        paint();
        return;
    }
    pick.note = value;
    saves::setNote(pick.path, value);
    setStatus(text.noteSaved);
}

// Uploads one archive. The file name is reported back through job.names so
// finishJob() can mark the matching picker row as synced.
void startUploadJob(const std::string& appId, const std::string& name,
                    const std::string& zipPath) {
    startJob(JobKind::Upload, [appId, name, zipPath](Job& job) {
        const Strings& text = i18n::str();
        setJobStatus(job, text.uploading);
        std::string err;

        auto progress = [&job](int percent) { job.percent = percent; };
        auto cancelled = [&job]() { return job.cancel.load(); };

        // Skip when this exact archive is already on the server.
        std::vector<std::string> names;
        if (g_dav.listVersions(appId, names, err, cancelled) &&
            std::find(names.begin(), names.end(), name) != names.end()) {
            job.percent = 100;
            job.ok = true;
            job.names.push_back(name);
            job.result = text.uploadSkippedExists;
            return;
        }

        bool ok = g_dav.ensureCollection(appId, err, cancelled);
        if (ok) {
            ok = g_dav.putFile(appId, name, zipPath, err, progress, cancelled);
        }

        if (job.cancel.load()) {
            job.cancelled = true;
            job.result = text.uploadCancelled;
            return;
        }
        if (ok) {
            job.percent = 100;
            job.names.push_back(name);
        }
        job.ok = ok;
        job.result = ok ? i18n::format(text.uploadedFmt, name.c_str())
                        : i18n::format(text.uploadFailedFmt, i18n::localizeError(err).c_str());
    });
}

// Batch upload: uploads the newest local archive of every game. Games without
// a local backup are skipped instead of being backed up implicitly.
// Single-archive uploads start from the picker instead.
void runUpload() {
    const Strings& text = i18n::str();
    if (g_saves.empty()) {
        setStatus(text.noSaves);
        return;
    }
    if (!ensureCloud()) return;

    const std::string root = g_backupDir;
    const std::vector<SaveEntry> entries = g_saves;
    startJob(JobKind::Upload, [entries, root](Job& job) {
        const Strings& text = i18n::str();
        auto cancelled = [&job]() { return job.cancel.load(); };
        int done = 0;
        int skipped = 0;
        int failed = 0;
        std::string lastError;
        const int count = static_cast<int>(entries.size());

        for (int i = 0; i < count && !job.cancel.load(); ++i) {
            const SaveEntry& entry = entries[i];
            const std::string appId = saves::hexId(entry.application_id);
            setJobStatus(job, i18n::format(text.uploadingAllFmt, i + 1, count,
                                           entry.title.c_str()));
            job.percent = i * 100 / count;
            std::string err;
            std::string zipPath;
            if (!saves::zipForUpload(entry, root, zipPath, err)) {
                ++skipped;
                continue;
            }
            if (job.cancel.load()) break;
            const std::string name = zipPath.substr(zipPath.find_last_of('/') + 1);

            std::vector<std::string> names;
            if (!g_dav.listVersions(appId, names, err, cancelled)) {
                if (job.cancel.load()) break;
                ++failed;
                lastError = err;
                continue;
            }
            if (std::find(names.begin(), names.end(), name) != names.end()) {
                ++skipped;
                continue;
            }

            bool ok = g_dav.ensureCollection(appId, err, cancelled);
            if (ok) {
                ok = g_dav.putFile(appId, name, zipPath, err,
                                   [&job, i, count](int p) {
                                       job.percent =
                                           (i * 100 + 50 + p / 2) / count;
                                   },
                                   cancelled);
            }
            if (job.cancel.load()) break;
            if (ok) {
                ++done;
            } else {
                ++failed;
                lastError = err;
            }
        }

        if (job.cancel.load()) {
            job.cancelled = true;
            job.result = text.uploadCancelled;
            return;
        }
        job.percent = 100;
        job.ok = failed == 0;
        job.result = failed > 0 && done + skipped == 0
                         ? i18n::localizeError(lastError)
                         : i18n::format(text.syncDoneFmt, done, skipped, failed);
    });
}

// Uploads the archive selected in the picker. Remote and JKSV rows are
// read-only here.
void runUploadPick(const saves::BackupFile& pick) {
    const Strings& text = i18n::str();
    if (g_saves.empty()) {
        setStatus(text.noSaves);
        return;
    }
    if (pick.source != "local") {
        playSfx(sound::Sfx::Error);
        setStatus(text.readOnlyHint);
        return;
    }
    if (!ensureCloud()) return;

    const SaveEntry entry = g_saves[g_cursor];
    const std::string name = pick.path.substr(pick.path.find_last_of('/') + 1);
    g_picking = false;
    startUploadJob(saves::hexId(entry.application_id), name, pick.path);
}

void runDownload(bool all = false) {
    const Strings& text = i18n::str();
    if (g_saves.empty()) {
        setStatus(text.noSaves);
        return;
    }
    if (!ensureCloud()) return;

    const std::string root = g_backupDir;
    if (all) {
        // Batch mode: pull the newest remote archive of every game, skipping
        // games whose newest local backup already has the same stamp.
        const std::vector<SaveEntry> entries = g_saves;
        startJob(JobKind::Download, [entries, root](Job& job) {
            const Strings& text = i18n::str();
            auto cancelled = [&job]() { return job.cancel.load(); };
            if (!fsx::mkdirs(kTmpDir)) {
                job.result = text.tmpDirFailed;
                return;
            }
            int done = 0;
            int skipped = 0;
            int failed = 0;
            std::string lastError;
            const int count = static_cast<int>(entries.size());

            for (int i = 0; i < count && !job.cancel.load(); ++i) {
                const SaveEntry& entry = entries[i];
                const std::string appId = saves::hexId(entry.application_id);
                setJobStatus(job, i18n::format(text.downloadingAllFmt, i + 1,
                                               count, entry.title.c_str()));
                job.percent = i * 100 / count;
                std::string err;
                std::vector<std::string> names;
                if (!g_dav.listVersions(appId, names, err, cancelled)) {
                    if (job.cancel.load()) break;
                    ++failed;
                    lastError = err;
                    continue;
                }
                if (names.empty()) {
                    ++skipped;
                    continue;
                }
                const auto latest = std::max_element(
                    names.begin(), names.end(),
                    [](const std::string& lhs, const std::string& rhs) {
                        const std::string leftTime = saves::humanStamp(lhs);
                        const std::string rightTime = saves::humanStamp(rhs);
                        if (leftTime != rightTime) return leftTime < rightTime;
                        return lhs < rhs;
                    });
                const std::string name = *latest;
                if (name.size() < 5 || name.substr(name.size() - 4) != ".zip") {
                    ++failed;
                    lastError = text.badRemoteName;
                    continue;
                }
                const std::string stamp = name.substr(0, name.size() - 4);
                if (stamp.find('/') != std::string::npos ||
                    stamp.find("..") != std::string::npos) {
                    ++failed;
                    lastError = text.badRemoteName;
                    continue;
                }
                // Skip a version the local library already holds, otherwise
                // repeated batch runs would import name-1.zip copies.
                bool haveLocal = false;
                for (const auto& file : saves::listBackups(entry, root, false)) {
                    if (file.source == "local" && file.stamp == stamp) {
                        haveLocal = true;
                        break;
                    }
                }
                if (haveLocal) {
                    ++skipped;
                    continue;
                }

                const std::string tmp =
                    std::string(kTmpDir) + "/" + appId + "-dl.zip";
                if (!g_dav.getFile(appId, name, tmp, err,
                                   [&job, i, count](int p) {
                                       job.percent = (i * 100 + p) / count;
                                   },
                                   cancelled)) {
                    remove(tmp.c_str());
                    if (job.cancel.load()) break;
                    ++failed;
                    lastError = err;
                    continue;
                }
                std::string outZip;
                const bool ok = saves::importZip(entry, root, tmp, stamp, outZip, err);
                remove(tmp.c_str());
                if (ok) {
                    ++done;
                } else {
                    ++failed;
                    lastError = err;
                }
            }

            if (job.cancel.load()) {
                job.cancelled = true;
                job.result = text.downloadCancelled;
                return;
            }
            job.percent = 100;
            job.ok = failed == 0;
            job.result = failed > 0 && done + skipped == 0
                             ? i18n::localizeError(lastError)
                             : i18n::format(text.syncDoneFmt, done, skipped, failed);
        });
        return;
    }

    const SaveEntry entry = g_saves[g_cursor];
    const std::string appId = saves::hexId(entry.application_id);

    startJob(JobKind::Download, [entry, appId, root](Job& job) {
        const Strings& text = i18n::str();
        auto cancelled = [&job]() { return job.cancel.load(); };

        setJobStatus(job, text.listingRemote);
        std::vector<std::string> names;
        std::string err;
        if (!g_dav.listVersions(appId, names, err, cancelled)) {
            job.result = job.cancel.load()
                             ? std::string(text.downloadCancelled)
                             : i18n::format(text.listFailedFmt, i18n::localizeError(err).c_str());
            return;
        }
        if (job.cancel.load()) {
            job.cancelled = true;
            job.result = text.downloadCancelled;
            return;
        }
        if (names.empty()) {
            job.result = text.noRemoteBackup;
            return;
        }

        const auto latest = std::max_element(
            names.begin(), names.end(),
            [](const std::string& lhs, const std::string& rhs) {
                const std::string leftTime = saves::humanStamp(lhs);
                const std::string rightTime = saves::humanStamp(rhs);
                if (leftTime != rightTime) return leftTime < rightTime;
                return lhs < rhs;
            });
        const std::string name = *latest;
        if (name.size() < 5 || name.substr(name.size() - 4) != ".zip") {
            job.result = text.badRemoteName;
            return;
        }

        const std::string stamp = name.substr(0, name.size() - 4);
        if (stamp.find('/') != std::string::npos ||
            stamp.find("..") != std::string::npos) {
            job.result = text.badRemoteName;
            return;
        }

        if (!fsx::mkdirs(kTmpDir)) {
            job.result = text.tmpDirFailed;
            return;
        }
        const std::string tmp = std::string(kTmpDir) + "/" + appId + "-dl.zip";

        setJobStatus(job, i18n::format(text.downloadingFmt, name.c_str()));
        if (!g_dav.getFile(appId, name, tmp, err,
                           [&job](int percent) { job.percent = percent; },
                           cancelled)) {
            remove(tmp.c_str());
            job.result = job.cancel.load()
                             ? std::string(text.downloadCancelled)
                             : i18n::format(text.downloadFailedFmt, i18n::localizeError(err).c_str());
            return;
        }

        setJobStatus(job, text.extracting);
        std::string outZip;
        const bool ok = saves::importZip(entry, root, tmp, stamp, outZip, err);
        remove(tmp.c_str());

        if (!ok) {
            job.result = i18n::format(text.extractFailedFmt, i18n::localizeError(err).c_str());
            return;
        }
        job.percent = 100;
        job.ok = true;
        job.result = i18n::format(text.downloadedToFmt, outZip.c_str());
    });
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    fsInitialize();
    if (R_FAILED(fsdevMountSdmc()) &&
        fsdevGetDeviceFileSystem("sdmc") == nullptr) {
        g_hasSd = false;
    }
    romfsInit();
    if (R_FAILED(accountInitialize(AccountServiceType_Application))) {
        accountInitialize(AccountServiceType_System);
    }
    nsInitialize();
    nifmInitialize(NifmServiceType_User);
    socketInitializeDefault();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    i18n::init();
    config::ensureTemplate();
    g_config = config::load();
    applyBackupDir();

    if (!ui::init("romfs:/font.ttf")) return 1;
    sound::init();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    reloadSaves();
    g_status =
        i18n::format(i18n::str().foundSavesFmt, static_cast<int>(g_saves.size()));

    u64 nextRepeat = 0;
    bool prevUp = false;
    bool prevDown = false;
    bool prevLeft = false;
    bool prevRight = false;
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        const u64 down = padGetButtonsDown(&g_pad);
        const u64 held = padGetButtons(&g_pad);

        // A background job owns the screen: show its progress, let B request a
        // cancel, and ignore every other button until the worker finishes.
        if (g_job) {
            if (down & HidNpadButton_B) {
                g_job->cancel = true;
                playSfx(sound::Sfx::Cancel);
            }

            if (g_job->done) {
                finishJob();
            } else {
                const int percent = g_job->percent.load();
                const u64 tick = SDL_GetTicks64();
                if (percent != g_lastJobPercent ||
                    tick - g_lastJobPaint >= 200 ||
                    (down & HidNpadButton_B) != 0) {
                    g_lastJobPercent = percent;
                    g_lastJobPaint = tick;
                    paint();
                }
            }
            SDL_Delay(1);
            continue;
        }

        const HidAnalogStickState leftStick = padGetStickPos(&g_pad, 0);
        const HidAnalogStickState rightStick = padGetStickPos(&g_pad, 1);
        const int stickY = std::abs(leftStick.y) >= std::abs(rightStick.y)
                               ? leftStick.y
                               : rightStick.y;
        const int stickX = std::abs(leftStick.x) >= std::abs(rightStick.x)
                               ? leftStick.x
                               : rightStick.x;
        const bool upNow = (held & HidNpadButton_Up) != 0 || stickY > 15000;
        const bool downNow = (held & HidNpadButton_Down) != 0 || stickY < -15000;
        const bool leftNow = (held & HidNpadButton_Left) != 0 || stickX < -15000;
        const bool rightNow = (held & HidNpadButton_Right) != 0 || stickX > 15000;

        const u64 now = SDL_GetTicks64();
        int move = 0;
        if ((upNow && !prevUp) || (downNow && !prevDown)) {
            move = upNow ? -1 : 1;
            nextRepeat = now + 200;
        } else if ((upNow || downNow) && now >= nextRepeat) {
            move = downNow ? 1 : -1;
            nextRepeat = now + 48;
        }
        prevUp = upNow;
        prevDown = downNow;

        int page = 0;
        if (leftNow && !prevLeft) {
            page = -kPageRows;
        } else if (rightNow && !prevRight) {
            page = kPageRows;
        }
        prevLeft = leftNow;
        prevRight = rightNow;

        // Rendering a full SDL frame is comparatively expensive on the Switch.
        // Only draw after input or an explicit status/progress update.
        if (down != 0 || move != 0 || page != 0) g_dirty = true;

        const Strings& text = i18n::str();
        if (g_prompt != Prompt::None) {
            if (down & HidNpadButton_A) {
                playSfx(sound::Sfx::Confirm);
                if (g_prompt == Prompt::Restore) {
                    const std::string path = g_restorePath;
                    g_restorePath.clear();
                    g_prompt = Prompt::None;
                    if (path.rfind("cloud:", 0) == 0) {
                        runCloudDownload(path.substr(6));
                    } else {
                        runRestore(path);
                    }
                } else if (g_prompt == Prompt::Delete) {
                    deleteSelectedBackup();
                } else if (g_prompt == Prompt::DeleteCloud) {
                    g_prompt = Prompt::None;
                    runCloudDelete(g_cloudDeleteName);
                } else if (g_prompt == Prompt::BackupAll) {
                    g_prompt = Prompt::None;
                    runBackup(true);
                } else if (g_prompt == Prompt::UploadAll) {
                    g_prompt = Prompt::None;
                    runUpload();
                } else if (g_prompt == Prompt::DownloadAll) {
                    g_prompt = Prompt::None;
                    runDownload(true);
                } else {
                    g_prompt = Prompt::None;
                    runBackup(false);
                }
            } else if (down & HidNpadButton_B) {
                playSfx(sound::Sfx::Cancel);
                if (g_prompt == Prompt::Restore) {
                    // Go back to the list the restore was started from.
                    g_prompt = Prompt::None;
                    g_picking = true;
                    setStatus(text.pickHint);
                } else {
                    g_prompt = Prompt::None;
                    paint();
                }
            }
        } else if (g_screen == ui::Screen::Menu) {
            if (move != 0) {
                playSfx(sound::Sfx::Move);
                g_menuCursor = (g_menuCursor + move + 3) % 3;
            }
            if (down & HidNpadButton_A) {
                playSfx(sound::Sfx::Confirm);
                if (g_menuCursor == 0) {
                    g_screen = ui::Screen::Settings;
                    g_fieldCursor = 0;
                } else if (g_menuCursor == 1) {
                    g_screen = ui::Screen::About;
                } else {
                    g_quit = true;
                }
            } else if (down & HidNpadButton_B) {
                playSfx(sound::Sfx::Cancel);
                g_screen = ui::Screen::Main;
            }
        } else if (g_screen == ui::Screen::Settings) {
            if (move != 0) {
                playSfx(sound::Sfx::Move);
                g_fieldCursor = (g_fieldCursor + move + 7) % 7;
            }
            if (down & HidNpadButton_A) {
                playSfx(sound::Sfx::Confirm);
                editSetting();
            } else if (down & HidNpadButton_B) {
                playSfx(sound::Sfx::Cancel);
                g_screen = ui::Screen::Menu;
            }
        } else if (g_screen == ui::Screen::About) {
            if (down & (HidNpadButton_A | HidNpadButton_B)) {
                playSfx(sound::Sfx::Cancel);
                g_screen = ui::Screen::Menu;
            }
        } else if (g_picking) {
            if ((page != 0 || move != 0) && !g_picks.empty()) {
                playSfx(sound::Sfx::Move);
            }
            if (page != 0 && !g_picks.empty()) {
                const int count = static_cast<int>(g_picks.size());
                g_pickCursor = ((g_pickCursor + page) % count + count) % count;
                setStatus(text.pickHint);
            }
            if (move != 0 && !g_picks.empty()) {
                const int count = static_cast<int>(g_picks.size());
                g_pickCursor = ((g_pickCursor + move) % count + count) % count;
                setStatus(text.pickHint);
            }
            if (down & HidNpadButton_A && !g_picks.empty()) {
                playSfx(sound::Sfx::Confirm);
                g_restorePath = g_picks[g_pickCursor].path;
                g_picking = false;
                g_prompt = Prompt::Restore;
                paint();
            } else if (down & HidNpadButton_X) {
                if (!g_picks.empty()) {
                    const std::string& source = g_picks[g_pickCursor].source;
                    if (source == "JKSV") {
                        playSfx(sound::Sfx::Error);
                        setStatus(text.readOnlyHint);
                    } else if (source == "cloud") {
                        playSfx(sound::Sfx::Confirm);
                        g_cloudDeleteName =
                            g_picks[g_pickCursor].path.substr(6);
                        g_prompt = Prompt::DeleteCloud;
                        paint();
                    } else {
                        playSfx(sound::Sfx::Confirm);
                        g_prompt = Prompt::Delete;
                        paint();
                    }
                }
            } else if (down & HidNpadButton_Y) {
                if (!g_picks.empty()) {
                    if (g_picks[g_pickCursor].source == "local") {
                        playSfx(sound::Sfx::Confirm);
                        editBackupNote();
                    } else {
                        playSfx(sound::Sfx::Error);
                        setStatus(text.readOnlyHint);
                    }
                }
            } else if (down & HidNpadButton_Minus) {
                if (!g_picks.empty()) {
                    const saves::BackupFile& pick = g_picks[g_pickCursor];
                    if (pick.source == "local") {
                        playSfx(sound::Sfx::Confirm);
                        runUploadPick(pick);
                    } else if (pick.source == "cloud") {
                        playSfx(sound::Sfx::Confirm);
                        runCloudDownloadToLocal(pick.stamp + ".zip");
                    } else {
                        playSfx(sound::Sfx::Error);
                        setStatus(text.readOnlyHint);
                    }
                }
            } else if (down & HidNpadButton_B) {
                playSfx(sound::Sfx::Cancel);
                g_picking = false;
                setStatus(text.restoreCancelled);
            }
        } else {
            if ((page != 0 || move != 0) && !g_saves.empty()) {
                playSfx(sound::Sfx::Move);
            }
            if (!g_saves.empty() && page != 0) {
                const int count = static_cast<int>(g_saves.size());
                g_cursor = ((g_cursor + page) % count + count) % count;
            }
            if (move != 0 && !g_saves.empty()) {
                const int count = static_cast<int>(g_saves.size());
                g_cursor = ((g_cursor + move) % count + count) % count;
            }

            if (down & HidNpadButton_A) {
                playSfx(sound::Sfx::Confirm);
                openPicker();
            } else if (down & HidNpadButton_X) {
                playSfx(sound::Sfx::Confirm);
                if (g_saves.empty()) {
                    setStatus(text.noSaves);
                } else {
                    g_prompt = Prompt::BackupOne;
                    paint();
                }
            } else if (down & HidNpadButton_L) {
                playSfx(sound::Sfx::Confirm);
                if (g_saves.empty()) {
                    setStatus(text.noSaves);
                } else {
                    g_prompt = Prompt::BackupAll;
                    paint();
                }
            } else if (down & HidNpadButton_ZL) {
                playSfx(sound::Sfx::Confirm);
                if (g_saves.empty()) {
                    setStatus(text.noSaves);
                } else {
                    g_prompt = Prompt::UploadAll;
                    paint();
                }
            } else if (down & HidNpadButton_ZR) {
                playSfx(sound::Sfx::Confirm);
                if (g_saves.empty()) {
                    setStatus(text.noSaves);
                } else {
                    g_prompt = Prompt::DownloadAll;
                    paint();
                }
            } else if (down & HidNpadButton_R) {
                playSfx(sound::Sfx::Confirm);
                reloadSaves();
                setStatus(i18n::format(text.foundSavesFmt,
                                       static_cast<int>(g_saves.size())));
            } else if (down & HidNpadButton_Plus) {
                playSfx(sound::Sfx::Confirm);
                g_screen = ui::Screen::Menu;
                g_menuCursor = 0;  // land on Settings
            }
        }

        if (g_dirty) paint();
        SDL_Delay(1);
        if (g_quit) break;
    }

    // Never destroy the UI while a worker still holds files or a transfer.
    if (g_job) {
        g_job->cancel = true;
        if (g_jobThread.joinable()) g_jobThread.join();
        g_job.reset();
    }

    sound::shutdown();
    ui::shutdown();
    curl_global_cleanup();
    socketExit();
    nifmExit();
    nsExit();
    accountExit();
    romfsExit();
    fsdevUnmountAll();
    fsExit();
    return 0;
}
