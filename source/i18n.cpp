#include "i18n.hpp"

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "i18n_tables.hpp"

namespace i18n {
namespace {

enum class Language {
    English,
    Simplified,
    Traditional,
    Japanese,
    French,
    German,
    Italian,
    Spanish,
    Korean,
    Dutch,
    Portuguese,
    Russian,
};

Language g_language = Language::English;

}  // namespace

void init() {
    g_language = Language::English;

    u64 code = 0;
    SetLanguage language = SetLanguage_ENUS;
    if (R_FAILED(setInitialize())) return;
    if (R_SUCCEEDED(setGetSystemLanguage(&code))) {
        if (R_FAILED(setMakeLanguage(code, &language))) {
            language = SetLanguage_ENUS;
        }
    }
    setExit();

    switch (language) {
        case SetLanguage_JA:
            g_language = Language::Japanese;
            break;
        case SetLanguage_FR:
        case SetLanguage_FRCA:
            g_language = Language::French;
            break;
        case SetLanguage_DE:
            g_language = Language::German;
            break;
        case SetLanguage_IT:
            g_language = Language::Italian;
            break;
        case SetLanguage_ES:
        case SetLanguage_ES419:
            g_language = Language::Spanish;
            break;
        case SetLanguage_KO:
            g_language = Language::Korean;
            break;
        case SetLanguage_NL:
            g_language = Language::Dutch;
            break;
        case SetLanguage_PT:
        case SetLanguage_PTBR:
            g_language = Language::Portuguese;
            break;
        case SetLanguage_RU:
            g_language = Language::Russian;
            break;
        case SetLanguage_ZHCN:
        case SetLanguage_ZHHANS:
            g_language = Language::Simplified;
            break;
        case SetLanguage_ZHTW:
        case SetLanguage_ZHHANT:
            g_language = Language::Traditional;
            break;
        case SetLanguage_ENUS:
        case SetLanguage_ENGB:
        default:
            break;
    }
}

const Strings& str() {
    switch (g_language) {
        case Language::Simplified:
            return kZhHans;
        case Language::Traditional:
            return kZhHant;
        case Language::Japanese:
            return kJa;
        case Language::French:
            return kFr;
        case Language::German:
            return kDe;
        case Language::Italian:
            return kIt;
        case Language::Spanish:
            return kEs;
        case Language::Korean:
            return kKo;
        case Language::Dutch:
            return kNl;
        case Language::Portuguese:
            return kPt;
        case Language::Russian:
            return kRu;
        default:
            return kEn;
    }
}

bool isSimplified() { return g_language == Language::Simplified; }

bool isTraditional() { return g_language == Language::Traditional; }

bool isKorean() { return g_language == Language::Korean; }

std::string format(const char* pattern, ...) {
    char buffer[512];
    va_list args;
    va_start(args, pattern);
    vsnprintf(buffer, sizeof(buffer), pattern, args);
    va_end(args);
    return std::string(buffer);
}

std::string localizeError(const std::string& err) {
    const Strings& text = str();
    const char* label = nullptr;

    if (err.find("HTTP 401") != std::string::npos) {
        label = text.errAuth;
    } else if (err.find("HTTP 403") != std::string::npos) {
        label = text.errForbidden;
    } else if (err.find("HTTP 404") != std::string::npos) {
        label = text.errNotFound;
    } else if (err.find("HTTP 409") != std::string::npos) {
        label = text.errConflict;
    } else if (err.find("HTTP 507") != std::string::npos ||
               err.find("No space") != std::string::npos ||
               err.find("ENOSPC") != std::string::npos) {
        label = text.errSpace;
    } else if (err.find("timed out") != std::string::npos ||
               err.find("Timeout") != std::string::npos ||
               err.find("timeout") != std::string::npos) {
        label = text.errTimeout;
    } else if (err.find("cancelled") != std::string::npos ||
               err.find("canceled") != std::string::npos) {
        label = text.errCancelled;
    } else if (err.find("save data is empty") != std::string::npos) {
        label = text.errEmptySave;
    } else if (err.find("backup type does not match") != std::string::npos) {
        label = text.errTypeMismatch;
    } else if (err.find("backup belongs to a different game") !=
               std::string::npos) {
        label = text.errWrongGame;
    } else if (err.find("backup archive is empty") != std::string::npos) {
        label = text.errEmptyArchive;
    } else if (err.find("extend save data failed") != std::string::npos) {
        label = text.errExtendFailed;
    } else if (err.find("final commit failed") != std::string::npos) {
        label = text.errCommitFailed;
    } else if (err.find("no local normal backup") != std::string::npos) {
        label = text.errNoLocalBackup;
    } else if (err.find("cannot store downloaded backup") !=
               std::string::npos) {
        label = text.errStoreFailed;
    } else if (err.find("cannot open zip") != std::string::npos ||
               err.find("unsafe zip entry") != std::string::npos ||
               err.find("cannot read zip entry") != std::string::npos ||
               err.find("not a valid backup") != std::string::npos ||
               err.find("cannot finalize zip") != std::string::npos) {
        label = text.errZip;
    } else if (err.find("curl") != std::string::npos ||
               err.find("connect") != std::string::npos ||
               err.find("resolve") != std::string::npos) {
        label = text.errNetwork;
    }

    if (!label) return err;
    return std::string(label) + " (" + err + ")";
}

}  // namespace i18n
