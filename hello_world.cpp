#include "immapp/immapp.h"
#include "hello_imgui/hello_imgui.h"
#include "imgui.h"
#include "installer.h"
#include "nlohmann/json.hpp"

#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>

using json = nlohmann::json;

// ============================================================
// Data model (loaded from JSON)
// ============================================================

enum class StepType { GithubZip, GithubFile, DirectZip, DirectFile, HekateBootEntries, Unknown };

struct InstallStep {
    StepType    type         = StepType::Unknown;
    std::string owner, repo;
    std::string assetMatch, assetExclude;
    std::string url;
    std::string extractTo;
    std::string dest;
};

struct ToolDef {
    std::string name;
    std::string description;
    std::string checkPath;
    std::string category;
    std::vector<InstallStep> steps;
    bool selected = false;   // deselected by default
};

static std::vector<ToolDef>     gTools;
static std::vector<std::string> gCategories;
static int                      gSelectedCat = 0;

static const struct { const char* cat; const char* file; } kCatFiles[] = {
    { "Essentials",   "tools/essentials.json"   },
    { "Overclocking", "tools/overclocking.json" },
    { "Sysmodules",   "tools/sysmodules.json"   },
    { "Overlays",     "tools/overlays.json"     },
    { "Homebrew",     "tools/homebrew.json"     },
};

// ============================================================
// Installer state
// ============================================================

struct InstallerState {
    std::vector<DriveInfo> drives;
    int selectedDrive = -1;

    std::vector<std::string> log;
    std::mutex logMutex;

    std::atomic<bool>  installing{false};
    std::atomic<bool>  deleting{false};
    std::atomic<float> overallProgress{0.f};
    std::string        currentStatus;

    bool busy() const { return installing || deleting; }

    void addLog(const std::string& msg) {
        std::lock_guard<std::mutex> lk(logMutex);
        log.push_back(msg);
    }

    void refreshDrives() {
        drives = listDrives();
        selectedDrive = -1;
    }

    bool isInstalled(const ToolDef& t) const {
        if (selectedDrive < 0) return false;
        std::error_code ec;
        return fs::exists(fs::path(drives[selectedDrive].path) / t.checkPath, ec);
    }
};

static InstallerState gState;

// ============================================================
// Download helpers
// ============================================================

static bool dlFile(const std::string& url, const fs::path& dest, const std::string& label)
{
    gState.addLog("  -> Downloading " + label + "...");
    bool ok = downloadFile(url, dest, [&](float, const std::string& s) {
        gState.currentStatus = label + ": " + s;
    });
    if (!ok) gState.addLog("  [!] Download failed: " + label);
    return ok;
}

static bool resolveUrl(const std::string& owner, const std::string& repo,
                       const std::string& contains, const std::string& excludes,
                       std::string& outUrl)
{
    gState.addLog("  -> Resolving " + owner + "/" + repo + "...");
    outUrl = getLatestReleaseUrl(owner, repo, contains, excludes);
    if (outUrl.empty()) { gState.addLog("  [!] Could not resolve URL for " + repo); return false; }
    gState.addLog("  -> " + outUrl);
    return true;
}

// ============================================================
// Hekate INI helpers
// ============================================================

static void patchHekateBootEntries(const fs::path& iniPath)
{
    std::string content;
    {
        std::ifstream f(iniPath);
        if (f.is_open()) { std::ostringstream ss; ss << f.rdbuf(); content = ss.str(); }
    }

    struct Entry { std::string header, block; };
    static const Entry entries[] = {
        { "[Atmosphere EmuNAND + HOC]",  "[Atmosphere EmuNAND + HOC]\npkg3=atmosphere/package3\nemummcforce=1\nkip1=atmosphere/kips/hoc.kip\n" },
        { "[Atmosphere SysNAND + HOC]",  "[Atmosphere SysNAND + HOC]\npkg3=atmosphere/package3\nkip1=atmosphere/kips/hoc.kip\n" },
        { "[Atmosphere EmuNAND (Safe)]", "[Atmosphere EmuNAND (Safe)]\npkg3=atmosphere/package3\nemummcforce=1\n" },
        { "[Atmosphere SysNAND (Safe)]", "[Atmosphere SysNAND (Safe)]\npkg3=atmosphere/package3\n" },
    };

    std::string toAppend;
    for (auto& e : entries)
        if (content.find(e.header) == std::string::npos)
            toAppend += "\n" + e.block;

    if (!toAppend.empty()) {
        makeDir(iniPath.parent_path());
        std::ofstream f(iniPath, std::ios::app);
        f << toAppend;
        gState.addLog("  -> Appended HOC boot entries");
    } else {
        gState.addLog("  -> HOC entries already present");
    }
}

