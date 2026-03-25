#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstdio>
#include <array>
#include <cstdlib>
#include <algorithm>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace fs = std::filesystem;

// ============================================================
// Types
// ============================================================

struct DriveInfo {
    std::string path;    // e.g. "E:\" on Windows, "/Volumes/NO NAME" on macOS
    std::string label;   // volume label
    uint64_t freeBytes  = 0;
    uint64_t totalBytes = 0;
};

// [section] -> { key -> value }
using IniData = std::map<std::string, std::map<std::string, std::string>>;

// progress: 0.0–1.0, status: short description
using ProgressCallback = std::function<void(float progress, const std::string& status)>;

// ============================================================
// Drive listing
// ============================================================

inline std::vector<DriveInfo> listDrives()
{
    std::vector<DriveInfo> drives;

#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1 << i))) continue;
        std::string path = std::string(1, 'A' + i) + ":\\";
        UINT type = GetDriveTypeA(path.c_str());
        if (type != DRIVE_REMOVABLE && type != DRIVE_FIXED) continue;

        DriveInfo d;
        d.path = path;
        char label[256] = {};
        GetVolumeInformationA(path.c_str(), label, sizeof(label),
                              nullptr, nullptr, nullptr, nullptr, 0);
        d.label = label[0] ? label : path;

        ULARGE_INTEGER free, total;
        if (GetDiskFreeSpaceExA(path.c_str(), &free, &total, nullptr)) {
            d.freeBytes  = free.QuadPart;
            d.totalBytes = total.QuadPart;
        }
        drives.push_back(d);
    }

#elif defined(__APPLE__)
    std::error_code ec;
    for (auto& entry : fs::directory_iterator("/Volumes", ec)) {
        DriveInfo d;
        d.path  = entry.path().string();
        d.label = entry.path().filename().string();
        auto si = fs::space(d.path, ec);
        if (!ec) { d.freeBytes = si.available; d.totalBytes = si.capacity; }
        drives.push_back(d);
    }

#else // Linux
    auto tryDir = [&](const std::string& base) {
        std::error_code ec;
        if (!fs::exists(base, ec)) return;
        for (auto& entry : fs::directory_iterator(base, ec)) {
            DriveInfo d;
            d.path  = entry.path().string();
            d.label = entry.path().filename().string();
            auto si = fs::space(d.path, ec);
            if (!ec) { d.freeBytes = si.available; d.totalBytes = si.capacity; }
            drives.push_back(d);
        }
    };
    if (const char* user = getenv("USER")) tryDir(std::string("/media/") + user);
    tryDir("/run/media");
    tryDir("/mnt");
#endif

    return drives;
}

// ============================================================
// File / directory helpers
// ============================================================

// Create a directory and all parents; throws on failure.
inline void makeDir(const fs::path& path)
{
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) throw std::runtime_error("makeDir failed: " + path.string() + " — " + ec.message());
}

// Copy a single file, creating the destination directory if needed.
inline void copyFile(const fs::path& src, const fs::path& dst)
{
    makeDir(dst.parent_path());
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("copyFile failed: " + src.string() + " — " + ec.message());
}

// Recursively copy a directory.
inline void copyDir(const fs::path& src, const fs::path& dst)
{
    std::error_code ec;
    fs::copy(src, dst,
             fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
    if (ec) throw std::runtime_error("copyDir failed: " + src.string() + " — " + ec.message());
}

// Delete a file or directory tree; ignores errors.
inline void removePath(const fs::path& path)
{
    std::error_code ec;
    fs::remove_all(path, ec);
}

// Delete every file from sdRoot that has a matching relative path under srcRoot.
// Then prune any directories that were in srcRoot and are now empty on sdRoot.
inline void deleteMatchingFiles(const fs::path& srcRoot, const fs::path& sdRoot)
{
    std::error_code ec;

    // Pass 1: delete matching regular files
    for (auto& entry : fs::recursive_directory_iterator(srcRoot, ec)) {
        if (!fs::is_regular_file(entry.path())) continue;
        fs::path target = sdRoot / fs::relative(entry.path(), srcRoot);
        fs::remove(target, ec);
    }

    // Pass 2: prune empty directories, deepest first
    std::vector<fs::path> dirs;
    for (auto& entry : fs::recursive_directory_iterator(srcRoot, ec))
        if (fs::is_directory(entry.path()))
            dirs.push_back(sdRoot / fs::relative(entry.path(), srcRoot));
    std::reverse(dirs.begin(), dirs.end());
    for (auto& d : dirs)
        if (fs::exists(d, ec) && fs::is_empty(d, ec))
            fs::remove(d, ec);
}

// ============================================================
// Shell / subprocess helpers (no console window on Windows)
// ============================================================

#ifdef _WIN32
// Run cmd silently, return exit code.
inline int runSilent(const std::string& cmd)
{
    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    // CreateProcess needs a mutable buffer
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

// Run cmd silently and capture stdout+stderr, return output string.
inline std::string runCapture(const std::string& cmd)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hR, hW;
    if (!CreatePipe(&hR, &hW, &sa, 0)) return "";
    SetHandleInformation(hR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hW;
    si.hStdError  = hW;
    si.wShowWindow = SW_HIDE;

    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hR); CloseHandle(hW);
        return "";
    }
    CloseHandle(hW);

    std::string result;
    char rbuf[512]; DWORD n;
    while (ReadFile(hR, rbuf, sizeof(rbuf) - 1, &n, nullptr) && n)
        result.append(rbuf, n);

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hR);
    return result;
}
#endif // _WIN32

