#pragma once

#include <functional>
#include <string>
#include <vector>

namespace fsx {

std::string join(const std::string& a, const std::string& b);

bool exists(const std::string& path);
bool isDir(const std::string& path);
bool mkdirs(const std::string& path);
bool listDir(const std::string& path, std::vector<std::string>& out);

bool removeTree(const std::string& path, std::string& err);
bool removeFile(const std::string& path, std::string& err);
bool clearDir(const std::string& path, std::string& err);
bool copyTree(const std::string& src, const std::string& dst,
              const std::function<void(const std::string&)>& onFile,
              std::string& err);

}  // namespace fsx
