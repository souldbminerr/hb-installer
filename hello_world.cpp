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
#include <functional>
#include <map>
#include <chrono>
#ifdef _WIN32
#include <shellapi.h>
#endif

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
    { "Payloads",       "tools/payloads.json"   },
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
    std::atomic<bool>  formatting{false};
    std::atomic<float> overallProgress{0.f};
    std::string        currentStatus;

    bool busy() const { return installing || deleting || formatting; }

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
// Slideshow / Tutorial system
// ============================================================

struct SlideButton {
    std::string label;
    std::string action; // "cmd" or "cmd:param"
};

struct Slide {
    std::string title;
    std::string body;
    std::string note;
    std::string inputPlaceholder;              // if set, renders an InputText
    bool        detectDrive      = false;      // show drive-detection status + auto-select new drives
    std::vector<std::string> installTools;     // if set, "install_recommended" installs tools with these names
    std::vector<SlideButton> buttons;
};

struct Slideshow {
    std::string id;
    std::string title;
    std::vector<Slide> slides;
    int  current    = 0;
    bool showInMenu = true;       // false = wizard, not shown as a Guides button
    char inputBuf[128] = {};      // shared input buffer across all slides
};

static std::vector<Slideshow>                       gTutorials;
static int                                          gActiveTutorial = -1;
static bool                                         gTutorialCloseRequested = false;
static std::string                                  gPendingGuide;
static std::string                                  gReturnTarget; // "tutorial_id:slide" to return to after a guide
static std::vector<DriveInfo>                       gDriveBaseline;
using ActionFn = std::function<void(const std::string&)>;
static std::map<std::string, ActionFn>              gActionRegistry;

// Forward-declared so action lambdas can reference them
static void runInstall(fs::path sdRoot);
static void runFormat(std::string driveLetter);

static void dispatchAction(const std::string& actionStr)
{
    // Support chained actions separated by '|'  e.g. "set_guide:rcm|goto:6"
    auto pipe = actionStr.find('|');
    if (pipe != std::string::npos) {
        dispatchAction(actionStr.substr(0, pipe));
        dispatchAction(actionStr.substr(pipe + 1));
        return;
    }
    auto sep  = actionStr.find(':');
    std::string cmd   = (sep == std::string::npos) ? actionStr : actionStr.substr(0, sep);
    std::string param = (sep == std::string::npos) ? ""        : actionStr.substr(sep + 1);
    auto it = gActionRegistry.find(cmd);
    if (it != gActionRegistry.end()) it->second(param);
}