static void removeHekateBootEntries(const fs::path& iniPath)
{
    std::ifstream f(iniPath);
    if (!f.is_open()) return;

    static const char* kHocHeaders[] = {
        "[Atmosphere EmuNAND + HOC]",
        "[Atmosphere SysNAND + HOC]",
        "[Atmosphere EmuNAND (Safe)]",
        "[Atmosphere SysNAND (Safe)]",
    };

    auto isHocHeader = [&](const std::string& line) {
        std::string s = line;
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        if (s.empty()) return false;
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        for (auto* h : kHocHeaders) if (s == h) return true;
        return false;
    };

    std::vector<std::string> lines, out;
    std::string line;
    while (std::getline(f, line)) lines.push_back(line);
    f.close();

    bool skipping = false;
    for (auto& l : lines) {
        if (isHocHeader(l)) { skipping = true; continue; }
        if (skipping && !l.empty() && l[0] == '[') skipping = false;
        if (!skipping) out.push_back(l);
    }

    std::ofstream fw(iniPath);
    for (auto& l : out) fw << l << '\n';
    gState.addLog("  -> Removed HOC boot entries from hekate_ipl.ini");
}

// ============================================================
// Install step executor
// ============================================================

static bool executeStep(const InstallStep& step, const fs::path& sd)
{
    switch (step.type) {
    case StepType::GithubZip: {
        std::string url;
        if (!resolveUrl(step.owner, step.repo, step.assetMatch, step.assetExclude, url)) return false;
        fs::path tmp = fs::temp_directory_path() / (step.repo + "_dl.zip");
        if (!dlFile(url, tmp, step.repo)) return false;
        fs::path dest = step.extractTo.empty() ? sd : sd / step.extractTo;
        gState.addLog("  -> Extracting " + step.repo + "...");
        bool ok = extractZip(tmp, dest);
        removePath(tmp);
        if (!ok) gState.addLog("  [!] Extraction failed");
        return ok;
    }
    case StepType::GithubFile: {
        std::string url;
        if (!resolveUrl(step.owner, step.repo, step.assetMatch, step.assetExclude, url)) return false;
        fs::path dest = sd / step.dest;
        makeDir(dest.parent_path());
        return dlFile(url, dest, fs::path(step.dest).filename().string());
    }
    case StepType::DirectZip: {
        fs::path tmp = fs::temp_directory_path() / "hbi_dl.zip";
        if (!dlFile(step.url, tmp, "archive")) return false;
        fs::path dest = step.extractTo.empty() ? sd : sd / step.extractTo;
        bool ok = extractZip(tmp, dest);
        removePath(tmp);
        return ok;
    }
    case StepType::DirectFile: {
        fs::path dest = sd / step.dest;
        makeDir(dest.parent_path());
        return dlFile(step.url, dest, fs::path(step.dest).filename().string());
    }
    case StepType::HekateBootEntries:
        patchHekateBootEntries(sd / "bootloader" / "hekate_ipl.ini");
        return true;
    default:
        gState.addLog("  [!] Unknown step type");
        return false;
    }
}

// ============================================================
// Delete step executor
// Re-downloads the same assets, extracts to temp, then mirrors
// the deletion onto the SD card.
// ============================================================

static bool deleteStep(const InstallStep& step, const fs::path& sd)
{
    switch (step.type) {
    case StepType::GithubZip: {
        std::string url;
        if (!resolveUrl(step.owner, step.repo, step.assetMatch, step.assetExclude, url)) return false;
        fs::path tmp    = fs::temp_directory_path() / (step.repo + "_del.zip");
        fs::path tmpDir = fs::temp_directory_path() / (step.repo + "_del");
        if (!dlFile(url, tmp, step.repo)) return false;
        gState.addLog("  -> Extracting to temp for file list...");
        if (!extractZip(tmp, tmpDir)) { removePath(tmp); return false; }
        removePath(tmp);
        fs::path sdDest = step.extractTo.empty() ? sd : sd / step.extractTo;
        deleteMatchingFiles(tmpDir, sdDest);
        removePath(tmpDir);
        return true;
    }
    case StepType::DirectZip: {
        fs::path tmp    = fs::temp_directory_path() / "hbi_del.zip";
        fs::path tmpDir = fs::temp_directory_path() / "hbi_del";
        if (!dlFile(step.url, tmp, "archive")) return false;
        if (!extractZip(tmp, tmpDir)) { removePath(tmp); return false; }
        removePath(tmp);
        fs::path sdDest = step.extractTo.empty() ? sd : sd / step.extractTo;
        deleteMatchingFiles(tmpDir, sdDest);
        removePath(tmpDir);
        return true;
    }
    case StepType::GithubFile:
    case StepType::DirectFile: {
        std::error_code ec;
        fs::remove(sd / step.dest, ec);
        return true;
    }
    case StepType::HekateBootEntries:
        removeHekateBootEntries(sd / "bootloader" / "hekate_ipl.ini");
        return true;
    default:
        gState.addLog("  [!] Unknown step type");
        return false;
    }
}

