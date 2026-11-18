#include "config.hpp"

#include <cstdio>
#include <string>

#include "fsutil.hpp"

namespace config {

const char* path() { return "sdmc:/switch/xys/config.ini"; }

namespace {

std::string trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

}  // namespace

Config load() {
    Config cfg;
    FILE* file = fopen(path(), "r");
    if (!file) return cfg;

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        std::string text = trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';') continue;

        size_t eq = text.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(text.substr(0, eq));
        std::string value = trim(text.substr(eq + 1));

        if (key == "base_url") {
            cfg.baseUrl = value;
        } else if (key == "username") {
            cfg.username = value;
        } else if (key == "password") {
            cfg.password = value;
        } else if (key == "backup_dir") {
            cfg.backupDir = value;
        } else if (key == "auto_backup") {
            cfg.autoSnapshot = value != "0" && value != "false";
        } else if (key == "sound") {
            cfg.sound = value != "0" && value != "false";
        } else if (key == "read_jksv") {
            cfg.readJksv = value != "0" && value != "false";
        } else if (key == "keep_backups") {
            cfg.keepBackups = atoi(value.c_str());
        }
    }
    fclose(file);
    return cfg;
}

void ensureTemplate() {
    if (fsx::exists(path())) return;
    fsx::mkdirs("sdmc:/switch/xys");

    FILE* file = fopen(path(), "w");
    if (!file) return;
    fputs("# XYS 配置文件\n", file);
    fputs("# WebDAV 地址：指向一个已存在的目录，路径格式由服务商决定\n", file);
    fputs("# 坚果云示例：https://dav.jianguoyun.com/dav/xys\n", file);
    fputs("# 请先在坚果云中手动创建 xys 文件夹，密码使用第三方应用密码\n", file);
    fputs("base_url=\n", file);
    fputs("username=\n", file);
    fputs("password=\n", file);
    fputs("# 可选：自定义备份根目录，留空则使用 sdmc:/switch/xys/backups\n", file);
    fputs("backup_dir=\n", file);
    fputs("# 恢复前自动生成当前存档快照：1 开启（默认），0 关闭\n", file);
    fputs("auto_backup=1\n", file);
    fputs("# 界面音效：1 开启（默认），0 关闭\n", file);
    fputs("sound=1\n", file);
    fputs("# 读取 JKSV 备份：1 开启（默认），0 关闭\n", file);
    fputs("read_jksv=1\n", file);
    fputs("# 每款游戏最多保留多少份本地备份（0 = 不自动清理）\n", file);
    fputs("keep_backups=10\n", file);
    fclose(file);
}

bool save(const Config& cfg) {
    if (!fsx::mkdirs("sdmc:/switch/xys")) return false;

    FILE* file = fopen(path(), "w");
    if (!file) return false;

    const std::string baseUrl = trim(cfg.baseUrl);
    const std::string username = trim(cfg.username);
    const std::string password = trim(cfg.password);
    const std::string backupDir = trim(cfg.backupDir);

    fputs("# XYS 配置文件\n", file);
    fputs("# WebDAV 地址：指向一个已存在的目录，路径格式由服务商决定\n", file);
    fputs("# 坚果云示例：https://dav.jianguoyun.com/dav/xys\n", file);
    fputs("# 请先在坚果云中手动创建 xys 文件夹，密码使用第三方应用密码\n", file);
    fprintf(file, "base_url=%s\n", baseUrl.c_str());
    fprintf(file, "username=%s\n", username.c_str());
    fprintf(file, "password=%s\n", password.c_str());
    fputs("# 可选：自定义备份根目录，留空则使用 sdmc:/switch/xys/backups\n", file);
    fprintf(file, "backup_dir=%s\n", backupDir.c_str());
    fputs("# 恢复前自动生成当前存档快照：1 开启（默认），0 关闭\n", file);
    fprintf(file, "auto_backup=%d\n", cfg.autoSnapshot ? 1 : 0);
    fputs("# 界面音效：1 开启（默认），0 关闭\n", file);
    fprintf(file, "sound=%d\n", cfg.sound ? 1 : 0);
    fputs("# 读取 JKSV 备份：1 开启（默认），0 关闭\n", file);
    fprintf(file, "read_jksv=%d\n", cfg.readJksv ? 1 : 0);
    fputs("# 每款游戏最多保留多少份本地备份（0 = 不自动清理）\n", file);
    fprintf(file, "keep_backups=%d\n", cfg.keepBackups);
    fclose(file);
    return true;
}

}  // namespace config