static void registerTutorialActions()
{
    gActionRegistry["next"] = [](const std::string&) {
        if (gActiveTutorial < 0) return;
        auto& ss = gTutorials[gActiveTutorial];
        if (ss.current + 1 < (int)ss.slides.size()) ++ss.current;
    };
    gActionRegistry["prev"] = [](const std::string&) {
        if (gActiveTutorial < 0) return;
        auto& ss = gTutorials[gActiveTutorial];
        if (ss.current > 0) --ss.current;
    };
    gActionRegistry["goto"] = [](const std::string& p) {
        if (gActiveTutorial < 0) return;
        auto& ss = gTutorials[gActiveTutorial];
        int idx = std::stoi(p);
        if (idx >= 0 && idx < (int)ss.slides.size()) ss.current = idx;
    };
    gActionRegistry["close"] = [](const std::string&) {
        gActiveTutorial = -1;
        gTutorialCloseRequested = true;
    };
    gActionRegistry["open_url"] = [](const std::string& url) {
#ifdef _WIN32
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOW);
#else
        std::system(("xdg-open " + url).c_str());
#endif
    };
    gActionRegistry["copy"] = [](const std::string& text) {
        ImGui::SetClipboardText(text.c_str());
    };
    // Switch to a different tutorial by id without closing the modal
    gActionRegistry["open_guide"] = [](const std::string& id) {
        for (int i = 0; i < (int)gTutorials.size(); ++i) {
            if (gTutorials[i].id == id) {
                gActiveTutorial = i;
                gTutorials[i].current = 0;
                return;
            }
        }
    };
    // Read inputBuf from current slideshow, classify serial using exact GBATemp ranges,
    // then jump to the appropriate result slide:
    //   goto:2 = unpatched, goto:3 = borderline, goto:4 = patched / not in table
    gActionRegistry["check_serial"] = [](const std::string&) {
        if (gActiveTutorial < 0) return;
        auto& ss = gTutorials[gActiveTutorial];

        // Normalise: uppercase, strip spaces and dashes
        std::string u;
        for (unsigned char c : ss.inputBuf)
            if (c && c != ' ' && c != '-') u += (char)toupper(c);

        if (u.size() < 5) return; // need prefix + at least one digit

        // Serial format: 4-char prefix + up to 10-digit number  e.g. XAW10065000000
        std::string prefix4 = u.substr(0, 4);
        std::string tail    = u.substr(4);
        std::string digits;
        for (unsigned char c : tail) if (isdigit(c)) digits += (char)c;
        if (digits.empty()) return;

        long long num = 0;
        try { num = std::stoll(digits); } catch (...) { return; }

        // Range table sourced from GBATemp serial thread.
        // start        : num below this is not in table (treat as patched)
        // unpatched_lt : num < this => unpatched
        // borderline_lt: num < this => borderline; -1 => no upper bound (all borderline above unpatched_lt)
        struct Range {
            const char* prefix;
            long long   start;
            long long   unpatched_lt;
            long long   borderline_lt;
        };
        static const Range kRanges[] = {
            { "XAW1",          0LL,  65000000LL, 120000000LL },
            { "XAW4",          0LL,  11000000LL,  12000000LL },
            { "XAW7",          0LL,  17500000LL,  30000000LL },
            { "XAW9",          0LL,         0LL,          -1LL }, // all borderline
            { "XAJ1",          0LL,  20000000LL,  30000000LL },
            { "XAJ4",          0LL,  44000000LL,  83000000LL },
            { "XAJ7",          0LL,  40000000LL,  50000000LL },
            { "XAJ9", 5000000000LL, 5000000000LL,          -1LL }, // borderline from 5B up
            { "XAK1",          0LL,         0LL,          -1LL }, // all borderline
            { "XAK4",          0LL,         0LL,          -1LL },
            { "XAK7",          0LL,         0LL,          -1LL },
            { "XAK9",          0LL,         0LL,          -1LL },
        };

        for (auto& r : kRanges) {
            if (prefix4 != r.prefix) continue;
            if (num < r.start)                                      { dispatchAction("goto:4"); return; }
            if (num < r.unpatched_lt)                               { dispatchAction("goto:2"); return; }
            if (r.borderline_lt == -1 || num < r.borderline_lt)    { dispatchAction("goto:3"); return; }
            dispatchAction("goto:4"); return; // >= borderline_lt = patched
        }
        // Prefix not in table = not vulnerable to fusee-gelee = patched
        dispatchAction("goto:4");
    };

    // Store which guide should open after the SD-card setup slide
    gActionRegistry["set_guide"] = [](const std::string& id) {
        gPendingGuide = id;
    };
    // Store where to return after a standalone guide (format: "tutorial_id:slide_index")
    gActionRegistry["set_return"] = [](const std::string& target) {
        gReturnTarget = target;
    };
    // Jump back to the stored return target, or close if none was set
    gActionRegistry["return_to"] = [](const std::string&) {
        if (gReturnTarget.empty()) { dispatchAction("close"); return; }
        std::string target = gReturnTarget;
        gReturnTarget.clear();
        auto colon = target.find(':');
        std::string id    = (colon == std::string::npos) ? target : target.substr(0, colon);
        std::string slide = (colon == std::string::npos) ? ""     : target.substr(colon + 1);
        dispatchAction("open_guide:" + id);
        if (!slide.empty()) dispatchAction("goto:" + slide);
    };
    gActionRegistry["open_pending_guide"] = [](const std::string&) {
        if (!gPendingGuide.empty()) {
            dispatchAction("open_guide:" + gPendingGuide);
            gPendingGuide.clear();
        } else {
            dispatchAction("close");
        }
    };

    // Refresh drives and auto-select any newly inserted drive
    gActionRegistry["detect_drive"] = [](const std::string&) {
        auto fresh = listDrives();
        for (auto& d : fresh) {
            bool isNew = true;
            for (auto& b : gDriveBaseline)
                if (b.path == d.path) { isNew = false; break; }
            if (isNew) {
                gState.drives = fresh;
                for (int i = 0; i < (int)gState.drives.size(); ++i)
                    if (gState.drives[i].path == d.path) { gState.selectedDrive = i; break; }
                return;
            }
        }
        // No new drive — just refresh the list
        gState.refreshDrives();
    };

    // Format selected drive to FAT32 in a background thread
    gActionRegistry["format_fat32"] = [](const std::string&) {
        if (gState.selectedDrive < 0) {
            gState.addLog("[!] No drive selected.");
            return;
        }
        if (gState.busy()) {
            gState.addLog("[!] Another operation is already running.");
            return;
        }
        std::string path = gState.drives[gState.selectedDrive].path;
        // Expect Windows path like "F:\" — extract the drive letter
        if (path.size() < 2 || path[1] != ':') {
            gState.addLog("[!] Could not determine drive letter from: " + path);
            return;
        }
        std::string letter(1, (char)toupper((unsigned char)path[0]));
        gState.log.clear();
        gState.overallProgress = 0.f;
        gState.currentStatus   = "Formatting...";
        gState.formatting      = true;
        std::thread(runFormat, letter).detach();
    };

    // Select named tools then kick off background install
    gActionRegistry["install_recommended"] = [](const std::string&) {
        if (gActiveTutorial < 0 || gState.selectedDrive < 0 || gState.busy()) return;
        auto& ss = gTutorials[gActiveTutorial];
        if (ss.current < 0 || ss.current >= (int)ss.slides.size()) return;
        auto& names = ss.slides[ss.current].installTools;
        if (names.empty()) return;

        for (auto& t : gTools)
            for (auto& name : names)
                if (t.name == name) t.selected = true;

        fs::path root = gState.drives[gState.selectedDrive].path;
        gState.log.clear();
        gState.overallProgress = 0.f;
        gState.installing      = true;
        std::thread(runInstall, root).detach();
    };
}

