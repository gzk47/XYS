#include "zipx.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "fsutil.hpp"
#include "miniz.h"

namespace {

void replaceBackslashes(std::string& text) {
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
}

// Drops empty and "." components, rejects ".." and absolute paths.
bool sanitizeName(const std::string& raw, std::string& out) {
    std::string name = raw;
    replaceBackslashes(name);
    if (name.empty() || name[0] == '/') return false;
    if (name.size() >= 2 && name[1] == ':') return false;

    out.clear();
    size_t begin = 0;
    while (begin <= name.size()) {
        const size_t end = name.find('/', begin);
        const std::string part =
            name.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (part == "..") return false;
        if (!part.empty() && part != ".") {
            if (!out.empty()) out.push_back('/');
            out += part;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return !out.empty();
}

// Shared progress state for one pack operation. `total` is the number of
// source bytes below the packed directory, so the bar tracks disk reads rather
// than compressed output.
struct PackProgress {
    const std::function<void(int)>& onProgress;
    const std::function<bool()>& cancelled;
    mz_uint64 total = 0;
    mz_uint64 written = 0;
    int lastPercent = -1;
    bool aborted = false;
};

bool cancelRequested(PackProgress& progress) {
    if (!progress.cancelled || !progress.cancelled()) return false;
    progress.aborted = true;
    return true;
}

void reportProgress(PackProgress& progress) {
    if (!progress.onProgress || progress.total == 0) return;
    int percent = static_cast<int>((progress.written * 100) / progress.total);
    if (percent > 100) percent = 100;
    if (percent == progress.lastPercent) return;
    progress.lastPercent = percent;
    progress.onProgress(percent);
}

mz_uint64 treeSize(const std::string& dir) {
    std::vector<std::string> names;
    if (!fsx::listDir(dir, names)) return 0;

    mz_uint64 total = 0;
    for (const auto& name : names) {
        const std::string path = fsx::join(dir, name);
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            total += treeSize(path);
        } else if (S_ISREG(st.st_mode)) {
            total += static_cast<mz_uint64>(st.st_size);
        }
    }
    return total;
}

struct ReadContext {
    FILE* file;
    PackProgress* progress;
};

// Streams one file into the archive. Reporting from the read callback keeps
// progress moving even when a single save file is larger than 100 MB.
size_t readFileChunk(void* opaque, mz_uint64 fileOfs, void* buffer,
                     size_t size) {
    ReadContext* context = static_cast<ReadContext*>(opaque);
    if (cancelRequested(*context->progress)) return 0;

    if (fseek(context->file, static_cast<long>(fileOfs), SEEK_SET) != 0) {
        return 0;
    }
    const size_t got = fread(buffer, 1, size, context->file);
    context->progress->written += got;
    reportProgress(*context->progress);
    return got;
}

bool addFileStream(mz_zip_archive& archive, const std::string& entryName,
                   const std::string& diskPath, PackProgress& progress) {
    FILE* file = fopen(diskPath.c_str(), "rb");
    if (!file) return false;

    struct stat st;
    mz_uint64 size = 0;
    if (stat(diskPath.c_str(), &st) == 0) {
        size = static_cast<mz_uint64>(st.st_size);
    } else {
        // devoptab mounts normally support stat; fall back to ftell if not.
        if (fseek(file, 0, SEEK_END) != 0) {
            fclose(file);
            return false;
        }
        const long end = ftell(file);
        if (end < 0) {
            fclose(file);
            return false;
        }
        size = static_cast<mz_uint64>(end);
        rewind(file);
    }

    if (size == 0) {
        fclose(file);
        return mz_zip_writer_add_mem(&archive, entryName.c_str(), "", 0,
                                     MZ_NO_COMPRESSION) != 0;
    }

    ReadContext context = {file, &progress};
    const mz_bool ok = mz_zip_writer_add_read_buf_callback(
        &archive, entryName.c_str(), readFileChunk, &context, size, nullptr,
        nullptr, 0, MZ_DEFAULT_COMPRESSION, nullptr, 0, nullptr, 0);
    fclose(file);
    return ok != 0;
}

bool addTree(mz_zip_archive& archive, const std::string& diskDir,
             const std::string& relative, PackProgress& progress,
             std::string& err) {
    if (cancelRequested(progress)) {
        err = "cancelled";
        return false;
    }

    std::vector<std::string> names;
    if (!fsx::listDir(diskDir, names)) {
        err = "cannot list " + diskDir;
        return false;
    }

    for (const auto& name : names) {
        if (cancelRequested(progress)) {
            err = "cancelled";
            return false;
        }

        const std::string diskPath = fsx::join(diskDir, name);
        const std::string entryName =
            relative.empty() ? name : relative + "/" + name;
        if (fsx::isDir(diskPath)) {
            const std::string dirName = entryName + "/";
            if (!mz_zip_writer_add_mem(&archive, dirName.c_str(), "", 0,
                                       MZ_NO_COMPRESSION)) {
                err = "cannot add directory " + entryName;
                return false;
            }
            if (!addTree(archive, diskPath, entryName, progress, err)) {
                return false;
            }
        } else if (!addFileStream(archive, entryName, diskPath, progress)) {
            if (progress.aborted) {
                err = "cancelled";
            } else {
                err = "cannot add file " + entryName;
            }
            return false;
        }
    }
    return true;
}

}  // namespace

bool zipx::create(const std::string& dir, const std::string& zipPath,
                  const std::vector<Extra>& extras, std::string& err,
                  const std::function<void(int)>& onProgress,
                  const std::function<bool()>& cancelled) {
    PackProgress progress{onProgress, cancelled};
    if (onProgress) {
        progress.total = treeSize(dir);
        onProgress(0);
    }
    if (cancelRequested(progress)) {
        err = "cancelled";
        return false;
    }

    mz_zip_archive archive;
    std::memset(&archive, 0, sizeof(archive));
    if (!mz_zip_writer_init_file(&archive, zipPath.c_str(), 0)) {
        err = "cannot create zip " + zipPath;
        return false;
    }

    bool ok = true;
    for (const auto& extra : extras) {
        if (cancelRequested(progress)) {
            err = "cancelled";
            ok = false;
            break;
        }
        if (!mz_zip_writer_add_mem(&archive, extra.name.c_str(),
                                   extra.data.data(), extra.data.size(),
                                   MZ_DEFAULT_COMPRESSION)) {
            err = "cannot add " + extra.name;
            ok = false;
            break;
        }
    }
    if (ok) ok = addTree(archive, dir, "", progress, err);
    if (progress.aborted) {
        err = "cancelled";
        ok = false;
    }

    if (!mz_zip_writer_finalize_archive(&archive)) {
        if (ok) err = "cannot finalize zip";
        ok = false;
    }
    mz_zip_writer_end(&archive);

    if (!ok) {
        std::remove(zipPath.c_str());
    } else if (onProgress) {
        onProgress(100);
    }
    return ok;
}

bool zipx::inspect(const std::string& zipPath, const std::string& skipName,
                   Summary& out, std::string& err) {
    out = Summary{};

    mz_zip_archive archive;
    std::memset(&archive, 0, sizeof(archive));
    if (!mz_zip_reader_init_file(&archive, zipPath.c_str(), 0)) {
        err = "cannot open zip " + zipPath;
        return false;
    }

    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat info;
        if (!mz_zip_reader_file_stat(&archive, i, &info)) {
            err = "cannot read zip entry";
            ok = false;
            break;
        }

        std::string safe;
        if (!sanitizeName(info.m_filename, safe)) {
            err = std::string("unsafe zip entry: ") + info.m_filename;
            ok = false;
            break;
        }
        if (info.m_is_directory) continue;
        if (!skipName.empty() && safe == skipName) continue;

        ++out.fileCount;
        out.totalBytes += info.m_uncomp_size;
    }

