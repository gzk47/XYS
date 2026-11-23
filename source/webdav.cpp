#include "webdav.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

size_t writeToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

void setupCommon(CURL* handle, const std::string& url, const std::string& userpass) {
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    if (!userpass.empty()) {
        curl_easy_setopt(handle, CURLOPT_USERPWD, userpass.c_str());
        curl_easy_setopt(handle, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
    }
    curl_easy_setopt(handle, CURLOPT_CAINFO, "romfs:/cacert.pem");
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "xys/1.0.0");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 60L);
}

std::string baseName(const std::string& href) {
    std::string value = href;
    while (!value.empty() && value.back() == '/') value.pop_back();
    const size_t slash = value.find_last_of('/');
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

std::string percentDecode(const std::string& input) {
    auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string out;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            const int hi = hexValue(input[i + 1]);
            const int lo = hexValue(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(input[i]);
    }
    return out;
}

void collectHrefs(const std::string& xml, std::vector<std::string>& out) {
    size_t pos = 0;
    while ((pos = xml.find("href", pos)) != std::string::npos) {
        const size_t gt = xml.find('>', pos);
        if (gt == std::string::npos) break;
        const size_t lt = xml.find('<', gt + 1);
        if (lt == std::string::npos) break;
        out.push_back(percentDecode(xml.substr(gt + 1, lt - gt - 1)));
        pos = lt;
    }
}

bool isSuccess(long code) { return code >= 200 && code < 300; }

struct TransferState {
    const std::function<void(int)>* onPercent = nullptr;
    const std::function<bool()>* cancelled = nullptr;
    int lastPercent = -1;
    bool started = false;
};

int progressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                     curl_off_t ultotal, curl_off_t ulnow) {
    TransferState* state = static_cast<TransferState*>(clientp);
    if (state->cancelled && *state->cancelled && (*state->cancelled)()) {
        return 1;  // curl aborts with CURLE_ABORTED_BY_CALLBACK
    }

    // Uploads report ultotal/ulnow, downloads dltotal/dlnow.
    curl_off_t total = dltotal;
    curl_off_t now = dlnow;
    if (ultotal > 0) {
        total = ultotal;
        now = ulnow;
    }
    if (now > 0) state->started = true;

    if (state->onPercent && *state->onPercent && total > 0) {
        int percent = static_cast<int>((now * 100) / total);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        if (percent != state->lastPercent) {
            state->lastPercent = percent;
            (*state->onPercent)(percent);
        }
    }
    return 0;
}

void setupProgress(CURL* handle, TransferState& state,
                   const std::function<void(int)>& onPercent,
                   const std::function<bool()>& cancelled) {
    state.onPercent = onPercent ? &onPercent : nullptr;
    state.cancelled = cancelled ? &cancelled : nullptr;
    if (!state.onPercent && !state.cancelled) return;

    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &state);
}

// Encodes one URL path segment. Backup names contain user nicknames, which may
// include spaces or reserved characters; sending them raw makes libcurl reject
// the URL or silently truncates the path at '#' and '?'.
std::string encodeSegment(const std::string& value) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (const char raw : value) {
        const unsigned char c = static_cast<unsigned char>(raw);
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                           c == '.' || c == '~';
        if (plain) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace

bool WebDav::configure(const Config& cfg) {
    base_ = cfg.baseUrl;
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
    userpass_.clear();
    if (!cfg.username.empty() || !cfg.password.empty()) {
        userpass_ = cfg.username + ":" + cfg.password;
    }
    return !base_.empty();
}

std::string WebDav::collectionUrl(const std::string& appId) const {
    return base_ + "/" + encodeSegment(appId);
}

std::string WebDav::fileUrl(const std::string& appId, const std::string& name) const {
    return base_ + "/" + encodeSegment(appId) + "/" + encodeSegment(name);
}

bool WebDav::listVersions(const std::string& appId, std::vector<std::string>& names,
                          std::string& err,
                          const std::function<bool()>& cancelled,
                          bool* hasIcon) {
    names.clear();
    if (hasIcon) *hasIcon = false;

    CURL* handle = curl_easy_init();
    if (!handle) {
        err = "curl init failed";
        return false;
    }

    static const char* kPropfindBody =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/>"
        "<d:getcontentlength/></d:prop></d:propfind>";

    std::string body;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Depth: 1");
    headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");

    setupCommon(handle, collectionUrl(appId) + "/", userpass_);
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, kPropfindBody);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(strlen(kPropfindBody)));
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
    TransferState state;
    setupProgress(handle, state, {}, cancelled);

    const CURLcode result = curl_easy_perform(handle);
    long code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);

    if (result != CURLE_OK) {
        err = curl_easy_strerror(result);
        return false;
    }
    // A game that was never uploaded has no collection yet. That is an empty
    // version list, not a failure, so batch sync can create it and continue.
    if (code == 404) return true;
    if (code != 207 && code != 200) {
        err = "PROPFIND HTTP " + std::to_string(code);
        return false;
    }

    std::vector<std::string> hrefs;
    collectHrefs(body, hrefs);
    for (const auto& href : hrefs) {
        const std::string name = baseName(href);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0) {
            names.push_back(name);
        } else if (hasIcon && name == "icon.jpg") {
            *hasIcon = true;
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return true;
}