static void loadTutorials()
{
    static const struct { const char* file; } kFiles[] = {
        { "tutorials/start.json"    },   // wizard — shown first, hidden from menu
        { "tutorials/rcm.json"      },
        { "tutorials/modchips.json" },
    };

    gTutorials.clear();
    for (auto& [file] : kFiles) {
        std::string path = HelloImGui::AssetFileFullPath(file);
        std::ifstream f(path);
        if (!f.is_open()) continue;
        json j;
        try { f >> j; } catch (...) { continue; }

        Slideshow ss;
        ss.id         = j.value("id",         "");
        ss.title      = j.value("title",      "");
        ss.showInMenu = j.value("menu",        true);
        ss.current    = 0;
        memset(ss.inputBuf, 0, sizeof(ss.inputBuf));
        for (auto& js : j.value("slides", json::array())) {
            Slide s;
            s.title            = js.value("title",            "");
            s.body             = js.value("body",             "");
            s.note             = js.value("note",             "");
            s.inputPlaceholder = js.value("inputPlaceholder", "");
            s.detectDrive      = js.value("detectDrive",      false);
            for (auto& jt : js.value("installTools", json::array()))
                s.installTools.push_back(jt.get<std::string>());
            for (auto& jb : js.value("buttons", json::array())) {
                SlideButton b;
                b.label  = jb.value("label",  "");
                b.action = jb.value("action", "");
                s.buttons.push_back(std::move(b));
            }
            ss.slides.push_back(std::move(s));
        }
        if (!ss.slides.empty()) gTutorials.push_back(std::move(ss));
    }
}

