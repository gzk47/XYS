#include "fsutil.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fsx {

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/' || a.back() == ':') return a + b;
    return a + "/" + b;
}

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool isDir(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool mkdirs(const std::string& path) {
    if (path.empty()) return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string part =
            path.substr(0, slash == std::string::npos ? path.size() : slash);
        if (!part.empty() && part.back() != ':') {
            struct stat st;
            if (stat(part.c_str(), &st) != 0) {
                if (mkdir(part.c_str(), 0777) != 0 && errno != EEXIST) return false;
            } else if (!S_ISDIR(st.st_mode)) {
                return false;
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool listDir(const std::string& path, std::vector<std::string>& out) {
    out.clear();
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    while (struct dirent* ent = readdir(dir)) {
        if (std::strcmp(ent->d_name, ".") == 0 ||
            std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        out.push_back(ent->d_name);
    }
    closedir(dir);
    return true;
}

bool removeTree(const std::string& path, std::string& err) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return true;

    if (!S_ISDIR(st.st_mode)) {
        if (remove(path.c_str()) != 0 && errno != ENOENT) {
            err = "remove failed: " + path + " (" + std::strerror(errno) + ")";
            return false;
        }
        return true;
    }

    DIR* dir = opendir(path.c_str());
    if (!dir) {
        err = "opendir failed: " + path;
        return false;
    }
    std::vector<std::string> names;
    while (struct dirent* ent = readdir(dir)) {
        if (std::strcmp(ent->d_name, ".") == 0 ||
            std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        names.push_back(ent->d_name);
    }
    closedir(dir);

    for (const auto& name : names) {
        if (!removeTree(join(path, name), err)) return false;
    }
    if (rmdir(path.c_str()) != 0 && errno != ENOENT) {
        err = "rmdir failed: " + path + " (" + std::strerror(errno) + ")";
        return false;
    }
    return true;
}

bool removeFile(const std::string& path, std::string& err) {
    if (remove(path.c_str()) != 0 && errno != ENOENT) {
        err = "remove failed: " + path + " (" + std::strerror(errno) + ")";
        return false;
    }
    return true;
}

bool clearDir(const std::string& path, std::string& err) {
    std::vector<std::string> names;
    if (!listDir(path, names)) return true;
    for (const auto& name : names) {
        if (!removeTree(join(path, name), err)) return false;
    }
    return true;
}

bool copyTree(const std::string& src, const std::string& dst,
              const std::function<void(const std::string&)>& onFile,
              std::string& err) {
    if (!mkdirs(dst)) {
        err = "mkdir failed: " + dst;
        return false;
    }

    DIR* dir = opendir(src.c_str());
    if (!dir) {
        err = "opendir failed: " + src;
        return false;
    }
    std::vector<std::string> names;
    while (struct dirent* ent = readdir(dir)) {
        if (std::strcmp(ent->d_name, ".") == 0 ||
            std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        names.push_back(ent->d_name);
    }
    closedir(dir);

    std::vector<char> buffer(64 * 1024);

    for (const auto& name : names) {
        const std::string from = join(src, name);
        const std::string to = join(dst, name);

        struct stat st;
        if (stat(from.c_str(), &st) != 0) {
            err = "stat failed: " + from;
            return false;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!copyTree(from, to, onFile, err)) return false;
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;

        FILE* in = fopen(from.c_str(), "rb");
        if (!in) {
            err = "open failed: " + from;
            return false;
        }
        FILE* out = fopen(to.c_str(), "wb");
        if (!out) {
            fclose(in);
            err = "create failed: " + to;
            return false;
        }

        bool ok = true;
        size_t got = 0;
        while ((got = fread(buffer.data(), 1, buffer.size(), in)) > 0) {
            if (fwrite(buffer.data(), 1, got, out) != got) {
                ok = false;
                err = "write failed: " + to;
                break;
            }
        }
        if (ok && ferror(in)) {
            ok = false;
            err = "read failed: " + from;
        }
        fclose(out);
        fclose(in);
        if (!ok) return false;
        if (onFile) onFile(to);
    }
    return true;
}

}  // namespace fsx