// ============================================================
// JSON loader
// ============================================================

static StepType parseStepType(const std::string& s)
{
    if (s == "github_zip")          return StepType::GithubZip;
    if (s == "github_file")         return StepType::GithubFile;
    if (s == "direct_zip")          return StepType::DirectZip;
    if (s == "direct_file")         return StepType::DirectFile;
    if (s == "hekate_boot_entries") return StepType::HekateBootEntries;
    return StepType::Unknown;
}

static void loadTools()
{
    gTools.clear();
    gCategories.clear();

    for (auto& [cat, file] : kCatFiles) {
        std::string path = HelloImGui::AssetFileFullPath(file);
        std::ifstream f(path);
        if (!f.is_open()) continue;

        json j;
        try { f >> j; } catch (...) { continue; }

        bool catPushed = false;
        for (auto& jt : j) {
            ToolDef t;
            t.name        = jt.value("name",        "");
            t.description = jt.value("description", "");
            t.checkPath   = jt.value("checkPath",   "");
            t.category    = cat;
            t.selected    = false;  // always start deselected

            if (jt.contains("steps")) {
                for (auto& js : jt["steps"]) {
                    InstallStep s;
                    s.type         = parseStepType(js.value("type",         ""));
                    s.owner        = js.value("owner",        "");
                    s.repo         = js.value("repo",         "");
                    s.assetMatch   = js.value("assetMatch",   "");
                    s.assetExclude = js.value("assetExclude", "");
                    s.url          = js.value("url",          "");
                    s.extractTo    = js.value("extractTo",    "");
                    s.dest         = js.value("dest",         "");
                    t.steps.push_back(std::move(s));
                }
            }

            if (!t.name.empty()) {
                gTools.push_back(std::move(t));
                if (!catPushed) { gCategories.push_back(cat); catPushed = true; }
            }
        }
    }
}

// ============================================================
// Background threads
// ============================================================

static void runInstall(fs::path sdRoot)
{
    gState.addLog("[*] Installing to: " + sdRoot.string());

    int total = 0, done = 0;
    for (auto& t : gTools) if (t.selected) ++total;
    if (total == 0) { gState.addLog("[!] Nothing selected."); gState.installing = false; return; }

    for (auto& t : gTools) {
        if (!t.selected) continue;
        gState.addLog(std::string(gState.isInstalled(t) ? "[^] Updating " : "[~] Installing ") + t.name + "...");
        bool ok = true;
        for (auto& step : t.steps)
            if (!executeStep(step, sdRoot)) { ok = false; break; }
        gState.addLog(ok ? ("[+] " + t.name + " done.") : ("[!] " + t.name + " FAILED."));
        gState.overallProgress = (float)(++done) / (float)total;
    }

    gState.addLog("[*] Done.");
    gState.installing = false;
}

static void runDelete(fs::path sdRoot)
{
    gState.addLog("[*] Deleting from: " + sdRoot.string());

    int total = 0, done = 0;
    for (auto& t : gTools) if (t.selected) ++total;
    if (total == 0) { gState.addLog("[!] Nothing selected."); gState.deleting = false; return; }

    for (auto& t : gTools) {
        if (!t.selected) continue;
        gState.addLog("[x] Removing " + t.name + "...");
        bool ok = true;
        for (auto& step : t.steps)
            if (!deleteStep(step, sdRoot)) { ok = false; break; }
        gState.addLog(ok ? ("[+] " + t.name + " removed.") : ("[!] " + t.name + " FAILED."));
        gState.overallProgress = (float)(++done) / (float)total;
    }

    gState.addLog("[*] Done.");
    gState.deleting = false;
}

// ============================================================
// GUI
// ============================================================

