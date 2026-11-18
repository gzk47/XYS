#pragma once

#include <functional>
#include <string>
#include <vector>

#include "config.hpp"

class WebDav {
public:
    bool configure(const Config& cfg);

    // `cancelled` aborts the request when it returns true.
    bool listVersions(const std::string& appId, std::vector<std::string>& names,
                      std::string& err,
                      const std::function<bool()>& cancelled = {});
    bool ensureCollection(const std::string& appId, std::string& err,
                          const std::function<bool()>& cancelled = {});
    // `onProgress` receives 0-100 based on transferred bytes.
    bool putFile(const std::string& appId, const std::string& name,
                 const std::string& localPath, std::string& err,
                 const std::function<void(int)>& onProgress = {},
                 const std::function<bool()>& cancelled = {});
    bool getFile(const std::string& appId, const std::string& name,
                 const std::string& localPath, std::string& err,
                 const std::function<void(int)>& onProgress = {},
                 const std::function<bool()>& cancelled = {});
    // Deletes one remote archive from the backup picker.
    bool removeVersion(const std::string& appId, const std::string& name,
                       std::string& err,
                       const std::function<bool()>& cancelled = {});

private:
    std::string collectionUrl(const std::string& appId) const;
    std::string fileUrl(const std::string& appId, const std::string& name) const;
    // Best-effort DELETE used to clear a cancelled or failed upload.
    bool removeRemote(const std::string& appId, const std::string& name);

    std::string base_;
    std::string userpass_;
};