    mz_zip_reader_end(&archive);
    return ok;
}

bool zipx::extract(const std::string& zipPath, const std::string& destDir,
                   const std::string& skipName, long long journalSize,
                   const std::function<bool()>& commit, std::string& err,
                   const std::function<void(int)>& onProgress) {
    mz_zip_archive archive;
    std::memset(&archive, 0, sizeof(archive));
    if (!mz_zip_reader_init_file(&archive, zipPath.c_str(), 0)) {
        err = "cannot open zip " + zipPath;
        return false;
    }

    if (!fsx::mkdirs(destDir)) {
        err = "cannot create " + destDir;
        mz_zip_reader_end(&archive);
        return false;
    }

    // Save data filesystems journal writes: only `journalSize` bytes may be
    // written between commits, otherwise writes fail. Commit mid-file when the
    // next chunk would exceed the journal, like JKSV does.
    const bool needCommits = journalSize > 0 && static_cast<bool>(commit);
    size_t chunkSize = 64 * 1024;
    if (needCommits && journalSize < static_cast<long long>(chunkSize)) {
        chunkSize = static_cast<size_t>(journalSize);
    }
    std::vector<unsigned char> buffer(chunkSize);

    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&archive);

    // Total uncompressed bytes, used for the progress percentage.
    mz_uint64 totalBytes = 0;
    if (onProgress) {
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat info;
            if (!mz_zip_reader_file_stat(&archive, i, &info)) continue;
            if (info.m_is_directory) continue;
            std::string safe;
            if (!sanitizeName(info.m_filename, safe)) continue;
            if (!skipName.empty() && safe == skipName) continue;
            totalBytes += info.m_uncomp_size;
        }
    }
    mz_uint64 written = 0;
    int lastPercent = -1;
    if (onProgress) onProgress(0);

    for (mz_uint i = 0; i < count && ok; ++i) {
        mz_zip_archive_file_stat info;
        if (!mz_zip_reader_file_stat(&archive, i, &info)) {
            err = "cannot read zip entry";
            ok = false;
            break;
        }

        std::string safe;
        if (!sanitizeName(info.m_filename, safe)) {
            err = std::string("unsafe zip entry: ") + info.m_filename;
            ok = false;
            break;
        }
        if (!skipName.empty() && safe == skipName) continue;

        std::string full = fsx::join(destDir, safe);
        const bool isDir = info.m_is_directory || safe.back() == '/';
        if (isDir) {
            if (!fsx::mkdirs(full)) {
                err = "cannot create " + full;
                ok = false;
            }
            continue;
        }

        const size_t slash = full.find_last_of('/');
        if (slash != std::string::npos) {
            if (!fsx::mkdirs(full.substr(0, slash))) {
                err = "cannot create " + full.substr(0, slash);
                ok = false;
                break;
            }
        }

        FILE* out = fopen(full.c_str(), "wb");
        if (!out) {
            err = "cannot create " + full + " (" + std::strerror(errno) + ")";
            ok = false;
            break;
        }

        mz_zip_reader_extract_iter_state* iter =
            mz_zip_reader_extract_iter_new(&archive, i, 0);
        if (!iter) {
            fclose(out);
            err = "cannot extract " + safe;
            ok = false;
            break;
        }

        long long journalCount = 0;
        long long total = 0;
        bool fileOk = true;
        for (;;) {
            const size_t got =
                mz_zip_reader_extract_iter_read(iter, buffer.data(), buffer.size());
            if (got == 0) break;

            if (needCommits &&
                journalCount + static_cast<long long>(got) >= journalSize) {
                fflush(out);
                if (!commit()) {
                    err = "commit failed while writing " + safe;
                    fileOk = false;
                    break;
                }
                journalCount = 0;
            }
            if (fwrite(buffer.data(), 1, got, out) != got) {
                err = "cannot extract " + safe + " (" + std::strerror(errno) + ")";
                fileOk = false;
                break;
            }
            journalCount += static_cast<long long>(got);
            total += static_cast<long long>(got);
            written += got;
            if (onProgress && totalBytes > 0) {
                const int percent =
                    static_cast<int>((written * 100) / totalBytes);
                if (percent != lastPercent) {
                    lastPercent = percent;
                    onProgress(percent);
                }
            }
        }
        const bool crcOk = mz_zip_reader_extract_iter_free(iter) != 0;

        if (fileOk && !crcOk) {
            err = "CRC check failed for " + safe;
            fileOk = false;
        }
        if (fileOk && static_cast<mz_uint64>(total) != info.m_uncomp_size) {
            err = "truncated entry " + safe;
            fileOk = false;
        }

        if (fclose(out) != 0 && fileOk) {
            err = "cannot close " + full + " (" + std::strerror(errno) + ")";
            fileOk = false;
        }
        if (!fileOk) {
            ok = false;
            break;
        }
        if (needCommits && !commit()) {
            err = "commit failed after " + safe;
            ok = false;
        }
    }

    if (ok && onProgress && lastPercent < 100) onProgress(100);

    mz_zip_reader_end(&archive);
    return ok;
}