bool WebDav::ensureCollection(const std::string& appId, std::string& err,
                              const std::function<bool()>& cancelled) {
    CURL* handle = curl_easy_init();
    if (!handle) {
        err = "curl init failed";
        return false;
    }

    setupCommon(handle, collectionUrl(appId), userpass_);
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "MKCOL");
    TransferState state;
    setupProgress(handle, state, {}, cancelled);

    const CURLcode result = curl_easy_perform(handle);
    long code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(handle);

    if (result != CURLE_OK) {
        err = curl_easy_strerror(result);
        return false;
    }
    // 405 means the collection already exists on most WebDAV servers.
    if (code == 201 || code == 200 || code == 405) return true;

    err = "MKCOL HTTP " + std::to_string(code);
    return false;
}

bool WebDav::putFile(const std::string& appId, const std::string& name,
                     const std::string& localPath, std::string& err,
                     const std::function<void(int)>& onProgress,
                     const std::function<bool()>& cancelled,
                     bool cleanupPartial) {
    FILE* file = fopen(localPath.c_str(), "rb");
    if (!file) {
        err = "cannot open " + localPath;
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    if (size < 0) {
        fclose(file);
        err = "cannot stat " + localPath;
        return false;
    }

    CURL* handle = curl_easy_init();
    if (!handle) {
        fclose(file);
        err = "curl init failed";
        return false;
    }

    setupCommon(handle, fileUrl(appId, name), userpass_);
    curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(handle, CURLOPT_READDATA, file);
    curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));
    TransferState state;
    setupProgress(handle, state, onProgress, cancelled);

    const CURLcode result = curl_easy_perform(handle);
    long code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(handle);
    fclose(file);

    if (result != CURLE_OK) {
        // A cancelled or interrupted PUT can leave a partial file on the
        // server. Only clean up when bytes were actually sent, so cancelling
        // before the upload starts never deletes a previous good backup.
        if (state.started && cleanupPartial) {
            removeRemote(appId, name);
        }
        err = curl_easy_strerror(result);
        return false;
    }
    if (!isSuccess(code)) {
        err = "PUT HTTP " + std::to_string(code);
        return false;
    }
    return true;
}

bool WebDav::getFile(const std::string& appId, const std::string& name,
                     const std::string& localPath, std::string& err,
                     const std::function<void(int)>& onProgress,
                     const std::function<bool()>& cancelled,
                     bool* notFound) {
    if (notFound) *notFound = false;
    FILE* file = fopen(localPath.c_str(), "wb");
    if (!file) {
        err = "cannot create " + localPath;
        return false;
    }

    CURL* handle = curl_easy_init();
    if (!handle) {
        fclose(file);
        remove(localPath.c_str());
        err = "curl init failed";
        return false;
    }

    setupCommon(handle, fileUrl(appId, name), userpass_);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, file);
    TransferState state;
    setupProgress(handle, state, onProgress, cancelled);

    const CURLcode result = curl_easy_perform(handle);
    long code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(handle);
    fclose(file);

    if (result != CURLE_OK) {
        remove(localPath.c_str());
        err = curl_easy_strerror(result);
        return false;
    }
    if (code != 200) {
        if (notFound && code == 404) *notFound = true;
        remove(localPath.c_str());
        err = "GET HTTP " + std::to_string(code);
        return false;
    }
    return true;
}

bool WebDav::removeVersion(const std::string& appId, const std::string& name,
                           std::string& err,
                           const std::function<bool()>& cancelled) {
    CURL* handle = curl_easy_init();
    if (!handle) {
        err = "curl init failed";
        return false;
    }

    setupCommon(handle, fileUrl(appId, name), userpass_);
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
    TransferState state;
    setupProgress(handle, state, {}, cancelled);

    const CURLcode result = curl_easy_perform(handle);
    long code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(handle);

    if (result == CURLE_ABORTED_BY_CALLBACK || (cancelled && cancelled())) {
        err = "cancelled";
        return false;
    }
    if (result != CURLE_OK) {
        err = std::string("curl: ") + curl_easy_strerror(result);
        return false;
    }
    if (!isSuccess(code)) {
        err = "DELETE HTTP " + std::to_string(code);
        return false;
    }
    return true;
}

bool WebDav::removeRemote(const std::string& appId, const std::string& name) {
    CURL* handle = curl_easy_init();
    if (!handle) return false;

    setupCommon(handle, fileUrl(appId, name), userpass_);
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");

    const CURLcode result = curl_easy_perform(handle);
    long code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(handle);
    return result == CURLE_OK && isSuccess(code);
}