static void Gui()
{
    static bool loaded = false;
    if (!loaded) { loadTools(); loaded = true; }

    float totalAvailH = ImGui::GetContentRegionAvail().y;

    // ---- Drive selector ----------------------------------------
    ImGui::SeparatorText("Target SD Card");
    if (ImGui::Button("Refresh")) gState.refreshDrives();
    ImGui::SameLine();
    const char* preview = gState.selectedDrive >= 0
        ? gState.drives[gState.selectedDrive].path.c_str() : "(select drive)";
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##drive", preview)) {
        for (int i = 0; i < (int)gState.drives.size(); ++i) {
            auto& d = gState.drives[i];
            char buf[256];
            snprintf(buf, sizeof(buf), "%s  [%s]  %.1f GB",
                     d.path.c_str(), d.label.c_str(),
                     (float)d.totalBytes / (1024.f * 1024.f * 1024.f));
            if (ImGui::Selectable(buf, gState.selectedDrive == i))
                gState.selectedDrive = i;
        }
        ImGui::EndCombo();
    }

    // ---- Tools panel -------------------------------------------
    ImGui::SeparatorText("Tools");

    if (ImGui::Button("All"))  for (auto& t : gTools) t.selected = true;
    ImGui::SameLine();
    if (ImGui::Button("None")) for (auto& t : gTools) t.selected = false;

    float panelH = totalAvailH - 260.f;
    if (panelH < 100.f) panelH = 100.f;

    // ---- Sidebar -----------------------------------------------
    float sideW = 130.f;
    ImGui::BeginChild("##sidebar", ImVec2(sideW, panelH), true);
    for (int i = 0; i < (int)gCategories.size(); ++i) {
        int selCount = 0, totalCount = 0;
        for (auto& t : gTools)
            if (t.category == gCategories[i]) { ++totalCount; if (t.selected) ++selCount; }

        char label[64];
        snprintf(label, sizeof(label), "%s\n%d / %d", gCategories[i].c_str(), selCount, totalCount);

        bool active = (gSelectedCat == i);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Header,        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Selectable(label, active, 0, ImVec2(0, 40))) gSelectedCat = i;
        if (active) ImGui::PopStyleColor(2);
    }
    ImGui::EndChild();

    // ---- Tools list (right panel) ------------------------------
    ImGui::SameLine();
    ImGui::BeginChild("##toolspanel", ImVec2(-1, panelH), true);

    if (!gCategories.empty() && gSelectedCat < (int)gCategories.size()) {
        const std::string& activeCat = gCategories[gSelectedCat];
        bool anyInCat = false;
        for (auto& t : gTools) {
            if (t.category != activeCat) continue;
            anyInCat = true;
            bool installed = gState.isInstalled(t);
            ImGui::Checkbox(t.name.c_str(), &t.selected);
            if (installed) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.f), "(installed)");
            }
            ImGui::Indent(22.f);
            ImGui::TextDisabled("%s", t.description.c_str());
            ImGui::Unindent(22.f);
            ImGui::Spacing();
        }
        if (!anyInCat) ImGui::TextDisabled("No tools defined in this category.");
    } else {
        ImGui::TextDisabled("No tools loaded. Check assets/tools/*.json");
    }
    ImGui::EndChild();

    // ---- Action buttons + progress -----------------------------
    ImGui::Spacing();

    bool anySelected = false, allInstalled = true;
    for (auto& t : gTools) {
        if (!t.selected) continue;
        anySelected = true;
        if (!gState.isInstalled(t)) allInstalled = false;
    }
    bool isUpdate   = anySelected && allInstalled;
    bool canAct     = gState.selectedDrive >= 0 && !gState.busy() && anySelected;

    if (!canAct) ImGui::BeginDisabled();

    if (ImGui::Button(isUpdate ? "Update" : "Install", ImVec2(100, 0))) {
        fs::path root = gState.drives[gState.selectedDrive].path;
        gState.log.clear();
        gState.overallProgress = 0.f;
        gState.installing      = true;
        std::thread(runInstall, root).detach();
    }

    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.65f, 0.15f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.20f, 0.20f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.10f, 0.10f, 1.f));
    if (ImGui::Button("Delete", ImVec2(100, 0))) {
        fs::path root = gState.drives[gState.selectedDrive].path;
        gState.log.clear();
        gState.overallProgress = 0.f;
        gState.deleting        = true;
        std::thread(runDelete, root).detach();
    }
    ImGui::PopStyleColor(3);

    if (!canAct) ImGui::EndDisabled();

    if (gState.busy() || gState.overallProgress > 0.f) {
        ImGui::SameLine();
        ImGui::ProgressBar(gState.overallProgress, ImVec2(-1, 0),
                           gState.currentStatus.c_str());
    }

    // ---- Log ---------------------------------------------------
    ImGui::SeparatorText("Log");
    ImGui::BeginChild("##log", ImVec2(0, -1), true, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(gState.logMutex);
        for (auto& line : gState.log)
            ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

// ============================================================
// Entry point
// ============================================================

int main(int, char*[])
{
#ifdef ASSETS_LOCATION
    HelloImGui::SetAssetsFolder(ASSETS_LOCATION);
#endif

    gState.refreshDrives();

    HelloImGui::SimpleRunnerParams params;
    params.guiFunction = Gui;
    params.windowTitle = "hb-installer";
    params.windowSize  = {800, 620};

    ImmApp::AddOnsParams addOns;
    ImmApp::Run(params, addOns);
    return 0;
}