bool zipx::readEntry(const std::string& zipPath, const std::string& name,
                     std::vector<unsigned char>& out, std::string& err) {
    mz_zip_archive archive;
    std::memset(&archive, 0, sizeof(archive));
    if (!mz_zip_reader_init_file(&archive, zipPath.c_str(), 0)) {
        err = "cannot open zip " + zipPath;
        return false;
    }

    const int index = mz_zip_reader_locate_file(&archive, name.c_str(), nullptr, 0);
    if (index < 0) {
        mz_zip_reader_end(&archive);
        err = "missing " + name;
        return false;
    }

    mz_zip_archive_file_stat info;
    if (!mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(index), &info)) {
        mz_zip_reader_end(&archive);
        err = "cannot stat " + name;
        return false;
    }

    out.resize(static_cast<size_t>(info.m_uncomp_size));
    const bool ok =
        mz_zip_reader_extract_to_mem(&archive, static_cast<mz_uint>(index),
                                     out.data(), out.size(), 0);
    mz_zip_reader_end(&archive);
    if (!ok) {
        out.clear();
        err = "cannot read " + name;
    }
    return ok;
}

namespace {

// FNV-1a over metadata; file contents are folded in as a CRC32 so large saves
// stream through a small buffer instead of a byte-wise hash loop.
void hashMix(mz_uint64& hash, const void* data, size_t size) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
}