static void renderTutorialModal()
{
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(540, 400), ImGuiCond_Always);

    if (!ImGui::BeginPopupModal("##tutorial_modal", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        return;

    if (gActiveTutorial < 0 || gActiveTutorial >= (int)gTutorials.size()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    auto& ss    = gTutorials[gActiveTutorial];
    auto& slide = ss.slides[ss.current];

    // --- Header ---
    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.f, 1.f), "%s", ss.title.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 34.f);
    if (ImGui::SmallButton(" X ")) {
        gActiveTutorial = -1;
        gTutorialCloseRequested = true;
    }
    ImGui::Separator();
    ImGui::Spacing();

    // --- Slide title ---
    ImGui::TextColored(ImVec4(1.f, 0.95f, 0.55f, 1.f), "%s", slide.title.c_str());
    ImGui::Spacing();

    // Snapshot drives when first entering a detect-drive slide
    {
        static int prevTut = -1, prevSl = -1;
        if ((gActiveTutorial != prevTut || ss.current != prevSl) && slide.detectDrive) {
            gDriveBaseline = listDrives();
        }
        prevTut = gActiveTutorial; prevSl = ss.current;
    }

    // --- Heights ---
    float inputH   = slide.inputPlaceholder.empty() ? 0.f : 32.f;
    float driveH   = slide.detectDrive ? 28.f : 0.f;
    bool showProgress = (gState.busy() || gState.overallProgress > 0.f) &&
                        (!slide.installTools.empty() || gState.formatting);
    float installH = showProgress ? 20.f : 0.f;
    float noteH    = slide.note.empty() ? 0.f : 56.f;
    float footerH  = 34.f;
    float bodyH    = ImGui::GetContentRegionAvail().y - inputH - driveH - installH - noteH - footerH;
    if (bodyH < 40.f) bodyH = 40.f;

    // --- Body ---
    ImGui::BeginChild("##sbody", ImVec2(0.f, bodyH));
    ImGui::TextWrapped("%s", slide.body.c_str());
    ImGui::EndChild();

    // --- Input field ---
    if (!slide.inputPlaceholder.empty()) {
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint("##sinput", slide.inputPlaceholder.c_str(),
                                 ss.inputBuf, sizeof(ss.inputBuf));
    }

    // --- Drive selector dropdown ---
    if (slide.detectDrive) {
        if (ImGui::SmallButton("Refresh")) gState.refreshDrives();
        ImGui::SameLine();
        const char* preview = gState.selectedDrive >= 0
            ? gState.drives[gState.selectedDrive].path.c_str() : "(select SD card)";
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::BeginCombo("##modal_drive", preview)) {
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
    }

    // --- Install / format progress ---
    if (showProgress) {
        const char* label = gState.formatting ? "Formatting..." : gState.currentStatus.c_str();
        float prog = gState.formatting ? -1.f : (float)gState.overallProgress; // -1 = indeterminate
        ImGui::ProgressBar(prog, ImVec2(-1.f, 16.f), label);
    }

    // --- Note ---
    if (!slide.note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.28f, 0.22f, 0.08f, 1.f));
        ImGui::BeginChild("##snote", ImVec2(0.f, noteH - 4.f), true);
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "[!]");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", slide.note.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // --- Footer: progress + buttons ---
    ImGui::Separator();
    ImGui::Text("%d / %d", ss.current + 1, (int)ss.slides.size());
    ImGui::SameLine();

    // right-align buttons
    float totalBtnW = 0.f;
    for (auto& b : slide.buttons)
        totalBtnW += ImGui::CalcTextSize(b.label.c_str()).x
                   + ImGui::GetStyle().FramePadding.x * 2.f + 8.f;
    float startX = ImGui::GetWindowWidth()
                 - totalBtnW
                 - ImGui::GetStyle().WindowPadding.x;
    if (startX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(startX);

    for (size_t i = 0; i < slide.buttons.size(); ++i) {
        auto& btn = slide.buttons[i];
        if (ImGui::Button(btn.label.c_str()))
            dispatchAction(btn.action);
        if (i + 1 < slide.buttons.size()) ImGui::SameLine();
    }

    if (gTutorialCloseRequested) {
        gTutorialCloseRequested = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

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

static void runFormat(std::string letter)
{
    gState.addLog("[*] Formatting " + letter + ": to FAT32...");
    gState.addLog("[*] This may take several minutes on large cards.");

    // Check for existing atmosphere install and warn (do not abort — user confirmed)
    std::error_code ec;
    if (fs::exists(fs::path(letter + ":\\") / "atmosphere", ec))
        gState.addLog("[!] WARNING: existing Atmosphere installation detected — it will be erased.");

#ifdef _WIN32
    // PowerShell Format-Volume supports FAT32 on drives of any size unlike the built-in format command
    std::string cmd =
        "powershell -NonInteractive -Command \""
        "Format-Volume -DriveLetter " + letter +
        " -FileSystem FAT32 -NewFileSystemLabel Switch -Force -Confirm:$false"
        "\"";
    std::string out = runCapture(cmd);
    if (!out.empty()) gState.addLog(out);
#else
    gState.addLog("[!] Automatic formatting is only supported on Windows.");
    gState.formatting = false;
    return;
#endif

    // Verify drive is accessible after format
    gState.addLog("[*] Verifying...");
    std::this_thread::sleep_for(std::chrono::seconds(2)); // let OS remount
    bool ok = fs::exists(fs::path(letter + ":\\"), ec);
    if (ok) {
        gState.addLog("[+] Format complete. Drive " + letter + ": is ready.");
        gState.refreshDrives();
        // Re-select the just-formatted drive
        for (int i = 0; i < (int)gState.drives.size(); ++i)
            if (!gState.drives[i].path.empty() &&
                toupper((unsigned char)gState.drives[i].path[0]) == toupper((unsigned char)letter[0]))
            { gState.selectedDrive = i; break; }
    } else {
        gState.addLog("[!] Format may have failed. Try running hb-installer as Administrator.");
    }
    gState.currentStatus = "";
    gState.formatting    = false;
}

// ============================================================
// GUI
// ============================================================

static void Gui()
{
    static bool loaded          = false;
    static bool pendingOpenStart = false;
    if (!loaded) {
        gState.refreshDrives();
        loadTools();
        loadTutorials();
        // Auto-open the start wizard on first frame
        for (int i = 0; i < (int)gTutorials.size(); ++i) {
            if (gTutorials[i].id == "start") { gActiveTutorial = i; pendingOpenStart = true; break; }
        }
        loaded = true;
    }
    if (pendingOpenStart) {
        pendingOpenStart = false;
        ImGui::OpenPopup("##tutorial_modal");
    }

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

    // ---- Tutorials -----------------------------------------------
    ImGui::SeparatorText("Guides");
    bool firstBtn = true;
    for (int i = 0; i < (int)gTutorials.size(); ++i) {
        if (!gTutorials[i].showInMenu) continue;
        if (!firstBtn) ImGui::SameLine();
        firstBtn = false;
        if (ImGui::Button(gTutorials[i].title.c_str())) {
            gActiveTutorial = i;
            gTutorials[i].current = 0;
            ImGui::OpenPopup("##tutorial_modal");
        }
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

    // ---- Tutorial modal (must be at end of Gui, outside children) --
    renderTutorialModal();
}

// ============================================================
// Entry point
// ============================================================

int main(int, char*[])
{
#ifdef ASSETS_LOCATION
    HelloImGui::SetAssetsFolder(ASSETS_LOCATION);
#endif

    registerTutorialActions();

    HelloImGui::SimpleRunnerParams params;
    params.guiFunction = Gui;
    params.windowTitle = "hb-installer";
    params.windowSize  = {800, 620};

    HelloImGui::Run(params);
    return 0;
}
