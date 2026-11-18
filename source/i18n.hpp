#pragma once

#include <string>

struct Strings {
    const char* subtitle;
    const char* cloudOn;
    const char* cloudOff;
    const char* sdOk;
    const char* sdMissing;
    const char* noSaves;
    const char* backupsFmt;  // %d
    const char* noBackup;
    const char* latestFmt;   // %s
    const char* titleIdFmt;  // %s
    const char* hints;
    const char* confirm;
    const char* ready;
    const char* backingUpFmt;  // %d / %d / %s
    const char* savedToFmt;
    const char* backupFailedFmt;
    const char* backupDoneFmt;  // %d / %d
    const char* noLocalBackup;
    const char* restoring;
    const char* restoreFailedFmt;
    const char* restoreDoneFmt;
    const char* needConfig;
    const char* invalidUrl;
    const char* backupFirst;
    const char* packing;
    const char* packFailedFmt;
    const char* uploading;
    const char* uploadedFmt;
    const char* uploadFailedFmt;
    const char* listingRemote;
    const char* listFailedFmt;
    const char* noRemoteBackup;
    const char* downloadingFmt;
    const char* downloadFailedFmt;
    const char* extracting;
    const char* downloadedToFmt;
    const char* extractFailedFmt;
    const char* foundSavesFmt;
    const char* restoreCancelled;
    const char* confirmRestoreStatus;
    const char* tmpDirFailed;
    const char* badRemoteName;
    const char* localLabel;
    const char* pickTitle;
    const char* pickHint;
    const char* pickHintUpload;
    const char* pickHintDownload;
    const char* pickEmpty;
    // Backup deletion.
    const char* deleteConfirm;    // permanent deletion warning
    const char* deletedFmt;       // %s
    const char* deleteFailedFmt;  // %s
    // Menu.
    const char* menuTitle;
    const char* menuSettings;
    const char* menuAbout;
    const char* menuQuit;
    const char* menuHint;
    // Settings.
    const char* settingsTitle;
    const char* settingsUrl;
    const char* settingsUser;
    const char* settingsPass;
    const char* settingsBackupDir;
    const char* settingsHint;
    const char* settingsSaved;
    const char* settingsSaveFailed;  // %s
    const char* notSet;
    const char* guideUrl;
    const char* guideUser;
    const char* guidePass;
    const char* guideBackupDir;
    // About.
    const char* aboutTitle;
    const char* aboutIntro;
    const char* aboutIntro2;
    const char* aboutLibs;
    const char* aboutRepo;
    const char* aboutVersionFmt;  // %s
    const char* aboutAuthorFmt;   // %s
    const char* aboutHint;
    // Confirmation prompts.
    const char* restoreConfirmTitle;
    const char* restoreWarn;
    const char* restoreFlowFmt;  // %s / %s
    const char* backupAllTitle;
    const char* backupAllConfirm;  // %d
    const char* promptHint;
    const char* progressHint;
    // Device save data and single backup confirmation.
    const char* deviceLabel;
    const char* backupOneTitle;  // %s
    const char* backupOneConfirm;
    // Example values shown under the selected setting.
    const char* exampleUrl;
    const char* exampleUser;
    const char* examplePass;
    const char* exampleBackupDir;
    const char* deleteTitle;
    // Automatic pre-restore snapshot and background job progress.
    const char* settingsAutoSnapshot;
    const char* settingsOn;
    const char* settingsOff;
    const char* exampleAutoSnapshot;
    const char* autoLabel;
    const char* snapshotting;
    const char* snapshotFailedFmt;  // %s
    const char* backupTitle;
    const char* uploadTitle;
    const char* downloadTitle;
    const char* backupCancelled;
    const char* uploadCancelled;
    const char* downloadCancelled;
    const char* cancelling;
    const char* cancelHint;
    // Batch upload/download of every game.
    const char* menuUploadAll;
    const char* menuDownloadAll;
    const char* uploadAllMessage;
    const char* downloadAllMessage;
    const char* uploadingAllFmt;    // %d / %d / %s
    const char* downloadingAllFmt;  // %d / %d / %s
    const char* syncDoneFmt;        // %d ok / %d skipped / %d failed
    const char* uploadSkippedExists;
    const char* alreadyLocal;
    const char* noChangeSkip;
    const char* guideNote;
    const char* noteSaved;
    const char* cloudLabel;
    // Backup entries owned by other tools cannot be edited or deleted here.
    const char* readOnlyHint;
    // Settings and localized network / filesystem errors.
    const char* settingsSound;
    const char* exampleSound;
    const char* errNetwork;
    const char* errAuth;
    const char* errForbidden;
    const char* errNotFound;
    const char* errConflict;
    const char* errTimeout;
    const char* errZip;
    const char* errSpace;
    const char* errCancelled;
    // Remote backup deletion.
    const char* deletingRemoteFmt;
    const char* cloudDeleteTitle;
    const char* cloudDeleteConfirm;
    const char* cloudDeletedFmt;
    const char* cloudDeleteFailedFmt;
    const char* deleteCancelled;
    const char* settingsJksv;
    const char* exampleJksv;
    const char* settingsCloudGroup;
    const char* settingsLocalGroup;
    const char* settingsDefaultPath;
};

namespace i18n {

// Reads the console system language once at startup.
void init();

const Strings& str();

std::string format(const char* pattern, ...);

// Maps common internal error strings to the current language while keeping
// the original text for diagnostics.
std::string localizeError(const std::string& err);

}  // namespace i18n