bool collectTree(const std::string& dir, const std::string& prefix,
                 std::vector<std::pair<std::string, std::string>>& out) {
    std::vector<std::string> names;
    if (!fsx::listDir(dir, names)) return false;
    for (const auto& name : names) {
        const std::string path = fsx::join(dir, name);
        const std::string rel = prefix.empty() ? name : prefix + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!collectTree(path, rel, out)) return false;
        } else if (S_ISREG(st.st_mode)) {
            out.emplace_back(rel, path);
        }
    }
    return true;
}

}  // namespace

bool zipx::hashDir(const std::string& dir, std::string& outHash,
                   std::string& err,
                   const std::function<bool()>& cancelled) {
    std::vector<std::pair<std::string, std::string>> files;
    if (!collectTree(dir, "", files)) {
        err = "cannot list " + dir;
        return false;
    }
    std::sort(files.begin(), files.end());

    mz_uint64 hash = 1469598103934665603ULL;
    for (const auto& item : files) {
        if (cancelled && cancelled()) {
            err = "cancelled";
            return false;
        }
        const std::string& rel = item.first;
        hashMix(hash, rel.data(), rel.size());

        FILE* file = fopen(item.second.c_str(), "rb");
        if (!file) {
            err = "cannot open " + item.second;
            return false;
        }
        mz_ulong crc = MZ_CRC32_INIT;
        mz_uint64 size = 0;
        unsigned char buffer[65536];
        size_t got = 0;
        while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
            crc = mz_crc32(crc, buffer, static_cast<mz_uint>(got));
            size += got;
            if (cancelled && cancelled()) {
                fclose(file);
                err = "cancelled";
                return false;
            }
        }
        const bool readError = ferror(file) != 0;
        fclose(file);
        if (readError) {
            err = "cannot read " + item.second;
            return false;
        }
        hashMix(hash, &crc, sizeof(crc));
        hashMix(hash, &size, sizeof(size));
    }

    char text[17];
    snprintf(text, sizeof(text), "%016llx",
             static_cast<unsigned long long>(hash));
    outHash = text;
    return true;
}