// Run a shell command and return its stdout.
inline std::string runCommand(const std::string& cmd)
{
#ifdef _WIN32
    return runCapture(cmd);
#else
    std::array<char, 512> buf;
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) throw std::runtime_error("runCommand: failed to open pipe for: " + cmd);
    while (fgets(buf.data(), (int)buf.size(), pipe))
        result += buf.data();
    pclose(pipe);
    return result;
#endif
}

// ============================================================
// Download a file via curl (ships with Win10+, macOS, all Linux distros)
// ============================================================

inline bool downloadFile(const std::string& url, const fs::path& dest,
                         const ProgressCallback& progress = {})
{
    makeDir(dest.parent_path());
    if (progress) progress(0.0f, "Downloading " + dest.filename().string() + "...");

    std::string cmd = "curl -L --silent --fail -o \""
                    + dest.string() + "\" \"" + url + "\"";
#ifdef _WIN32
    int ret = runSilent(cmd);
#else
    int ret = std::system(cmd.c_str());
#endif
    if (progress) progress(1.0f, ret == 0 ? "Done" : "Failed");
    return ret == 0;
}

// ============================================================
// ZIP extraction
// ============================================================

inline bool extractZip(const fs::path& zipPath, const fs::path& destDir)
{
    makeDir(destDir);
#ifdef _WIN32
    std::string cmd =
        "powershell -NoProfile -Command \"Expand-Archive -Force -Path '"
        + zipPath.string() + "' -DestinationPath '" + destDir.string() + "'\"";
    return runSilent(cmd) == 0;
#else
    std::string cmd = "unzip -o \"" + zipPath.string()
                    + "\" -d \"" + destDir.string() + "\" > /dev/null";
    return std::system(cmd.c_str()) == 0;
#endif
}

// ============================================================
// INI file helpers
// ============================================================

// Parse an INI file into an IniData map.
// Supports [sections], key=value pairs, and ; or # comments.
inline IniData readIni(const fs::path& path)
{
    IniData data;
    std::ifstream f(path);
    if (!f.is_open()) return data;

    auto trim = [](std::string& s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        auto end = s.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) s.erase(end + 1);
        else s.clear();
    };

    std::string line, section;
    while (std::getline(f, line)) {
        trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            auto close = line.find(']');
            section = (close != std::string::npos) ? line.substr(1, close - 1) : line.substr(1);
            trim(section);
        } else {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            trim(key); trim(val);
            data[section][key] = val;
        }
    }
    return data;
}

// Write an IniData map back to a file, preserving section order.
inline void writeIni(const fs::path& path, const IniData& data)
{
    makeDir(path.parent_path());
    std::ofstream f(path);
    if (!f.is_open()) throw std::runtime_error("writeIni: cannot open " + path.string());
    for (auto& [section, keys] : data) {
        if (!section.empty()) f << "[" << section << "]\n";
        for (auto& [key, val] : keys)
            f << key << "=" << val << "\n";
        f << "\n";
    }
}

// Get a value from an IniData map; returns `def` if not found.
inline std::string getIniValue(const IniData& data,
                               const std::string& section,
                               const std::string& key,
                               const std::string& def = "")
{
    auto sit = data.find(section);
    if (sit == data.end()) return def;
    auto kit = sit->second.find(key);
    return kit == sit->second.end() ? def : kit->second;
}

// Set a value in an IniData map.
inline void setIniValue(IniData& data,
                        const std::string& section,
                        const std::string& key,
                        const std::string& value)
{
    data[section][key] = value;
}

// Read an INI file, set one key, and write it back. Creates the file if missing.
inline void patchIni(const fs::path& path,
                     const std::string& section,
                     const std::string& key,
                     const std::string& value)
{
    IniData d = readIni(path);
    setIniValue(d, section, key, value);
    writeIni(path, d);
}

// ============================================================
// GitHub release helpers
// ============================================================

// Fetch the latest release download URL for a given GitHub repo and asset name
// (partial match, e.g. ".zip", "atmosphere-", etc.).
// Requires curl and Python or jq to parse JSON. Returns "" on failure.
// NOTE: For production, replace this with a proper JSON HTTP client.
inline std::string getLatestReleaseUrl(const std::string& owner,
                                       const std::string& repo,
                                       const std::string& assetContains,
                                       const std::string& assetExcludes = "")
{
    std::string api = "https://api.github.com/repos/" + owner + "/" + repo + "/releases/latest";
#ifdef _WIN32
    std::string cmd = "curl -s -H \"Accept: application/vnd.github+json\" \""
                    + api + "\" | findstr /i \"browser_download_url\" | findstr /i \""
                    + assetContains + "\"";
    if (!assetExcludes.empty())
        cmd += " | findstr /v /i \"" + assetExcludes + "\"";
#else
    std::string cmd = "curl -s -H 'Accept: application/vnd.github+json' \""
                    + api + "\" | grep -i browser_download_url | grep -i \""
                    + assetContains + "\"";
    if (!assetExcludes.empty())
        cmd += " | grep -vi \"" + assetExcludes + "\"";
#endif
    std::string out = runCommand(cmd);
    auto start = out.find("https://");
    if (start == std::string::npos) return "";
    auto end = out.find('"', start);
    return end != std::string::npos ? out.substr(start, end - start) : out.substr(start);
}
