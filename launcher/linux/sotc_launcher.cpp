// SPDX-License-Identifier: GPL-2.0-or-later
// Shadow of the Colossus launcher for Linux: the settings of launcher/sotc_launcher_win.py in a
// window that works with a gamepad as well as with mouse and keyboard (Steam Deck, Big Picture,
// a couch). SDL3 + Dear ImGui (gpu/third_party/imgui), the same libraries the game uses, so the
// AppImage needs nothing new from the system.
//
//   sotc-launcher            the window
//   sotc-launcher --play     the game with the saved settings, no window (Steam launch options)
//
// Settings: <data>/launcher.ini (data = SOTC_DATA_DIR, the AppImage sets ~/.local/share/sotc-pc).
// They reach sotc.py as environment variables, exactly as the Windows launcher passes them.
// Gamepad: D-pad/stick move, A (✕) selects, B (○) goes back, X (□) clears a folder, LB/RB (L1/R1)
// change tabs, Start (Options) plays. Keyboard: arrows, Enter, Esc, PageUp/PageDown, F11.

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SOTC_VERSION
#define SOTC_VERSION "dev"
#endif

namespace fs = std::filesystem;
extern char** environ;

static constexpr const char* GAME_ID = "CUSA08809";
static constexpr const char* GAME_VERSION = "01.01";
static constexpr size_t MAX_LOG_LINES = 6000;

// ---------------------------------------------------------------------------------------------
// Texts: Portuguese for Portuguese systems, English otherwise (launcher.ini ui_language).

static bool g_pt = true;
static const char* T(const char* pt, const char* en) { return g_pt ? pt : en; }

// ---------------------------------------------------------------------------------------------
// Paths.

static fs::path g_exe_dir, g_port, g_data;

static fs::path home_dir() {
    const char* h = getenv("HOME");
    return h && *h ? fs::path(h) : fs::path("/tmp");
}
static bool is_file(const fs::path& p) { std::error_code e; return fs::is_regular_file(p, e); }
static bool is_dir(const fs::path& p) { std::error_code e; return !p.empty() && fs::is_directory(p, e); }
static bool dir_has_entries(const fs::path& p) {
    std::error_code e;
    if (!fs::is_directory(p, e)) return false;
    return fs::directory_iterator(p, e) != fs::directory_iterator();
}

static void find_paths() {
    char buffer[4096];
    ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    g_exe_dir = n > 0 ? fs::path(std::string(buffer, n)).parent_path() : fs::current_path();
    // out/sotc-launcher (development) and usr/share/sotc-pc/bin/sotc-launcher (AppImage): the
    // port is the parent folder; SOTC_PORT_DIR overrides it.
    if (const char* p = getenv("SOTC_PORT_DIR"); p && *p) g_port = p;
    else g_port = g_exe_dir.parent_path();
    if (!is_file(g_port / "sotc.py")) g_port = g_exe_dir;
    if (const char* d = getenv("SOTC_DATA_DIR"); d && *d) g_data = d;
    else g_data = g_port;
    std::error_code e;
    fs::create_directories(g_data, e);
}

// ---------------------------------------------------------------------------------------------
// Settings (launcher.ini: name=value), the same names as the Windows launcher's settings.json.

struct Settings {
    std::map<std::string, std::string> v;
    std::string get(const std::string& k) const {
        auto it = v.find(k);
        return it == v.end() ? std::string() : it->second;
    }
    bool on(const std::string& k) const { return get(k) == "1"; }
    bool has(const std::string& k) const { return v.count(k) != 0; }
    void set(const std::string& k, const std::string& value) { v[k] = value; }
};
static Settings S;
static bool g_dirty = false;

static const std::pair<const char*, const char*> DEFAULTS[] = {
    {"ui_language", ""}, {"game_dir", ""}, {"sys_modules", ""}, {"user_dir", ""}, {"dlc_dir", ""},
    {"af16", "1"}, {"fps60", "0"}, {"fullscreen", "0"}, {"present_mode", "Mailbox"}, {"language", ""},
    {"player_name", ""}, {"show_fps", "0"}, {"readbacks", ""}, {"extra_env", ""}, {"close_on_play", "0"},
    {"gamepad", ""}, {"output_res", "1920x1080"}, {"upscaler", "fsr1"}, {"sharpness", "250"},
    {"mouse", "1"}, {"mouse_sens", "1.0"}, {"mouse_invert", "0"}, {"neo", "0"}, {"fx_vignette", "1"},
    {"fx_fog", "1"}, {"fx_ssao", "1"}, {"engine_cvars", ""}, {"perf_culling", "1"},
    {"perf_record_thread", "1"}, {"perf_dma", "1"}, {"gpu_submit", "auto"}, {"launcher_fullscreen", "auto"},
    {"setup_done", "0"},
};

static fs::path settings_file() { return g_data / "launcher.ini"; }

static void load_settings() {
    for (auto& [k, value] : DEFAULTS) S.v[k] = value;
    std::ifstream in(settings_file());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        S.v[line.substr(0, eq)] = line.substr(eq + 1);
    }
    // The AppImage's first-run setup (packaging/linux/AppRun) wrote the game and sound folders to
    // sotc.ini: take them over.
    std::ifstream ini(g_data / "sotc.ini");
    while (std::getline(ini, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), value = line.substr(eq + 1);
        if (k == "SOTC_GAME_DIR" && S.get("game_dir").empty()) S.set("game_dir", value);
        if (k == "SOTC_SYS_MODULES" && S.get("sys_modules").empty()) S.set("sys_modules", value);
    }
}

static void save_settings() {
    std::ofstream out(settings_file());
    out << "# Shadow of the Colossus launcher (Linux)\n";
    for (auto& [k, value] : S.v) out << k << '=' << value << '\n';
    g_dirty = false;
}

// ---------------------------------------------------------------------------------------------
// Option lists (launcher/sotc_launcher_win.py).

struct Opt { const char* value; const char* pt; const char* en; };

static const Opt OUTPUTS[] = {
    {"1280x720", "1280 × 720 (16:9, 720p)", "1280 × 720 (16:9, 720p)"},
    {"1920x1080", "1920 × 1080 (16:9, 1080p)", "1920 × 1080 (16:9, 1080p)"},
    {"2560x1440", "2560 × 1440 (16:9, 1440p)", "2560 × 1440 (16:9, 1440p)"},
    {"3840x2160", "3840 × 2160 (16:9, 4K)", "3840 × 2160 (16:9, 4K)"},
    {"1280x800", "1280 × 800 (16:10, Steam Deck)", "1280 × 800 (16:10, Steam Deck)"},
    {"1920x1200", "1920 × 1200 (16:10)", "1920 × 1200 (16:10)"},
    {"2560x1600", "2560 × 1600 (16:10)", "2560 × 1600 (16:10)"},
    {"2560x1080", "2560 × 1080 (21:9)", "2560 × 1080 (21:9)"},
    {"3440x1440", "3440 × 1440 (21:9)", "3440 × 1440 (21:9)"},
    {"3840x1600", "3840 × 1600 (21:9)", "3840 × 1600 (21:9)"},
    {"3840x1080", "3840 × 1080 (32:9)", "3840 × 1080 (32:9)"},
    {"5120x1440", "5120 × 1440 (32:9)", "5120 × 1440 (32:9)"},
};
static const Opt UPSCALERS[] = {
    {"fsr1", "FSR 1 (AMD, qualquer placa)", "FSR 1 (AMD, any GPU)"},
    {"bilinear", "Bilinear (simples)", "Bilinear (plain)"},
};
static const Opt SHARPNESS[] = {
    {"0", "Alta", "High"}, {"250", "Média (padrão)", "Medium (default)"}, {"600", "Baixa", "Low"},
    {"off", "Desligada", "Off"},
};
static const Opt MOUSE_SENS[] = {
    {"0.5", "0,5 (lenta)", "0.5 (slow)"}, {"0.75", "0,75", "0.75"}, {"1.0", "1,0 (padrão)", "1.0 (default)"},
    {"1.5", "1,5", "1.5"}, {"2.0", "2,0 (rápida)", "2.0 (fast)"},
};
static const Opt PRESENT_MODES[] = {
    {"Mailbox", "Mailbox (baixa latência)", "Mailbox (low latency)"},
    {"Fifo", "FIFO (VSync)", "FIFO (VSync)"},
    {"Immediate", "Immediate (pode cortar)", "Immediate (tearing)"},
};
static const Opt LANGUAGES[] = {
    {"", "Automático (o do jogo)", "Automatic (the game's)"},
    {"7", "Português (Portugal)", "Portuguese (Portugal)"}, {"15", "Português (Brasil)", "Portuguese (Brazil)"},
    {"1", "Inglês", "English"}, {"3", "Espanhol", "Spanish"}, {"2", "Francês", "French"},
    {"4", "Alemão", "German"}, {"5", "Italiano", "Italian"}, {"0", "Japonês", "Japanese"},
};
static const Opt READBACKS[] = {
    {"", "Precisas (padrão)", "Precise (default)"}, {"1", "Relaxadas (pode travar)", "Relaxed (may hang)"},
};
static const Opt UI_LANGUAGES[] = {
    {"", "Automático", "Automatic"}, {"pt", "Português", "Portuguese"}, {"en", "Inglês", "English"},
};
static const Opt GPU_SUBMIT[] = {
    {"auto", "Automático (ligado em NVIDIA)", "Automatic (on with NVIDIA)"},
    {"on", "Ligado (AMD/Intel: experimental)", "On (AMD/Intel: experimental)"},
    {"off", "Desligado", "Off"},
};
static const Opt LAUNCHER_FULLSCREEN[] = {
    {"auto", "Automático (tela cheia no Steam Deck)", "Automatic (fullscreen on Steam Deck)"},
    {"1", "Sempre em tela cheia", "Always fullscreen"}, {"0", "Janela", "Window"},
};

struct KeyInput { const char* name; const char* key; const char* pt; const char* en; };
static const KeyInput KEY_INPUTS[] = {
    {"move_up", "W", "Andar para frente", "Move forward"}, {"move_down", "S", "Andar para trás", "Move back"},
    {"move_left", "A", "Andar para a esquerda", "Move left"}, {"move_right", "D", "Andar para a direita", "Move right"},
    {"look_up", "Up", "Câmera para cima", "Camera up"}, {"look_down", "Down", "Câmera para baixo", "Camera down"},
    {"look_left", "Left", "Câmera para a esquerda", "Camera left"}, {"look_right", "Right", "Câmera para a direita", "Camera right"},
    {"cross", "Space", "✕", "✕"}, {"circle", "Left Shift", "○", "○"}, {"square", "E", "□", "□"},
    {"triangle", "Q", "△", "△"}, {"l1", "1", "L1", "L1"}, {"r1", "3", "R1", "R1"}, {"l2", "R", "L2", "L2"},
    {"r2", "F", "R2", "R2"}, {"l3", "Z", "L3", "L3"}, {"r3", "C", "R3", "R3"},
    {"options", "Return", "Options", "Options"}, {"touchpad", "Tab", "Touchpad (esquerda)", "Touchpad (left)"},
    {"touchpad_right", "Backspace", "Touchpad (direita)", "Touchpad (right)"},
    {"up", "I", "Direcional ↑", "D-pad ↑"}, {"down", "K", "Direcional ↓", "D-pad ↓"},
    {"left", "J", "Direcional ←", "D-pad ←"}, {"right", "L", "Direcional →", "D-pad →"},
};

static std::string key_of(const KeyInput& k) {
    std::string name = std::string("key.") + k.name;
    return S.has(name) ? S.get(name) : k.key;
}

// ---------------------------------------------------------------------------------------------
// Game folder facts.

static std::map<std::string, std::string> read_sfo(const fs::path& path) {
    std::map<std::string, std::string> out;
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) { return o + 2 <= d.size() ? unsigned(d[o] | d[o + 1] << 8) : 0u; };
    auto u32 = [&](size_t o) {
        return o + 4 <= d.size() ? uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24) : 0u;
    };
    if (d.size() < 20 || memcmp(d.data(), "\0PSF", 4) != 0) return out;
    uint32_t keys = u32(8), data = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= d.size(); ++i) {
        size_t e = 20 + i * 16;
        unsigned key_off = u16(e), fmt = u16(e + 2);
        uint32_t len = u32(e + 4), data_off = u32(e + 12);
        size_t ko = keys + key_off, dofs = data + data_off;
        if (ko >= d.size() || dofs + len > d.size()) continue;
        std::string key(reinterpret_cast<const char*>(&d[ko]));
        if (fmt == 0x0404) out[key] = std::to_string(u32(dofs));
        else out[key] = std::string(reinterpret_cast<const char*>(&d[dofs]), strnlen(reinterpret_cast<const char*>(&d[dofs]), len));
    }
    return out;
}

struct GameInfo { bool ok = false; std::string id, version, title; };
static GameInfo game_info(const std::string& folder) {
    GameInfo info;
    if (folder.empty() || !is_file(fs::path(folder) / "eboot.bin")) return info;
    info.ok = true;
    auto sfo = read_sfo(fs::path(folder) / "sce_sys" / "param.sfo");
    info.id = sfo.count("TITLE_ID") ? sfo["TITLE_ID"] : "?";
    info.version = sfo.count("APP_VER") ? sfo["APP_VER"] : "?";
    info.title = sfo.count("TITLE") ? sfo["TITLE"] : "Shadow of the Colossus";
    return info;
}

static fs::path user_dir() {
    std::string u = S.get("user_dir");
    fs::path folder = u.empty() ? g_data / "user" : fs::path(u);
    // A folder picked one level too deep (savedata, or a save) goes back up to the data folder.
    std::vector<fs::path> parts(folder.begin(), folder.end());
    for (size_t i = parts.size(); i-- > 0;) {
        std::string lower = parts[i].string();
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "savedata") {
            fs::path up;
            for (size_t j = 0; j < i; ++j) up /= parts[j];
            return up;
        }
    }
    return folder;
}

static bool sound_ready() {
    fs::path m = S.get("sys_modules");
    return !m.empty() && is_file(m / "libSceNgs2.sprx") && is_file(m / "libSceUlt.sprx");
}

static int dlc_count(const std::string& root) {
    if (root.empty() || !is_dir(root)) return 0;
    fs::path base = root;
    if (is_dir(base / GAME_ID)) base /= GAME_ID;
    int n = 0;
    std::error_code e;
    for (auto& entry : fs::directory_iterator(base, e))
        if (entry.is_directory() && is_file(entry.path() / "sce_sys" / "param.sfo")) ++n;
    return n;
}

// ---------------------------------------------------------------------------------------------
// Processes.

static std::vector<std::string> base_environment() {
    std::vector<std::string> env;
    for (char** e = environ; *e; ++e) env.emplace_back(*e);
    return env;
}
static void env_set(std::vector<std::string>& env, const std::string& name, const std::string& value) {
    std::string prefix = name + "=";
    for (auto& item : env)
        if (item.compare(0, prefix.size(), prefix) == 0) { item = prefix + value; return; }
    env.push_back(prefix + value);
}

// Runs argv, returns (exit status, stdout+stderr). Used by the background checks only.
static std::pair<int, std::string> run_capture(const std::vector<std::string>& argv, const fs::path& cwd = {}) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) return {-1, ""};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], 1);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], 2);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    std::string out;
    if (rc == 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) out.append(buf, n);
    }
    close(pipefd[0]);
    if (rc != 0) return {-1, ""};
    int status = 0;
    waitpid(pid, &status, 0);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), out};
}

// ---------------------------------------------------------------------------------------------
// Background checks: graphics card, eboot version, the drive scan for the game.

struct Shared {
    std::mutex m;
    std::string gpu_name;
    int gpu_state = 0;            // 0 checking, 1 found, 2 none, 3 not checked
    std::string eboot_key;        // folder + size + mtime of the eboot last checked
    int eboot_supported = -1;     // -1 checking, -2 unreadable, 0 other version, 1 EU 1.01
    bool scanning = false;
    bool scan_ready = false;
    bool scan_manual = false;
    std::vector<std::string> found_games, found_modules, found_saves;
    std::string found_dlc;
    std::vector<std::pair<std::string, std::string>> picked;  // folder dialog results (key, path)
};
static Shared g_shared;

static void detect_gpu() {
    std::thread([] {
        fs::path tool = g_exe_dir / "bb-gpu-capabilities";
        std::string name;
        int state = 3;
        if (is_file(tool)) {
            auto [code, out] = run_capture({tool.string(), "--live-resolution"});
            std::istringstream lines(out);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.rfind("GPU: ", 0) == 0) {
                    name = line.substr(5);
                    // "<name>, <type>, <n> MiB: ..." -> <name> (the name may hold commas itself).
                    if (size_t colon = name.find(": live"); colon != std::string::npos) name.resize(colon);
                    for (int cut = 0; cut < 2; ++cut)
                        if (size_t c = name.rfind(", "); c != std::string::npos) name.resize(c);
                    break;
                }
            }
            state = !name.empty() ? 1 : code ? 2 : 3;
        }
        std::lock_guard lock(g_shared.m);
        g_shared.gpu_name = name;
        g_shared.gpu_state = state;
    }).detach();
}

static std::string eboot_key(const std::string& game) {
    std::error_code e;
    fs::path eboot = fs::path(game) / "eboot.bin";
    auto size = fs::file_size(eboot, e);
    if (e) return {};
    auto time = fs::last_write_time(eboot, e).time_since_epoch().count();
    return game + "|" + std::to_string(size) + "|" + std::to_string(time);
}

// Whether the eboot holds the EU 1.01 code the modes patch (dumps often keep the base game's
// param.sfo next to an updated eboot): scripts/prepare.py and sotc_modes.py decide.
static void check_eboot(const std::string& game) {
    std::string key = eboot_key(game);
    {
        std::lock_guard lock(g_shared.m);
        if (key.empty() || key == g_shared.eboot_key) return;
        g_shared.eboot_key = key;
        g_shared.eboot_supported = -1;
    }
    std::thread([key, game] {
        static const char* code =
            "import sys; sys.path.insert(0, sys.argv[1])\n"
            "from prepare import parse_self\nfrom sotc_modes import is_supported\n"
            "try:\n    elf, _h, ph, _s, _m = parse_self(open(sys.argv[2], 'rb').read())\n"
            "except Exception:\n    print('UNREADABLE'); raise SystemExit\n"
            "print('SUPPORTED' if is_supported(elf, ph) else 'OTHER')\n";
        auto [rc, out] = run_capture({"python3", "-I", "-c", code, (g_port / "scripts").string(),
                                      (fs::path(game) / "eboot.bin").string()});
        std::lock_guard lock(g_shared.m);
        if (g_shared.eboot_key == key)
            g_shared.eboot_supported = out.find("SUPPORTED") != std::string::npos ? 1
                                     : out.find("OTHER") != std::string::npos ? 0 : -2;
    }).detach();
}

// launcher/sotc_discovery.py walks the likely folders for the game, the sound modules, saves
// and DLC (the Windows launcher's search), with Linux places instead of drive letters.
static void start_scan(bool manual) {
    {
        std::lock_guard lock(g_shared.m);
        if (g_shared.scanning) return;
        g_shared.scanning = true;
        g_shared.scan_manual = manual;
    }
    std::thread([] {
        static const char* code =
            "import sys, os\nfrom pathlib import Path\n"
            "sys.path.insert(0, sys.argv[1]); sys.path.insert(0, sys.argv[2])\n"
            "import sotc_discovery as d\nd.local_drives = lambda: []\n"
            "h = Path.home(); user = os.environ.get('USER', '')\n"
            "roots = [h/'.local/share/shadPS4', h/'.var/app/net.shadps4.shadPS4/data/shadPS4',\n"
            "         h/'.config/shadPS4', Path('/run/media')/user, Path('/media')/user, Path('/mnt')]\n"
            "found = d.scan(seconds=20.0, depth=5, extra_roots=[str(r) for r in roots if r.is_dir()])\n"
            "for g in found['games']: print('GAME\\t' + str(g['path']))\n"
            "for m in found['sys_modules']: print('SYS\\t' + str(m))\n"
            "for s in found['saves']: print('SAVE\\t' + str(s))\n"
            "if found['games']:\n"
            "    dlc = d.find_dlc_root(str(found['games'][0]['path']))\n"
            "    if dlc: print('DLC\\t' + str(dlc))\n";
        auto [rc, out] = run_capture({"python3", "-I", "-c", code, (g_port / "launcher").string(),
                                      (g_port / "scripts").string()});
        std::vector<std::string> games, modules, saves;
        std::string dlc;
        std::istringstream lines(out);
        std::string line;
        while (std::getline(lines, line)) {
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string kind = line.substr(0, tab), path = line.substr(tab + 1);
            if (kind == "GAME") games.push_back(path);
            else if (kind == "SYS") modules.push_back(path);
            else if (kind == "SAVE") saves.push_back(path);
            else if (kind == "DLC") dlc = path;
        }
        std::lock_guard lock(g_shared.m);
        g_shared.found_games = games;
        g_shared.found_modules = modules;
        g_shared.found_saves = saves;
        g_shared.found_dlc = dlc;
        g_shared.scanning = false;
        g_shared.scan_ready = true;
    }).detach();
}

// ---------------------------------------------------------------------------------------------
// Messages at the bottom of the window.

static std::string g_toast;
static double g_toast_until = 0;
static double now_s() { return SDL_GetTicks() / 1000.0; }
static void toast(const std::string& text, double seconds = 5) { g_toast = text; g_toast_until = now_s() + seconds; }

// ---------------------------------------------------------------------------------------------
// The game process (python3 sotc.py), its output in the log page and <data>/last_run.log.

struct Log {
    std::mutex m;
    std::deque<std::string> lines;
    bool changed = false;
    void add(const std::string& line) {
        std::lock_guard lock(m);
        lines.push_back(line);
        while (lines.size() > MAX_LOG_LINES) lines.pop_front();
        changed = true;
    }
    void clear() { std::lock_guard lock(m); lines.clear(); changed = true; }
    std::string text() {
        std::lock_guard lock(m);
        std::string all;
        for (auto& l : lines) { all += l; all += '\n'; }
        return all;
    }
};
static Log g_log;

struct GameProcess {
    pid_t pid = 0;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> in_game{false};
    std::atomic<bool> finished{false};
    std::atomic<int> exit_code{0};
    double stop_requested = 0;
};
static GameProcess g_game;

static std::vector<std::string> game_environment() {
    auto env = base_environment();
    fs::path user = user_dir();
    env_set(env, "SOTC_DATA_DIR", g_data.string());
    env_set(env, "SOTC_GAME_DIR", S.get("game_dir"));
    env_set(env, "SOTC_USER_DIR", user.string());
    if (!S.get("sys_modules").empty()) env_set(env, "SOTC_SYS_MODULES", S.get("sys_modules"));
    if (!S.get("dlc_dir").empty()) env_set(env, "SOTC_DLC_DIR", S.get("dlc_dir"));
    std::string modes;
    for (const char* m : {"af16", "fps60"})
        if (S.on(m)) modes += (modes.empty() ? "" : ",") + std::string(m);
    env_set(env, "SOTC_MODES", modes);
    env_set(env, "BB_FULLSCREEN", S.on("fullscreen") ? "1" : "0");
    env_set(env, "BB_PRESENT_MODE", S.get("present_mode"));
    if (!S.get("language").empty()) env_set(env, "BB_LANGUAGE", S.get("language"));
    if (!S.get("player_name").empty()) env_set(env, "BB_USER_NAME", S.get("player_name"));
    if (S.on("show_fps")) env_set(env, "SOTC_FPS", "1");
    if (!S.get("readbacks").empty()) env_set(env, "BB_READBACKS", S.get("readbacks"));
    std::string cvars;
    auto add_cvars = [&](const std::string& c) { if (!c.empty()) cvars += (cvars.empty() ? "" : " ") + c; };
    if (!S.on("fx_vignette")) add_cvars("r_vignetteOverrideEnable=1 r_vignetteEnable=0");
    if (!S.on("fx_fog")) add_cvars("fog_enable=0");
    if (!S.on("fx_ssao")) add_cvars("AO_Mode=0");
    if (S.on("perf_culling")) add_cvars("doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0");
    add_cvars(S.get("engine_cvars"));
    env_set(env, "SOTC_CVARS", cvars);
    env_set(env, "SOTC_RECORD_THREAD", S.on("perf_record_thread") ? "1" : "0");
    if (!S.on("perf_dma")) env_set(env, "BB_DIRECT_MEMORY_ACCESS", "0");
    if (S.get("gpu_submit") == "on") { env_set(env, "SOTC_FLUSH_US", "1000"); env_set(env, "SOTC_RB_AHEAD", "1"); }
    else if (S.get("gpu_submit") == "off") { env_set(env, "SOTC_FLUSH_EVERY", "0"); env_set(env, "SOTC_RB_AHEAD", "0"); }
    env_set(env, "SOTC_NEO", S.on("neo") ? "1" : "0");
    std::string output = S.get("output_res").empty() ? "1920x1080" : S.get("output_res");
    env_set(env, "SOTC_OUTPUT_RES", output);
    // Outputs wider than 16:9 render at that aspect (a wider view); 16:9 and narrower render at
    // 1080p and the upscaler takes it to the output (sotc_launcher_win.py).
    int w = 16, h = 9;
    if (sscanf(output.c_str(), "%dx%d", &w, &h) != 2) { w = 16; h = 9; }
    if (w * 9 > h * 16 * 1.01 && !S.on("neo")) env_set(env, "SOTC_RENDER_RES", output);
    env_set(env, "BB_FSR1", S.get("upscaler") == "bilinear" ? "0" : "1");
    if (S.get("sharpness") == "off") env_set(env, "BB_RCAS", "0");
    else { env_set(env, "BB_RCAS", "1"); env_set(env, "BB_RCAS_ATTENUATION", S.get("sharpness").empty() ? "250" : S.get("sharpness")); }
    env_set(env, "SOTC_MOUSE", S.on("mouse") ? "1" : "0");
    env_set(env, "SOTC_MOUSE_SENS", S.get("mouse_sens").empty() ? "1.0" : S.get("mouse_sens"));
    env_set(env, "SOTC_MOUSE_INVERT", S.on("mouse_invert") ? "1" : "0");
    if (!S.get("gamepad").empty()) env_set(env, "BB_GAMEPAD", S.get("gamepad"));
    env_set(env, "BB_CONFIG", (g_data / "sotc.ini").string());
    std::istringstream extra(S.get("extra_env"));
    std::string item;
    while (extra >> item)
        if (size_t eq = item.find('='); eq != std::string::npos) env_set(env, item.substr(0, eq), item.substr(eq + 1));
    env_set(env, "PYTHONUNBUFFERED", "1");
    env_set(env, "PYTHONIOENCODING", "utf-8");
    return env;
}

// The launcher's key bindings as key.<input>= lines of sotc.ini (BB_CONFIG); other lines stay.
static void write_key_bindings() {
    fs::path ini = g_data / "sotc.ini";
    std::vector<std::string> keep;
    std::ifstream in(ini);
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("key.", 0) != 0 && line != "# key bindings (launcher)") keep.push_back(line);
    in.close();
    std::vector<std::string> custom;
    for (auto& [k, value] : S.v)
        if (k.rfind("key.", 0) == 0) custom.push_back(k + "=" + value);
    if (!custom.empty()) {
        keep.push_back("# key bindings (launcher)");
        keep.insert(keep.end(), custom.begin(), custom.end());
    }
    std::ofstream out(ini);
    for (auto& l : keep) out << l << '\n';
}

static constexpr int LOGS_KEPT = 5, CRASH_LOGS_KEPT = 20;

static FILE* open_session_log() {
    std::error_code e;
    for (int i = LOGS_KEPT - 1; i > 0; --i) {
        fs::path older = g_data / (i == 1 ? std::string("last_run.log") : "last_run." + std::to_string(i - 1) + ".log");
        if (is_file(older)) fs::rename(older, g_data / ("last_run." + std::to_string(i) + ".log"), e);
    }
    return fopen((g_data / "last_run.log").c_str(), "w");
}

static void keep_crash_log(int code) {
    std::error_code e;
    fs::path crashes = g_data / "crashes";
    fs::create_directories(crashes, e);
    char stamp[64];
    time_t t = time(nullptr);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", localtime(&t));
    fs::copy_file(g_data / "last_run.log", crashes / (std::string(stamp) + "_" + std::to_string(code) + ".log"),
                  fs::copy_options::overwrite_existing, e);
    std::vector<fs::path> logs;
    for (auto& entry : fs::directory_iterator(crashes, e))
        if (entry.path().extension() == ".log") logs.push_back(entry.path());
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i + CRASH_LOGS_KEPT < logs.size(); ++i) fs::remove(logs[i], e);
}

static bool start_game() {
    if (g_game.running) return false;
    save_settings();
    write_key_bindings();
    g_log.clear();
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], 1);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], 2);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addchdir_np(&actions, g_data.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);  // Stop ends python and bb-probe
    posix_spawnattr_setpgroup(&attr, 0);
    std::string script = (g_port / "sotc.py").string();
    std::vector<std::string> argv = {"python3", script};
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    auto env = game_environment();
    std::vector<char*> envp;
    for (auto& item : env) envp.push_back(const_cast<char*>(item.c_str()));
    envp.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, "python3", &actions, &attr, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        g_log.add(std::string(T("Não foi possível iniciar o python3: ", "Could not start python3: ")) + strerror(rc));
        toast(T("Python 3 não encontrado. Instale o python3 e tente de novo.",
                "Python 3 not found. Install python3 and try again."));
        return false;
    }
    g_game.pid = pid;
    g_game.running = true;
    g_game.stopping = false;
    g_game.in_game = false;
    g_game.finished = false;
    g_game.stop_requested = 0;
    std::thread([fd = pipefd[0], pid] {
        FILE* log = open_session_log();
        std::string pending;
        char buf[8192];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            if (log) { fwrite(buf, 1, n, log); fflush(log); }
            pending.append(buf, n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.find("Entering original x86-64 code") != std::string::npos) g_game.in_game = true;
                g_log.add(line);
                pending.erase(0, nl + 1);
            }
        }
        if (!pending.empty()) g_log.add(pending);
        close(fd);
        int status = 0;
        waitpid(pid, &status, 0);
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (log) { fprintf(log, "[exit code %d]\n", code); fclose(log); }
        if (code != 0 && !g_game.stopping) keep_crash_log(code);
        g_log.add(std::string(T("— o jogo terminou (código ", "— the game exited (code ")) + std::to_string(code) + ") —");
        g_game.exit_code = code;
        g_game.running = false;
        g_game.finished = true;
    }).detach();
    return true;
}

static void stop_game() {
    if (!g_game.running || g_game.pid <= 0) return;
    g_game.stopping = true;
    g_game.stop_requested = now_s();
    kill(-g_game.pid, SIGTERM);
}

// --play: the game with the saved settings and no window (sotc.py replaces this process).
static int play_without_window(int argc, char** argv, int first) {
    if (!game_info(S.get("game_dir")).ok) {
        fprintf(stderr, "%s\n", T("Escolha a pasta do jogo no lançador primeiro.", "Choose the game folder in the launcher first."));
        return 1;
    }
    write_key_bindings();
    auto env = game_environment();
    std::vector<char*> envp;
    for (auto& item : env) envp.push_back(const_cast<char*>(item.c_str()));
    envp.push_back(nullptr);
    std::string script = (g_port / "sotc.py").string();
    // Arguments after --play go to sotc.py (and from it to bb-probe).
    std::vector<char*> args = {const_cast<char*>("python3"), script.data()};
    for (int i = first; i < argc; ++i) args.push_back(argv[i]);
    args.push_back(nullptr);
    if (chdir(g_data.c_str())) {}
    execvpe("python3", args.data(), envp.data());
    fprintf(stderr, "python3: %s\n", strerror(errno));
    return 127;
}

// ---------------------------------------------------------------------------------------------
// Actions.

static void open_folder(const fs::path& path) {
    std::error_code e;
    fs::create_directories(path, e);
    SDL_OpenURL(("file://" + path.string()).c_str());
}

static void import_shadps4_save() {
    std::vector<fs::path> candidates;
    {
        std::lock_guard lock(g_shared.m);
        for (auto& s : g_shared.found_saves) candidates.emplace_back(s);
    }
    for (fs::path root : {home_dir() / ".local/share/shadPS4", home_dir() / ".var/app/net.shadps4.shadPS4/data/shadPS4"}) {
        std::error_code e;
        if (!is_dir(root / "savedata")) continue;
        for (auto& user : fs::directory_iterator(root / "savedata", e))
            if (dir_has_entries(user.path() / GAME_ID)) candidates.push_back(user.path() / GAME_ID);
        if (dir_has_entries(root / "savedata" / GAME_ID)) candidates.push_back(root / "savedata" / GAME_ID);
    }
    if (candidates.empty()) {
        toast(T("Nenhum save do shadPS4 encontrado (~/.local/share/shadPS4/savedata).",
                "No shadPS4 save found (~/.local/share/shadPS4/savedata)."));
        return;
    }
    fs::path source = candidates.front();
    fs::path target = user_dir() / "savedata" / "1" / GAME_ID;
    std::error_code e;
    if (dir_has_entries(target)) {
        fs::path backup = target;
        backup += ".bak";
        fs::remove_all(backup, e);
        fs::rename(target, backup, e);
    }
    fs::create_directories(target, e);
    fs::copy(source, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, e);
    if (e) toast(std::string(T("Não foi possível importar o save: ", "Could not import the save: ")) + e.message());
    else toast(std::string(T("Save importado de ", "Save imported from ")) + source.string());
}

static void clear_shader_cache() {
    std::error_code e;
    for (const char* name : {"cache", "shader"}) fs::remove_all(user_dir() / name, e);
    toast(T("Cache de shaders limpo. A próxima partida engasga alguns minutos enquanto ele é refeito.",
            "Shader cache cleared. The next start stutters for a few minutes while it is rebuilt."));
}

// A menu entry (~/.local/share/applications), which Steam also lists under "Add a Non-Steam Game".
static void desktop_shortcut() {
    std::error_code e;
    fs::path apps = home_dir() / ".local/share/applications";
    fs::create_directories(apps, e);
    fs::path icon = g_data / "sotc-pc.png";
    fs::path game_icon = fs::path(S.get("game_dir")) / "sce_sys" / "icon0.png";
    if (!S.get("game_dir").empty() && is_file(game_icon)) fs::copy_file(game_icon, icon, fs::copy_options::overwrite_existing, e);
    else if (is_file(g_port / "launcher" / "sotc.png")) fs::copy_file(g_port / "launcher" / "sotc.png", icon, fs::copy_options::overwrite_existing, e);
    const char* appimage = getenv("APPIMAGE");
    std::string exec = appimage && *appimage ? appimage : (g_exe_dir / "sotc-launcher").string();
    std::ofstream out(apps / "sotc-pc.desktop");
    out << "[Desktop Entry]\nType=Application\nName=Shadow of the Colossus\n"
        << "Comment=" << T("Port nativo para Linux (precisa do seu dump do jogo)", "Native Linux port (your own game dump required)") << "\n"
        << "Exec=\"" << exec << "\"\nIcon=" << icon.string() << "\nCategories=Game;\nTerminal=false\n";
    out.close();
    toast(e || !out ? T("Não foi possível criar o atalho.", "Could not create the shortcut.")
                    : T("Atalho criado no menu de aplicativos. Na Steam: Adicionar jogo › Adicionar um jogo não-Steam.",
                        "Shortcut added to the applications menu. In Steam: Add a Game › Add a Non-Steam Game."), 7);
}

static void SDLCALL folder_picked(void* userdata, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard lock(g_shared.m);
    g_shared.picked.emplace_back(static_cast<const char*>(userdata), files[0]);
}

static SDL_Window* g_window = nullptr;
static void pick_folder(const char* key, const char* title) {
    (void)title;
    std::string current = S.get(key);
    std::string start = !current.empty() && is_dir(current) ? current : home_dir().string();
    SDL_ShowOpenFolderDialog(folder_picked, const_cast<char*>(key), g_window, start.c_str(), false);
}

// ---------------------------------------------------------------------------------------------
// Look: Steam's colours.

static ImU32 C(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
static const ImU32 BG_TOP = C(27, 40, 56), BG_BOTTOM = C(14, 20, 27), HEADER = C(23, 26, 33),
                   ROW = C(255, 255, 255, 7), ROW_HOVER = C(42, 63, 84), ROW_FOCUS = C(46, 82, 110),
                   TEXT = C(199, 213, 224), TEXT_HI = C(255, 255, 255), MUTED = C(143, 152, 160),
                   ACCENT = C(26, 159, 255), ACCENT_SOFT = C(102, 192, 244), GREEN_TOP = C(117, 176, 34),
                   GREEN_BOTTOM = C(84, 133, 25), OK = C(164, 208, 7), WARN = C(230, 190, 70),
                   ERR = C(222, 108, 92), CARD = C(16, 24, 33, 220);

static float g_scale = 1.0f;
static float px(float v) { return v * g_scale; }

static void apply_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(0, 0);
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.FrameRounding = 6;
    s.FramePadding = ImVec2(12, 8);
    s.ItemSpacing = ImVec2(10, 8);
    s.ScrollbarSize = 10;
    s.ScrollbarRounding = 6;
    s.ChildRounding = 8;
    s.PopupRounding = 8;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(TEXT);
    c[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(MUTED);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ImGui::ColorConvertU32ToFloat4(C(10, 16, 22));
    c[ImGuiCol_FrameBgHovered] = ImGui::ColorConvertU32ToFloat4(C(18, 28, 38));
    c[ImGuiCol_FrameBgActive] = ImGui::ColorConvertU32ToFloat4(C(18, 28, 38));
    c[ImGuiCol_Button] = ImGui::ColorConvertU32ToFloat4(C(61, 68, 80));
    c[ImGuiCol_ButtonHovered] = ImGui::ColorConvertU32ToFloat4(C(80, 100, 120));
    c[ImGuiCol_ButtonActive] = ImGui::ColorConvertU32ToFloat4(ACCENT);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImGui::ColorConvertU32ToFloat4(C(61, 68, 80));
    c[ImGuiCol_NavCursor] = ImGui::ColorConvertU32ToFloat4(ACCENT_SOFT);
    c[ImGuiCol_TextSelectedBg] = ImGui::ColorConvertU32ToFloat4(C(26, 159, 255, 110));
    s.ScaleAllSizes(g_scale);
}

// ---------------------------------------------------------------------------------------------
// Gamepad glyphs (PlayStation shapes or Xbox letters, following the connected gamepad).

enum class Glyph { South, East, West, North, LB, RB, Start, Select };
enum class PadFamily { None, PlayStation, Xbox, Nintendo };
static PadFamily g_family = PadFamily::None;
static std::string g_pad_name;

static void refresh_gamepads() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    g_family = PadFamily::None;
    g_pad_name.clear();
    if (ids && count > 0) {
        SDL_GamepadType type = SDL_GetGamepadTypeForID(ids[0]);
        const char* name = SDL_GetGamepadNameForID(ids[0]);
        g_pad_name = name ? name : "Gamepad";
        switch (type) {
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: g_family = PadFamily::PlayStation; break;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            g_family = PadFamily::Nintendo; break;
        default: g_family = PadFamily::Xbox; break;
        }
    }
    SDL_free(ids);
}

// Draws one glyph at pos (top-left), returns its width.
static float draw_glyph(ImDrawList* dl, ImVec2 pos, Glyph g, float h) {
    ImFont* font = ImGui::GetFont();
    const float r = h * 0.5f;
    const ImVec2 c(pos.x + r, pos.y + r);
    auto text_center = [&](const char* t, ImU32 col, float size) {
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, t);
        dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, t);
    };
    auto pill = [&](const char* t) {
        float size = h * 0.56f;
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, t);
        float w = std::max(h * 1.35f, ts.x + h * 0.6f);
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), C(61, 68, 80), h * 0.35f);
        dl->AddText(font, size, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), TEXT_HI, t);
        return w;
    };
    if (g == Glyph::LB || g == Glyph::RB) {
        bool ps = g_family == PadFamily::PlayStation;
        return pill(g == Glyph::LB ? (ps ? "L1" : g_family == PadFamily::Nintendo ? "L" : "LB")
                                   : (ps ? "R1" : g_family == PadFamily::Nintendo ? "R" : "RB"));
    }
    if (g == Glyph::Start) return pill(g_family == PadFamily::PlayStation ? "Options" : g_family == PadFamily::Nintendo ? "+" : "Menu");
    if (g == Glyph::Select) return pill(g_family == PadFamily::PlayStation ? "Share" : g_family == PadFamily::Nintendo ? "−" : "View");
    dl->AddCircleFilled(c, r, C(48, 54, 64), 24);
    if (g_family == PadFamily::PlayStation) {
        const float s = r * 0.42f, t = std::max(1.5f, h * 0.09f);
        switch (g) {
        case Glyph::South:  // cross
            dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), C(124, 178, 232), t);
            dl->AddLine(ImVec2(c.x + s, c.y - s), ImVec2(c.x - s, c.y + s), C(124, 178, 232), t);
            break;
        case Glyph::East: dl->AddCircle(c, s * 1.05f, C(255, 102, 102), 20, t); break;
        case Glyph::West:
            dl->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), C(255, 105, 248), 0, 0, t);
            break;
        case Glyph::North:
            dl->AddTriangle(ImVec2(c.x, c.y - s * 1.1f), ImVec2(c.x + s * 1.1f, c.y + s * 0.8f),
                            ImVec2(c.x - s * 1.1f, c.y + s * 0.8f), C(64, 226, 160), t);
            break;
        default: break;
        }
    } else {
        // Xbox: A south, B east, X west, Y north. Nintendo: B south, A east, Y west, X north.
        bool nin = g_family == PadFamily::Nintendo;
        const char* label = g == Glyph::South ? (nin ? "B" : "A") : g == Glyph::East ? (nin ? "A" : "B")
                          : g == Glyph::West ? (nin ? "Y" : "X") : (nin ? "X" : "Y");
        ImU32 col = nin ? TEXT_HI : g == Glyph::South ? C(96, 196, 72) : g == Glyph::East ? C(230, 80, 70)
                  : g == Glyph::West ? C(70, 140, 240) : C(240, 200, 60);
        text_center(label, col, h * 0.62f);
    }
    return h;
}

static void draw_key_cap(ImDrawList* dl, ImVec2 pos, const char* key, float h, float* width) {
    ImFont* font = ImGui::GetFont();
    float size = h * 0.56f;
    ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, key);
    float w = std::max(h, ts.x + h * 0.6f);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), C(120, 130, 140), h * 0.2f, 0, 1.5f);
    dl->AddText(font, size, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), TEXT, key);
    *width = w;
}

// ---------------------------------------------------------------------------------------------
// Rows: one setting per full-width row, as in Steam's settings. A gamepad moves between rows;
// A acts on the row, left/right change a choice, X clears a folder.

static bool g_focus_first = false;
static bool g_nav_visible() { return ImGui::GetCurrentContext()->NavCursorVisible; }

struct RowGeom { ImVec2 min, max; float w, h, text_w, control_x; };

static float hint_size() { return ImGui::GetFontSize() * 0.84f; }

static RowGeom row_layout(const char* hint, const char* label = nullptr) {
    RowGeom g;
    g.w = ImGui::GetContentRegionAvail().x;
    float pad_x = px(18), pad_y = px(12);
    // Rows with a description keep a fixed control column; plain rows give the control all the
    // room the label leaves.
    float control_w = std::min(g.w * 0.44f, px(440));
    if (label && !(hint && *hint))
        control_w = std::max(control_w, g.w - pad_x * 3 - ImGui::CalcTextSize(label).x - px(24));
    g.text_w = g.w - control_w - pad_x * 3;
    float h = pad_y * 2 + ImGui::GetTextLineHeight();
    if (hint && *hint)
        h += px(4) + ImGui::GetFont()->CalcTextSizeA(hint_size(), FLT_MAX, g.text_w, hint).y;
    g.h = std::max(h, px(60));
    g.min = ImGui::GetCursorScreenPos();
    g.max = ImVec2(g.min.x + g.w, g.min.y + g.h);
    g.control_x = g.max.x - pad_x - control_w;
    return g;
}

static void row_background(const RowGeom& g, bool focused, bool hovered) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool show_focus = focused && g_nav_visible();
    dl->AddRectFilled(g.min, g.max, show_focus ? ROW_FOCUS : hovered ? ROW_HOVER : ROW, px(6));
    if (show_focus)
        dl->AddRectFilled(g.min, ImVec2(g.min.x + px(4), g.max.y), ACCENT, px(6), ImDrawFlags_RoundCornersLeft);
}

static void row_texts(const RowGeom& g, const char* label, const char* hint, bool focused) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float pad_x = px(18), pad_y = px(12);
    float line = ImGui::GetTextLineHeight();
    float total = line + (hint && *hint ? px(4) + ImGui::GetFont()->CalcTextSizeA(hint_size(), FLT_MAX, g.text_w, hint).y : 0);
    float y = g.min.y + std::max(pad_y, (g.h - total) * 0.5f);
    dl->AddText(ImVec2(g.min.x + pad_x, y), focused && g_nav_visible() ? TEXT_HI : TEXT, label);
    if (hint && *hint)
        dl->AddText(ImGui::GetFont(), hint_size(), ImVec2(g.min.x + pad_x, y + line + px(4)), MUTED, hint, nullptr, g.text_w);
}

static void row_gap() { ImGui::Dummy(ImVec2(0, px(4))); }

static void focus_first_here() {
    if (g_focus_first) {
        ImGui::SetKeyboardFocusHere();
        g_focus_first = false;
    }
}

// Upper case for section titles, with the accented letters Portuguese uses.
static std::string to_upper(const std::string& in) {
    static const std::pair<const char*, const char*> accents[] = {
        {"á", "Á"}, {"à", "À"}, {"â", "Â"}, {"ã", "Ã"}, {"ç", "Ç"}, {"é", "É"}, {"ê", "Ê"},
        {"í", "Í"}, {"ó", "Ó"}, {"ô", "Ô"}, {"õ", "Õ"}, {"ú", "Ú"}, {"ü", "Ü"}};
    std::string out;
    for (size_t i = 0; i < in.size();) {
        bool done = false;
        for (auto& [lower, upper] : accents) {
            size_t n = strlen(lower);
            if (in.compare(i, n, lower) == 0) { out += upper; i += n; done = true; break; }
        }
        if (!done) out += (char)toupper((unsigned char)in[i++]);
    }
    return out;
}

static void section(const char* title) {
    ImGui::Dummy(ImVec2(0, px(14)));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ACCENT_SOFT));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.86f);
    std::string upper = to_upper(title);
    ImGui::TextUnformatted(upper.c_str());
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, px(2)));
}

static void note(const char* text) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.86f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(MUTED));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// The row's own button: returns pressed; focused/hovered out.
static bool row_button(const char* id, const RowGeom& g, bool* focused, bool* hovered) {
    focus_first_here();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(g.w, g.h), ImGuiButtonFlags_EnableNav);
    *focused = ImGui::IsItemFocused();
    *hovered = ImGui::IsItemHovered();
    return pressed;
}

static void claim_keys(std::initializer_list<ImGuiKey> keys) {
    ImGuiID id = ImGui::GetItemID();
    for (ImGuiKey k : keys) ImGui::SetKeyOwner(k, id);
}
static bool owned_pressed(ImGuiKey k) { return ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, ImGui::GetItemID()); }

static void toggle_row(const char* key, const char* label, const char* hint = nullptr) {
    ImGui::PushID(key);
    RowGeom g = row_layout(hint);
    bool focused, hovered;
    if (row_button("row", g, &focused, &hovered)) { S.set(key, S.on(key) ? "0" : "1"); g_dirty = true; }
    row_background(g, focused, hovered);
    row_texts(g, label, hint, focused);
    // A switch on the right.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool on = S.on(key);
    float sh = px(26), sw = px(48);
    ImVec2 p(g.max.x - px(18) - sw, g.min.y + (g.h - sh) * 0.5f);
    dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sh), on ? ACCENT : C(61, 68, 80), sh * 0.5f);
    float knob = on ? p.x + sw - sh * 0.5f : p.x + sh * 0.5f;
    dl->AddCircleFilled(ImVec2(knob, p.y + sh * 0.5f), sh * 0.5f - px(3), TEXT_HI, 24);
    ImGui::PopID();
    row_gap();
}

static void choice_row(const char* key, const Opt* opts, int count, const char* label, const char* hint = nullptr,
                       const std::vector<std::string>* extra = nullptr) {
    ImGui::PushID(key);
    // Options: the fixed list, or (extra) a list of plain values after the first fixed entry.
    std::vector<std::pair<std::string, std::string>> values;
    for (int i = 0; i < count; ++i) values.emplace_back(opts[i].value, T(opts[i].pt, opts[i].en));
    if (extra) for (auto& e : *extra) values.emplace_back(e, e);
    std::string cur = S.get(key);
    int index = -1;
    for (size_t i = 0; i < values.size(); ++i) if (values[i].first == cur) index = (int)i;
    if (index < 0) {
        if (extra && !cur.empty()) { values.emplace_back(cur, cur); index = (int)values.size() - 1; }
        else index = 0;
    }
    RowGeom g = row_layout(hint, label);
    bool focused, hovered;
    bool pressed = row_button("row", g, &focused, &hovered);
    int step = 0;
    if (pressed) {
        // A mouse click on the left arrow goes back, anywhere else forward (A also forward).
        step = ImGui::GetIO().MousePos.x < g.control_x + px(40) && ImGui::IsItemHovered() ? -1 : 1;
    }
    if (focused) {
        claim_keys({ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft,
                    ImGuiKey_GamepadLStickRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow});
        if (owned_pressed(ImGuiKey_GamepadDpadLeft) || owned_pressed(ImGuiKey_GamepadLStickLeft) || owned_pressed(ImGuiKey_LeftArrow)) step = -1;
        if (owned_pressed(ImGuiKey_GamepadDpadRight) || owned_pressed(ImGuiKey_GamepadLStickRight) || owned_pressed(ImGuiKey_RightArrow)) step = 1;
    }
    if (step) {
        int n = (int)values.size();
        index = ((index + step) % n + n) % n;
        S.set(key, values[index].first);
        g_dirty = true;
    }
    row_background(g, focused, hovered);
    row_texts(g, label, hint, focused);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string& text = values[index].second;
    float right = g.max.x - px(18);
    float y = g.min.y + (g.h - ImGui::GetTextLineHeight()) * 0.5f;
    ImU32 arrow = focused && g_nav_visible() ? ACCENT_SOFT : C(90, 100, 110);
    float aw = ImGui::CalcTextSize("›").x;
    dl->AddText(ImVec2(right - aw, y), arrow, "›");
    float avail = right - aw * 2 - px(28) - g.control_x;
    std::string shown = text;
    while (shown.size() > 4 && ImGui::CalcTextSize(shown.c_str()).x > avail) shown = shown.substr(0, shown.size() - 4) + "…";
    float tw = ImGui::CalcTextSize(shown.c_str()).x;
    float tx = right - aw - px(14) - tw;
    dl->AddText(ImVec2(tx, y), focused && g_nav_visible() ? TEXT_HI : TEXT, shown.c_str());
    dl->AddText(ImVec2(tx - px(14) - aw, y), arrow, "‹");
    ImGui::PopID();
    row_gap();
}

static std::string elide_left(const std::string& s, float width) {
    if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
    std::string t = s;
    while (t.size() > 4 && ImGui::CalcTextSize(("…" + t).c_str()).x > width) t.erase(0, 1);
    return "…" + t;
}

static void folder_row(const char* key, const char* label, const char* hint, const char* empty_text = nullptr) {
    ImGui::PushID(key);
    RowGeom g = row_layout(hint, label);
    bool focused, hovered;
    if (row_button("row", g, &focused, &hovered)) pick_folder(key, label);
    if (focused) {
        claim_keys({ImGuiKey_GamepadFaceLeft, ImGuiKey_Delete});
        if ((owned_pressed(ImGuiKey_GamepadFaceLeft) || owned_pressed(ImGuiKey_Delete)) && !S.get(key).empty()) {
            S.set(key, "");
            g_dirty = true;
        }
    }
    row_background(g, focused, hovered);
    row_texts(g, label, hint, focused);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string value = S.get(key);
    bool missing = !value.empty() && !is_dir(value);
    std::string shown = value.empty() ? (empty_text ? empty_text : T("Escolher…", "Choose…"))
                                      : elide_left(value, g.max.x - px(18) - g.control_x);
    float tw = ImGui::CalcTextSize(shown.c_str()).x;
    float y = g.min.y + (g.h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(g.max.x - px(18) - tw, y), value.empty() ? MUTED : missing ? ERR : TEXT, shown.c_str());
    ImGui::PopID();
    row_gap();
}

static void text_row(const char* key, const char* label, const char* hint = nullptr) {
    ImGui::PushID(key);
    RowGeom g = row_layout(hint);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImVec2 start = ImGui::GetCursorScreenPos();
    float field_w = g.max.x - px(18) - g.control_x;
    float field_h = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(g.control_x, g.min.y + (g.h - field_h) * 0.5f));
    focus_first_here();
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", S.get(key).c_str());
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::InputText("##v", buf, sizeof(buf))) { S.set(key, buf); g_dirty = true; }
    bool focused = ImGui::IsItemFocused() || ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();
    row_texts(g, label, hint, focused);
    dl->ChannelsSetCurrent(0);
    row_background(g, focused, hovered);
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(g.w, g.h));
    ImGui::PopID();
    row_gap();
}

static bool action_row(const char* id, const char* label, const char* hint = nullptr, const char* value = "›") {
    ImGui::PushID(id);
    RowGeom g = row_layout(hint, label);
    bool focused, hovered;
    bool pressed = row_button("row", g, &focused, &hovered);
    row_background(g, focused, hovered);
    row_texts(g, label, hint, focused);
    float tw = ImGui::CalcTextSize(value).x;
    float y = g.min.y + (g.h - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::GetWindowDrawList()->AddText(ImVec2(g.max.x - px(18) - tw, y), focused && g_nav_visible() ? ACCENT_SOFT : MUTED, value);
    ImGui::PopID();
    row_gap();
    return pressed;
}

// ---------------------------------------------------------------------------------------------
// Pages.

enum Page { PAGE_PLAY, PAGE_GAME, PAGE_DISPLAY, PAGE_CONTROLS, PAGE_ADVANCED, PAGE_LOG, PAGE_COUNT };
static int g_page = PAGE_PLAY;
static const char* page_title(int p) {
    switch (p) {
    case PAGE_PLAY: return T("Jogar", "Play");
    case PAGE_GAME: return T("Jogo e saves", "Game & saves");
    case PAGE_DISPLAY: return T("Imagem e FPS", "Display & FPS");
    case PAGE_CONTROLS: return T("Controles", "Controls");
    case PAGE_ADVANCED: return T("Avançado", "Advanced");
    default: return T("Registro", "Log");
    }
}
static void go_page(int p) {
    if (p == g_page) return;
    g_page = p;
    g_focus_first = true;
    if (g_dirty) save_settings();
}

static std::string g_capture_key;   // key binding being captured ("" = none)

static SDL_Renderer* g_renderer = nullptr;
struct Texture { SDL_Texture* tex = nullptr; int w = 0, h = 0; std::string source; };
static Texture g_banner;

static SDL_Surface* load_png_surface(const fs::path& path, std::vector<unsigned char>* keep) {
    int w, h, n;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data) return nullptr;
    keep->assign(data, data + size_t(w) * h * 4);
    stbi_image_free(data);
    return SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, keep->data(), w * 4);
}

static void refresh_banner() {
    fs::path art = fs::path(S.get("game_dir")) / "sce_sys" / "pic1.png";
    std::string source = S.get("game_dir").empty() ? "" : art.string();
    if (source == g_banner.source) return;
    if (g_banner.tex) SDL_DestroyTexture(g_banner.tex);
    g_banner = Texture();
    g_banner.source = source;
    if (source.empty() || !is_file(art)) return;
    std::vector<unsigned char> pixels;
    if (SDL_Surface* s = load_png_surface(art, &pixels)) {
        g_banner.tex = SDL_CreateTextureFromSurface(g_renderer, s);
        g_banner.w = s->w;
        g_banner.h = s->h;
        SDL_DestroySurface(s);
    }
}

static void set_window_icon() {
    std::vector<unsigned char> pixels;
    fs::path icon = fs::path(S.get("game_dir")) / "sce_sys" / "icon0.png";
    SDL_Surface* s = !S.get("game_dir").empty() && is_file(icon) ? load_png_surface(icon, &pixels) : nullptr;
    if (!s) s = load_png_surface(g_port / "launcher" / "sotc.png", &pixels);
    if (s) { SDL_SetWindowIcon(g_window, s); SDL_DestroySurface(s); }
}

static std::string summary() {
    std::string out = S.on("fps60") ? T("60 FPS", "60 FPS") : T("30 FPS (como no PS4)", "30 FPS (as on PS4)");
    std::string res = S.get("output_res");
    if (size_t x = res.find('x'); x != std::string::npos) res = res.substr(0, x) + " × " + res.substr(x + 1);
    out += "  ·  " + res;
    out += S.get("upscaler") == "bilinear" ? "  ·  bilinear" : "  ·  FSR 1";
    if (S.on("af16")) out += T("  ·  AF 16x", "  ·  16x AF");
    if (S.on("neo")) out += "  ·  PS4 Pro";
    out += S.on("fullscreen") ? T("  ·  tela cheia", "  ·  fullscreen") : T("  ·  janela", "  ·  window");
    return out;
}

struct Check { ImU32 color; std::string text; };
static std::vector<Check> checks() {
    std::vector<Check> out;
    GameInfo info = game_info(S.get("game_dir"));
    int supported;
    {
        std::lock_guard lock(g_shared.m);
        supported = g_shared.eboot_supported;
    }
    if (!info.ok) out.push_back({ERR, T("✗  Pasta do jogo não escolhida (Jogo e saves)", "✗  Game folder not set (Game & saves)")});
    else if (supported == 1 || (supported < 0 && info.id == GAME_ID && info.version == GAME_VERSION))
        out.push_back({OK, std::string("✓  ") + GAME_ID + T(" versão 01.01: todos os modos", " version 01.01: every mode")});
    else if (supported == -1) out.push_back({MUTED, std::string("•  ") + info.id + T(" versão ", " version ") + info.version + T(": verificando…", ": checking…")});
    else out.push_back({WARN, std::string("⚠  ") + info.id + T(" versão ", " version ") + info.version +
                                  T(": os modos (60 FPS, AF) são da EU 1.01", ": the modes (60 FPS, AF) target EU 1.01")});
    int dlc = dlc_count(S.get("dlc_dir"));
    out.push_back(dlc ? Check{TEXT, std::string("✓  DLC: ") + std::to_string(dlc) + T(" instalada(s)", " installed")}
                      : Check{MUTED, T("•  Sem DLC", "•  No DLC")});
    out.push_back(sound_ready() ? Check{TEXT, T("✓  Módulos de som encontrados", "✓  Sound modules found")}
                                : Check{WARN, T("⚠  Sem os módulos de som: o jogo fica mudo", "⚠  No sound modules: the game is silent")});
    fs::path saves = user_dir() / "savedata" / "1" / GAME_ID;
    out.push_back(dir_has_entries(saves) ? Check{TEXT, T("✓  Saves encontrados", "✓  Saves found")}
                                         : Check{MUTED, T("•  Ainda sem saves (o jogo cria)", "•  No saves yet (the game creates them)")});
    std::lock_guard lock(g_shared.m);
    switch (g_shared.gpu_state) {
    case 0: out.push_back({MUTED, T("•  Verificando a placa de vídeo…", "•  Checking the graphics card…")}); break;
    case 1: out.push_back({TEXT, std::string("✓  ") + g_shared.gpu_name}); break;
    case 2: out.push_back({ERR, T("✗  Nenhuma placa com Vulkan 1.3 (atualize o driver)", "✗  No Vulkan 1.3 graphics card (update the driver)")}); break;
    default: out.push_back({MUTED, T("•  Placa de vídeo não verificada", "•  Graphics card not checked")}); break;
    }
    return out;
}

static void play_pressed() {
    if (g_game.running) { stop_game(); return; }
    if (!game_info(S.get("game_dir")).ok) { pick_folder("game_dir", ""); return; }
    if (start_game()) {
        toast(T("Preparando o jogo; ele abre na própria janela…", "Preparing the game; it opens in its own window…"), 6);
        if (S.on("close_on_play")) SDL_HideWindow(g_window);
    }
}

static void draw_play_button(const char* id, float w, float h) {
    focus_first_here();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    bool focused = ImGui::IsItemFocused() && g_nav_visible();
    bool hovered = ImGui::IsItemHovered();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool running = g_game.running;
    bool has_game = game_info(S.get("game_dir")).ok;
    ImU32 top = running ? C(150, 60, 50) : has_game ? GREEN_TOP : C(30, 120, 210);
    ImU32 bottom = running ? C(110, 40, 35) : has_game ? GREEN_BOTTOM : C(20, 90, 170);
    if (hovered || focused) {
        ImVec4 t = ImGui::ColorConvertU32ToFloat4(top), bb = ImGui::ColorConvertU32ToFloat4(bottom);
        for (ImVec4* v : {&t, &bb}) { v->x = std::min(1.f, v->x * 1.18f); v->y = std::min(1.f, v->y * 1.18f); v->z = std::min(1.f, v->z * 1.18f); }
        top = ImGui::ColorConvertFloat4ToU32(t);
        bottom = ImGui::ColorConvertFloat4ToU32(bb);
    }
    // Rounded gradient: two halves.
    float r = px(6);
    dl->AddRectFilled(a, b, bottom, r);
    dl->AddRectFilledMultiColor(ImVec2(a.x + r * 0.3f, a.y + px(1)), ImVec2(b.x - r * 0.3f, b.y - px(1)), top, top, bottom, bottom);
    if (focused) dl->AddRect(ImVec2(a.x - px(3), a.y - px(3)), ImVec2(b.x + px(3), b.y + px(3)), TEXT_HI, r + px(2), 0, px(2));
    const char* label = running ? T("■   PARAR", "■   STOP") : has_game ? T("▶   JOGAR", "▶   PLAY")
                                                                       : T("ESCOLHER O JOGO", "CHOOSE THE GAME");
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 tp(a.x + (w - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f);
    dl->AddText(ImVec2(tp.x + 1, tp.y + 1), C(0, 0, 0, 90), label);
    dl->AddText(tp, TEXT_HI, label);
    dl->AddText(ImVec2(tp.x + 0.6f, tp.y), TEXT_HI, label);  // a little weight
    ImGui::PopFont();
    if (pressed) play_pressed();
}

static void draw_hero(float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 b(a.x + w, a.y + height);
    refresh_banner();
    if (g_banner.tex) {
        // Cover: fill the width, crop top and bottom around the upper third.
        float scale = std::max(w / g_banner.w, height / g_banner.h);
        float vis_w = w / scale / g_banner.w, vis_h = height / scale / g_banner.h;
        float u0 = (1 - vis_w) * 0.5f, v0 = (1 - vis_h) * 0.35f;
        dl->AddImage(ImTextureRef((void*)g_banner.tex), a, b, ImVec2(u0, v0), ImVec2(u0 + vis_w, v0 + vis_h));
    } else {
        dl->AddRectFilledMultiColor(a, b, C(42, 71, 94), C(27, 40, 56), C(14, 20, 27), C(14, 20, 27));
        // Distant hills.
        for (int layer = 0; layer < 3; ++layer) {
            ImU32 col = C(20 + layer * 6, 32 + layer * 8, 44 + layer * 10, 255);
            float base = b.y - height * (0.18f + 0.1f * (2 - layer));
            std::vector<ImVec2> pts;
            for (int i = 0; i <= 48; ++i) {
                float x = a.x + w * i / 48.0f;
                float y = base - height * 0.12f * (0.5f + 0.5f * std::sin(i * 0.37f + layer * 1.7f)) *
                          (0.6f + 0.4f * std::sin(i * 0.11f + layer));
                pts.emplace_back(x, y);
            }
            for (size_t i = 0; i + 1 < pts.size(); ++i)
                dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, b.y), ImVec2(pts[i].x, b.y), col);
        }
    }
    // Shade: left for the title, bottom into the page.
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + w * 0.65f, b.y), C(14, 20, 27, 200), C(14, 20, 27, 0), C(14, 20, 27, 0), C(14, 20, 27, 200));
    dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - height * 0.55f), b, C(27, 40, 56, 0), C(27, 40, 56, 0), BG_TOP, BG_TOP);
    GameInfo info = game_info(S.get("game_dir"));
    float x = a.x + px(48);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.4f);
    float th = ImGui::GetTextLineHeight();
    float y = b.y - px(70) - th;
    const char* title = "Shadow of the Colossus";
    dl->AddText(ImVec2(x + 2, y + 2), C(0, 0, 0, 140), title);
    dl->AddText(ImVec2(x, y), TEXT_HI, title);
    dl->AddText(ImVec2(x + 0.8f, y), TEXT_HI, title);
    ImGui::PopFont();
    std::string sub = info.ok ? info.id + "  ·  " + T("versão ", "version ") + info.version + "  ·  " + T("port nativo para Linux", "native Linux port")
                              : std::string(T("Port nativo para Linux  ·  escolha a pasta do seu dump", "Native Linux port  ·  choose your dump's folder"));
    dl->AddText(ImVec2(x, y + th + px(6)), ACCENT_SOFT, sub.c_str());
    ImGui::Dummy(ImVec2(w, height));
}

static void page_play(float avail_h) {
    draw_hero(std::clamp(avail_h * 0.46f, px(200), px(420)));
    float margin = px(48);
    float w = ImGui::GetContentRegionAvail().x - margin * 2;
    float left_w = std::min(w * 0.56f, px(640));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + margin);
    ImGui::BeginGroup();
    ImGui::BeginChild("play_left", ImVec2(left_w, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
    draw_play_button("play", std::min(left_w, px(340)), px(64));
    ImGui::Dummy(ImVec2(0, px(6)));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(MUTED));
    ImGui::TextUnformatted(g_game.running ? (g_game.in_game ? T("O jogo está rodando.", "The game is running.")
                                                             : T("Preparando o jogo…", "Preparing the game…"))
                                          : summary().c_str());
    ImGui::PopStyleColor();
    section(T("Ajustes rápidos", "Quick settings"));
    choice_row("output_res", OUTPUTS, IM_ARRAYSIZE(OUTPUTS), T("Saída", "Output"));
    choice_row("upscaler", UPSCALERS, IM_ARRAYSIZE(UPSCALERS), T("Upscaler", "Upscaler"));
    toggle_row("fps60", T("60 FPS (experimental)", "60 FPS (experimental)"));
    toggle_row("af16", T("Texturas nítidas (filtro 16x)", "Sharp textures (16x filtering)"));
    toggle_row("fullscreen", T("Tela cheia", "Fullscreen"));
    ImGui::EndChild();
    ImGui::EndGroup();
    // The ready check, beside.
    ImGui::SameLine(0, px(32));
    ImGui::BeginGroup();
    float card_w = w - left_w - px(32);
    if (card_w > px(220)) {
        ImVec2 a = ImGui::GetCursorScreenPos();
        auto list = checks();
        float line = ImGui::GetTextLineHeight() + px(12);
        float card_h = px(56) + line * list.size();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + card_w, a.y + card_h), CARD, px(8));
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.86f, ImVec2(a.x + px(18), a.y + px(16)), ACCENT_SOFT,
                    T("VERIFICAÇÃO", "READY CHECK"));
        float y = a.y + px(46);
        for (auto& c : list) {
            std::string t = c.text;
            float max_w = card_w - px(36);
            while (t.size() > 4 && ImGui::CalcTextSize(t.c_str()).x > max_w) t = t.substr(0, t.size() - 4) + "…";
            dl->AddText(ImVec2(a.x + px(18), y), c.color, t.c_str());
            y += line;
        }
        ImGui::Dummy(ImVec2(card_w, card_h));
    }
    ImGui::EndGroup();
}

static void page_game() {
    section(T("Jogo", "Game"));
    folder_row("game_dir", T("Pasta do jogo", "Game folder"),
               T("O dump CUSA08809 (europeu 1.01), com eboot.bin, sce_module e sce_sys.",
                 "The CUSA08809 dump (EU 1.01) with eboot.bin, sce_module and sce_sys."));
    bool scanning;
    { std::lock_guard lock(g_shared.m); scanning = g_shared.scanning; }
    if (action_row("scan", T("Procurar no PC", "Search this PC"),
                   T("Procura o jogo, os módulos de som do shadPS4 e as DLCs nas pastas de costume.",
                     "Looks for the game, shadPS4's sound modules and the DLC in the usual folders."),
                   scanning ? T("procurando…", "searching…") : "›") && !scanning)
        start_scan(true);
    section(T("Som", "Sound"));
    folder_row("sys_modules", T("Módulos de som", "Sound modules"),
               T("libSceNgs2.sprx e libSceUlt.sprx do SEU console (o user/sys_modules do shadPS4). São firmware da "
                 "Sony e nunca vêm com o port. Sem eles o jogo roda mudo.",
                 "libSceNgs2.sprx and libSceUlt.sprx from YOUR console (shadPS4's user/sys_modules). They are Sony "
                 "firmware and never come with the port. Without them the game is silent."));
    section(T("DLC", "DLC"));
    folder_row("dlc_dir", T("Pasta de DLC", "DLC folder"),
               T("Uma subpasta por DLC (cada uma com sce_sys/param.sfo), ou o addcont do shadPS4.",
                 "One subfolder per DLC (each with sce_sys/param.sfo), or shadPS4's addcont folder."),
               T("Sem DLC", "No DLC"));
    section(T("Saves", "Saves"));
    folder_row("user_dir", T("Pasta de dados", "Data folder"),
               T("Saves e cache de shaders. Vazio = ~/.local/share/sotc-pc/user.",
                 "Saves and shader cache. Empty = ~/.local/share/sotc-pc/user."),
               T("Padrão", "Default"));
    if (action_row("import", T("Importar save do shadPS4", "Import a shadPS4 save"),
                   T("Copia o savedata/CUSA08809 do shadPS4 para cá (os saves atuais ficam em .bak).",
                     "Copies shadPS4's savedata/CUSA08809 here (the current saves stay in .bak).")))
        import_shadps4_save();
    if (action_row("open_saves", T("Abrir a pasta dos saves", "Open the saves folder")))
        open_folder(user_dir() / "savedata" / "1" / GAME_ID);
    section(T("Sistema", "System"));
    choice_row("language", LANGUAGES, IM_ARRAYSIZE(LANGUAGES), T("Idioma do sistema", "System language"),
               T("O idioma que o jogo vê no \"console\".", "The language the game sees on the \"console\"."));
    text_row("player_name", T("Nome do jogador", "Player name"));
}

static void page_display() {
    section(T("Modos", "Modes"));
    toggle_row("af16", T("Texturas nítidas (filtro anisotrópico 16x)", "Sharp textures (16x anisotropic filtering)"),
               T("Chão, grama e rochas ficam nítidos à distância, quase sem custo.",
                 "Ground, grass and rocks stay sharp at a distance, at almost no cost."));
    toggle_row("fps60", T("60 FPS (experimental)", "60 FPS (experimental)"),
               T("Tira o limite de 30 FPS do jogo; o vblank vai a 120 Hz.", "Removes the game's 30 FPS cap; vblank at 120 Hz."));
    section(T("Resolução", "Resolution"));
    choice_row("output_res", OUTPUTS, IM_ARRAYSIZE(OUTPUTS), T("Saída", "Output"),
               T("Em 16:9 o jogo renderiza em 1080p e o upscaler amplia. Mais largo que 16:9 mostra mais dos lados.",
                 "At 16:9 the game renders at 1080p and the upscaler enlarges it. Wider than 16:9 shows more at the sides."));
    choice_row("upscaler", UPSCALERS, IM_ARRAYSIZE(UPSCALERS), T("Upscaler", "Upscaler"),
               T("FSR 1 amplia com bordas nítidas (acima de 1080p).", "FSR 1 upscales with sharp edges (above 1080p)."));
    choice_row("sharpness", SHARPNESS, IM_ARRAYSIZE(SHARPNESS), T("Nitidez (RCAS)", "Sharpening (RCAS)"));
    section(T("Modo PS4 Pro", "PS4 Pro mode"));
    toggle_row("neo", T("Apresentar-se como PS4 Pro", "Present as a PS4 Pro"),
               T("O jogo oferece o Modo Grafismo do Pro (Opções > Apresentação). Mais pesado.",
                 "The game offers the Pro graphics mode (Options > Presentation). Heavier."));
    section(T("Desempenho", "Performance"));
    toggle_row("perf_culling", T("Culling sem a GPU (recomendado)", "Culling without the GPU (recommended)"),
               T("Mesma imagem e bem menos esperas pela GPU (técnica do Pink-shadPS4).",
                 "Same image, far fewer GPU waits (Pink-shadPS4)."));
    toggle_row("perf_record_thread", T("Comandos Vulkan em thread própria (recomendado)", "Vulkan commands on their own thread (recommended)"),
               T("Desligue se notar travamentos.", "Turn it off if the game hangs."));
    toggle_row("perf_dma", T("Reflexos completos da água", "Full water reflections"),
               T("Desligado ganha ~10% de FPS, mas a água longe fica mais lisa.", "Off gains ~10% FPS, but distant water is flatter."));
    choice_row("gpu_submit", GPU_SUBMIT, IM_ARRAYSIZE(GPU_SUBMIT), T("Envio de trabalho à GPU", "GPU work submission"),
               T("Medido em NVIDIA (+10–15%). Em AMD/Intel, volte para Automático se a tela ficar preta.",
                 "Measured on NVIDIA (+10–15%). On AMD/Intel, go back to Automatic if the screen turns black."));
    section(T("Efeitos", "Effects"));
    toggle_row("fx_vignette", T("Vinheta (cantos escurecidos)", "Vignette (darkened corners)"));
    toggle_row("fx_fog", T("Névoa atmosférica", "Atmospheric fog"));
    toggle_row("fx_ssao", T("Oclusão ambiente (SSAO)", "Ambient occlusion (SSAO)"));
    section(T("Tela", "Display"));
    toggle_row("fullscreen", T("Tela cheia", "Fullscreen"));
    choice_row("present_mode", PRESENT_MODES, IM_ARRAYSIZE(PRESENT_MODES), T("Apresentação", "Presentation"));
}

static void page_controls() {
    section(T("Controle", "Gamepad"));
    std::vector<std::string> pads;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i)
        if (const char* n = SDL_GetGamepadNameForID(ids[i])) pads.emplace_back(n);
    SDL_free(ids);
    static const Opt AUTO_PAD[] = {{"", "Automático (o primeiro)", "Automatic (the first)"}};
    choice_row("gamepad", AUTO_PAD, 1, T("Controle preferido", "Preferred gamepad"),
               T("Qual controle usar quando houver vários conectados.", "Which gamepad to use when several are connected."), &pads);
    section(T("Mouse", "Mouse"));
    toggle_row("mouse", T("Câmera no mouse", "Mouse camera"),
               T("Clique na janela do jogo para capturar o mouse; Esc solta.", "Click the game window to capture the mouse; Esc releases it."));
    choice_row("mouse_sens", MOUSE_SENS, IM_ARRAYSIZE(MOUSE_SENS), T("Sensibilidade", "Sensitivity"));
    toggle_row("mouse_invert", T("Inverter o eixo vertical", "Invert the vertical axis"));
    section(T("Teclado (funciona junto com o controle)", "Keyboard (works next to a gamepad)"));
    note(T("Selecione uma ação e aperte a nova tecla. Vale na próxima vez que o jogo abrir.",
           "Select an action and press the new key. Applies the next time the game starts."));
    ImGui::Dummy(ImVec2(0, px(4)));
    for (auto& k : KEY_INPUTS) {
        std::string value = key_of(k);
        bool custom = S.has(std::string("key.") + k.name);
        std::string shown = (value.empty() ? "—" : value) + (custom ? "  *" : "");
        if (action_row(k.name, T(k.pt, k.en), nullptr, shown.c_str())) g_capture_key = k.name;
    }
    if (action_row("reset_keys", T("Restaurar as teclas padrão", "Restore the default keys"))) {
        for (auto it = S.v.begin(); it != S.v.end();)
            it = it->first.rfind("key.", 0) == 0 ? S.v.erase(it) : std::next(it);
        g_dirty = true;
    }
}

static void page_advanced() {
    section(T("Lançador", "Launcher"));
    choice_row("ui_language", UI_LANGUAGES, IM_ARRAYSIZE(UI_LANGUAGES), T("Idioma do lançador", "Launcher language"));
    choice_row("launcher_fullscreen", LAUNCHER_FULLSCREEN, IM_ARRAYSIZE(LAUNCHER_FULLSCREEN),
               T("Janela do lançador", "Launcher window"), T("F11 também alterna.", "F11 toggles it too."));
    toggle_row("close_on_play", T("Esconder o lançador enquanto o jogo roda", "Hide the launcher while the game runs"),
               T("Ele volta quando o jogo fechar.", "It comes back when the game closes."));
    if (action_row("shortcut", T("Criar atalho no menu de aplicativos", "Add to the applications menu"),
                   T("Depois dá para adicionar na Steam como jogo não-Steam (bom no Steam Deck).",
                     "Then it can be added to Steam as a non-Steam game (handy on Steam Deck).")))
        desktop_shortcut();
    if (action_row("open_data", T("Abrir a pasta de dados", "Open the data folder"), g_data.c_str())) open_folder(g_data);
    if (action_row("clear_cache", T("Limpar cache de shaders", "Clear shader cache"),
                   T("Use se aparecerem gráficos estranhos depois de uma atualização.", "Use it if graphics look wrong after an update.")) &&
        !g_game.running)
        clear_shader_cache();
    section(T("Diagnóstico", "Diagnostics"));
    toggle_row("show_fps", T("FPS no registro (a cada 5 s)", "FPS in the log (every 5 s)"));
    choice_row("readbacks", READBACKS, IM_ARRAYSIZE(READBACKS), T("Leituras da GPU", "GPU readbacks"));
    text_row("engine_cvars", T("Cvars do motor", "Engine cvars"),
             T("NOME=valor separados por espaço, na linha de comando do jogo.", "NAME=value separated by spaces, on the game's command line."));
    text_row("extra_env", T("Variáveis extras", "Extra variables"),
             T("Pares NOME=valor separados por espaço (veja o README).", "NAME=value pairs separated by spaces (see the README)."));
}

static float g_log_scroll = 0;   // gamepad scrolling request for the log (lines)

static void page_log() {
    ImGui::Dummy(ImVec2(0, px(14)));
    focus_first_here();
    if (ImGui::Button(T("Copiar", "Copy"))) { SDL_SetClipboardText(g_log.text().c_str()); toast(T("Registro copiado.", "Log copied.")); }
    ImGui::SameLine();
    if (ImGui::Button(T("Limpar", "Clear"))) g_log.clear();
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir pasta dos registros", "Open the logs folder"))) open_folder(g_data);
    ImGui::Dummy(ImVec2(0, px(6)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(C(8, 12, 16, 230)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12), px(10)));
    ImGui::BeginChild("log", ImVec2(0, -px(16)), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.82f);
    bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2;
    {
        std::lock_guard lock(g_log.m);
        ImGuiListClipper clipper;
        clipper.Begin((int)g_log.lines.size());
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) ImGui::TextUnformatted(g_log.lines[i].c_str());
        if (g_log.lines.empty()) ImGui::TextDisabled("%s", T("Nada ainda. A saída do jogo aparece aqui.", "Nothing yet. The game's output shows here."));
        if (g_log.changed && at_bottom) ImGui::SetScrollHereY(1.0f);
        g_log.changed = false;
    }
    if (g_log_scroll != 0) {
        ImGui::SetScrollY(ImGui::GetScrollY() + g_log_scroll * ImGui::GetTextLineHeightWithSpacing());
        g_log_scroll = 0;
    }
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------------------------
// Frame: header with tabs, the page, footer with the gamepad hints.

static void draw_background(ImVec2 size) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilledMultiColor(ImVec2(0, 0), size, BG_TOP, BG_TOP, BG_BOTTOM, BG_BOTTOM);
}

static void draw_header(ImVec2 size, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(size.x, h), HEADER);
    dl->AddLine(ImVec2(0, h), ImVec2(size.x, h), C(0, 0, 0, 120));
    float x = px(28);
    float y = (h - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.86f);
    const char* brand = "SHADOW OF THE COLOSSUS";
    float by = (h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(x, by), ACCENT_SOFT, brand);
    dl->AddText(ImVec2(x + 0.6f, by), ACCENT_SOFT, brand);
    x += ImGui::CalcTextSize(brand).x + px(36);
    ImGui::PopFont();
    float glyph_h = px(26);
    bool pad = g_family != PadFamily::None;
    if (pad) x += draw_glyph(dl, ImVec2(x, (h - glyph_h) * 0.5f), Glyph::LB, glyph_h) + px(18);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int p = 0; p < PAGE_COUNT; ++p) {
        const char* t = page_title(p);
        ImVec2 ts = ImGui::CalcTextSize(t);
        ImGui::SetCursorScreenPos(ImVec2(x - px(10), 0));
        ImGui::PushID(p);
        if (ImGui::InvisibleButton("tab", ImVec2(ts.x + px(20), h))) go_page(p);
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        bool sel = p == g_page;
        dl->AddText(ImVec2(x, y), sel ? TEXT_HI : hovered ? TEXT : MUTED, t);
        if (sel) dl->AddRectFilled(ImVec2(x, h - px(4)), ImVec2(x + ts.x, h), ACCENT, px(2));
        x += ts.x + px(30);
    }
    ImGui::PopItemFlag();
    if (pad) x += draw_glyph(dl, ImVec2(x - px(12), (h - glyph_h) * 0.5f), Glyph::RB, glyph_h);
    // Right: the gamepad and the clock.
    char clock[16];
    time_t t = time(nullptr);
    strftime(clock, sizeof(clock), "%H:%M", localtime(&t));
    float cw = ImGui::CalcTextSize(clock).x;
    float rx = size.x - px(28) - cw;
    dl->AddText(ImVec2(rx, y), TEXT, clock);
    if (pad) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
        std::string name = g_pad_name;
        if (name.size() > 28) name = name.substr(0, 27) + "…";
        float nw = ImGui::CalcTextSize(name.c_str()).x;
        float ny = (h - ImGui::GetTextLineHeight()) * 0.5f;
        float nx = rx - px(24) - nw;
        if (nx > x + px(20)) {
            dl->AddText(ImVec2(nx, ny), MUTED, name.c_str());
            // A small gamepad pictogram.
            float gx = nx - px(30), gy = h * 0.5f;
            dl->AddRectFilled(ImVec2(gx, gy - px(6)), ImVec2(gx + px(22), gy + px(6)), MUTED, px(6));
            dl->AddCircleFilled(ImVec2(gx + px(6), gy), px(2), HEADER);
            dl->AddCircleFilled(ImVec2(gx + px(16), gy), px(2), HEADER);
        }
        ImGui::PopFont();
    }
}

static void draw_footer(ImVec2 size, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float top = size.y - h;
    dl->AddRectFilled(ImVec2(0, top), ImVec2(size.x, size.y), HEADER);
    dl->AddLine(ImVec2(0, top), ImVec2(size.x, top), C(0, 0, 0, 120));
    float y = top + (h - ImGui::GetTextLineHeight()) * 0.5f;
    // Left: a message, or the state of the game.
    std::string left;
    ImU32 left_col = MUTED;
    if (g_toast_until > now_s()) { left = g_toast; left_col = TEXT; }
    else if (g_game.running) { left = g_game.in_game ? T("● O jogo está rodando", "● The game is running") : T("● Preparando o jogo…", "● Preparing the game…"); left_col = OK; }
    else left = std::string("v") + SOTC_VERSION + T("  ·  port nativo para Linux", "  ·  native Linux port");
    // Right: what the buttons do.
    struct Hint { Glyph g; const char* key; const char* text; };
    std::vector<Hint> hints = {{Glyph::South, "Enter", T("Selecionar", "Select")}};
    if (g_page != PAGE_PLAY) hints.push_back({Glyph::East, "Esc", T("Voltar", "Back")});
    hints.push_back({Glyph::Start, "F5", g_game.running ? T("Parar", "Stop") : T("Jogar", "Play")});
    float gh = px(26);
    bool pad = g_family != PadFamily::None;
    float x = size.x - px(28);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.9f);
    float ty = top + (h - ImGui::GetTextLineHeight()) * 0.5f;
    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        float tw = ImGui::CalcTextSize(it->text).x;
        x -= tw;
        dl->AddText(ImVec2(x, ty), TEXT, it->text);
        x -= px(10);
        float gw;
        if (pad) {
            gw = (it->g == Glyph::Start) ? std::max(gh * 1.35f, ImGui::GetFont()->CalcTextSizeA(gh * 0.56f, FLT_MAX, 0,
                         g_family == PadFamily::PlayStation ? "Options" : g_family == PadFamily::Nintendo ? "+" : "Menu").x + gh * 0.6f) : gh;
            draw_glyph(dl, ImVec2(x - gw, top + (h - gh) * 0.5f), it->g, gh);
        } else {
            float kw;
            ImFont* font = ImGui::GetFont();
            ImVec2 ts = font->CalcTextSizeA(gh * 0.56f, FLT_MAX, 0, it->key);
            kw = std::max(gh, ts.x + gh * 0.6f);
            draw_key_cap(dl, ImVec2(x - kw, top + (h - gh) * 0.5f), it->key, gh, &gw);
        }
        x -= gw + px(26);
    }
    float max_left = x - px(48);
    while (left.size() > 4 && ImGui::CalcTextSize(left.c_str()).x > max_left) left = left.substr(0, left.size() - 4) + "…";
    dl->AddText(ImVec2(px(28), y), left_col, left.c_str());
    ImGui::PopFont();
}

static void draw_capture_overlay(ImVec2 size) {
    if (g_capture_key.empty()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), size, C(0, 0, 0, 170));
    const KeyInput* k = nullptr;
    for (auto& i : KEY_INPUTS) if (g_capture_key == i.name) k = &i;
    std::string title = std::string(T("Aperte a tecla para: ", "Press the key for: ")) + (k ? T(k->pt, k->en) : "");
    const char* back = g_family == PadFamily::PlayStation ? "○" : g_family == PadFamily::Nintendo ? "A" : "B";
    std::string hint_text = std::string(T("Esc ou ", "Esc or ")) + back + T(" cancela  ·  Delete deixa sem tecla", " cancels  ·  Delete leaves it unbound");
    const char* hint = hint_text.c_str();
    ImFont* font = ImGui::GetFont();
    float big = ImGui::GetFontSize() * 1.25f, small = ImGui::GetFontSize() * 0.9f;
    ImVec2 ts = font->CalcTextSizeA(big, FLT_MAX, 0, title.c_str()), hs = font->CalcTextSizeA(small, FLT_MAX, 0, hint);
    float w = std::max(ts.x, hs.x) + px(80), h = px(140);
    ImVec2 a((size.x - w) * 0.5f, (size.y - h) * 0.5f), b(a.x + w, a.y + h);
    dl->AddRectFilled(a, b, C(32, 44, 58), px(10));
    dl->AddRect(a, b, ACCENT, px(10), 0, px(2));
    dl->AddText(font, big, ImVec2(a.x + (w - ts.x) * 0.5f, a.y + px(38)), TEXT_HI, title.c_str());
    dl->AddText(font, small, ImVec2(a.x + (w - hs.x) * 0.5f, a.y + px(88)), MUTED, hint);
}

static void finish_capture(const std::string& key) {
    const KeyInput* k = nullptr;
    for (auto& i : KEY_INPUTS) if (g_capture_key == i.name) k = &i;
    if (k) {
        std::string name = std::string("key.") + k->name;
        if (key == k->key) S.v.erase(name);
        else S.set(name, key);
        // One action per key: another action that had this key loses it.
        if (!key.empty())
            for (auto& other : KEY_INPUTS)
                if (&other != k && key_of(other) == key) S.set(std::string("key.") + other.name, "");
        g_dirty = true;
    }
    g_capture_key.clear();
}

// ---------------------------------------------------------------------------------------------
// Scripted input for tests (SOTC_LAUNCHER_SCRIPT="frame:key,frame:key", keys as ImGui key names)
// and a screenshot at the end (SOTC_LAUNCHER_SHOT=file.bmp, SOTC_LAUNCHER_FRAMES=n).

// "pad.<button>" presses a button of a virtual gamepad (SOTC_LAUNCHER_VPAD=ps5 or xbox), which
// goes through SDL like a real one.
struct ScriptStep { int frame; ImGuiKey key; int pad_button; };
static std::vector<ScriptStep> g_script;
static SDL_Joystick* g_vpad = nullptr;
static void attach_virtual_pad() {
    const char* kind = getenv("SOTC_LAUNCHER_VPAD");
    if (!kind || !*kind) return;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    bool ps = !strcmp(kind, "ps5");
    desc.vendor_id = ps ? 0x054c : 0x045e;
    desc.product_id = ps ? 0x0ce6 : 0x0b12;
    desc.name = ps ? "DualSense Wireless Controller" : "Xbox Series X Controller";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    g_vpad = id ? SDL_OpenJoystick(id) : nullptr;
}
static void load_script() {
    const char* s = getenv("SOTC_LAUNCHER_SCRIPT");
    if (!s) return;
    std::stringstream in(s);
    std::string item;
    while (std::getline(in, item, ',')) {
        size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        int frame = atoi(item.substr(0, colon).c_str());
        std::string name = item.substr(colon + 1);
        static const std::pair<const char*, int> pad[] = {
            {"pad.south", SDL_GAMEPAD_BUTTON_SOUTH}, {"pad.east", SDL_GAMEPAD_BUTTON_EAST},
            {"pad.west", SDL_GAMEPAD_BUTTON_WEST}, {"pad.north", SDL_GAMEPAD_BUTTON_NORTH},
            {"pad.lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"pad.rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
            {"pad.start", SDL_GAMEPAD_BUTTON_START}, {"pad.up", SDL_GAMEPAD_BUTTON_DPAD_UP},
            {"pad.down", SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"pad.left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
            {"pad.right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}};
        for (auto& [n, b] : pad)
            if (name == n) g_script.push_back({frame, ImGuiKey_None, b});
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
            if (name == ImGui::GetKeyName((ImGuiKey)k)) g_script.push_back({frame, (ImGuiKey)k, -1});
    }
}

int main(int argc, char** argv) {
    find_paths();
    load_settings();
    std::string ui = S.get("ui_language");
    if (ui.empty()) {
        const char* lang = getenv("LC_ALL");
        if (!lang || !*lang) lang = getenv("LC_MESSAGES");
        if (!lang || !*lang) lang = getenv("LANG");
        g_pt = lang && strncmp(lang, "pt", 2) == 0;
    } else {
        g_pt = ui == "pt";
    }
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--play")) return play_without_window(argc, argv, i + 1);

    SDL_SetHint(SDL_HINT_APP_NAME, "Shadow of the Colossus");
    SDL_SetHint(SDL_HINT_APP_ID, "sotc-pc");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    g_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (g_scale <= 0) g_scale = 1;
    if (const char* s = getenv("SOTC_LAUNCHER_SCALE")) g_scale = (float)atof(s);
    bool steam_ui = getenv("SteamDeck") || getenv("SteamGamepadUI") ||
                    (getenv("XDG_CURRENT_DESKTOP") && strstr(getenv("XDG_CURRENT_DESKTOP"), "gamescope"));
    std::string lf = S.get("launcher_fullscreen");
    bool fullscreen = lf == "1" || (lf == "auto" && steam_ui);
    int win_w = 1280, win_h = 800;
    if (const char* s = getenv("SOTC_LAUNCHER_SIZE")) sscanf(s, "%dx%d", &win_w, &win_h);
    g_window = SDL_CreateWindow("Shadow of the Colossus", int(win_w * g_scale), int(win_h * g_scale),
                                SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!g_window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(g_window, 900, 560);
    g_renderer = SDL_CreateRenderer(g_window, nullptr);
    if (!g_renderer) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(g_renderer, 1);
    set_window_icon();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAuto = true;
    for (fs::path font : {g_exe_dir / "fonts" / "DejaVuSans.ttf", g_port / "gpu" / "third_party" / "fonts" / "DejaVuSans.ttf",
                          fs::path("/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"),
                          fs::path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
                          fs::path("/usr/share/fonts/TTF/DejaVuSans.ttf")}) {
        if (is_file(font) && io.Fonts->AddFontFromFileTTF(font.c_str(), 19.0f)) break;
    }
    ImGui::GetStyle().FontSizeBase = 19.0f;
    apply_style();
    ImGui::GetStyle().FontScaleDpi = g_scale;
    ImGui_ImplSDL3_InitForSDLRenderer(g_window, g_renderer);
    ImGui_ImplSDLRenderer3_Init(g_renderer);
    attach_virtual_pad();
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    refresh_gamepads();

    detect_gpu();
    if (!game_info(S.get("game_dir")).ok && S.get("setup_done") != "1") start_scan(false);
    g_focus_first = true;
    load_script();
    const char* shot = getenv("SOTC_LAUNCHER_SHOT");
    int shot_frames = getenv("SOTC_LAUNCHER_FRAMES") ? atoi(getenv("SOTC_LAUNCHER_FRAMES")) : 30;
    if (const char* p = getenv("SOTC_LAUNCHER_PAGE")) g_page = atoi(p);
    if (const char* l = getenv("SOTC_LAUNCHER_LOG")) { std::ifstream f(l); std::string line; while (std::getline(f, line)) g_log.add(line); }

    bool quit = false, quit_after_game = false;
    int frame = 0;
    while (!quit) {
        bool focused = SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS;
        bool hidden = SDL_GetWindowFlags(g_window) & SDL_WINDOW_HIDDEN;
        if (!shot && (hidden || (!focused && g_game.running))) SDL_WaitEventTimeout(nullptr, 200);
        for (auto& step : g_script) {
            if (step.pad_button < 0 || !g_vpad) continue;
            if (step.frame == frame) SDL_SetJoystickVirtualButton(g_vpad, step.pad_button, true);
            if (step.frame + 3 == frame) SDL_SetJoystickVirtualButton(g_vpad, step.pad_button, false);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (!g_capture_key.empty()) {
                // Key binding capture: the next key (or B / Esc to cancel).
                if (e.type == SDL_EVENT_KEY_DOWN) {
                    if (e.key.scancode == SDL_SCANCODE_ESCAPE) g_capture_key.clear();
                    else if (e.key.scancode == SDL_SCANCODE_DELETE) finish_capture("");
                    else if (const char* name = SDL_GetScancodeName(e.key.scancode); name && *name) finish_capture(name);
                    continue;
                }
                if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) g_capture_key.clear();
                    continue;
                }
                if (e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_TEXT_INPUT || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) continue;
            }
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(g_window))) {
                if (g_game.running) { quit_after_game = true; SDL_HideWindow(g_window); }
                else quit = true;
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) refresh_gamepads();
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat)
                SDL_SetWindowFullscreen(g_window, !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN));
        }
        // While the game runs it owns the gamepad: the launcher must not react to its buttons.
        if (g_game.running && !shot) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        // Results from the background.
        {
            std::lock_guard lock(g_shared.m);
            for (auto& [key, path] : g_shared.picked) {
                S.set(key, path);
                g_dirty = true;
                if (key == "game_dir") {
                    if (!game_info(path).ok) toast(T("Não há eboot.bin nessa pasta. Escolha a pasta do dump (CUSA08809).",
                                                      "No eboot.bin in that folder. Choose the dump's folder (CUSA08809)."));
                    else S.set("setup_done", "1");
                }
                if (key == "sys_modules" && !sound_ready())
                    toast(T("libSceNgs2.sprx e libSceUlt.sprx não estão nessa pasta.", "libSceNgs2.sprx and libSceUlt.sprx are not in that folder."));
            }
            g_shared.picked.clear();
            if (g_shared.scan_ready) {
                g_shared.scan_ready = false;
                int applied = 0;
                if (!g_shared.found_games.empty() && (g_shared.scan_manual || !game_info(S.get("game_dir")).ok)) {
                    S.set("game_dir", g_shared.found_games.front()); ++applied; S.set("setup_done", "1");
                }
                if (!g_shared.found_modules.empty() && (g_shared.scan_manual || !sound_ready())) {
                    S.set("sys_modules", g_shared.found_modules.front()); ++applied;
                }
                if (!g_shared.found_dlc.empty() && S.get("dlc_dir").empty()) { S.set("dlc_dir", g_shared.found_dlc); ++applied; }
                if (applied) {
                    g_dirty = true;
                    toast(game_info(S.get("game_dir")).ok
                              ? std::string(T("Jogo encontrado: ", "Game found: ")) + S.get("game_dir")
                              : std::string(T("Módulos de som encontrados.", "Sound modules found.")), 7);
                } else if (g_shared.scan_manual) {
                    toast(T("Nada encontrado. Escolha a pasta do jogo à mão.", "Nothing found. Choose the game folder by hand."));
                }
            }
        }
        if (g_dirty) { save_settings(); set_window_icon(); }
        if (game_info(S.get("game_dir")).ok) check_eboot(S.get("game_dir"));
        if (g_game.finished) {
            g_game.finished = false;
            int code = g_game.exit_code;
            if (code != 0 && !g_game.stopping)
                toast(std::string(T("O jogo fechou com erro (", "The game closed with an error (")) + std::to_string(code) +
                      T("). Veja a aba Registro.", "). See the Log tab."), 8);
            if (quit_after_game) quit = true;
            SDL_ShowWindow(g_window);
            SDL_RaiseWindow(g_window);
        }
        if (g_game.running && g_game.stop_requested > 0 && now_s() - g_game.stop_requested > 4) {
            kill(-g_game.pid, SIGKILL);
            g_game.stop_requested = 0;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        for (auto& step : g_script) {
            if (step.pad_button >= 0) continue;
            if (step.frame == frame) io.AddKeyEvent(step.key, true);
            if (step.frame + 2 == frame) io.AddKeyEvent(step.key, false);
        }
        ImGui::NewFrame();

        // Global buttons: tabs (LB/RB, PageUp/PageDown), back (B, Esc), play (Start, F5).
        bool typing = io.WantTextInput;
        if (g_capture_key.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || (!typing && ImGui::IsKeyPressed(ImGuiKey_PageUp, false)))
                go_page((g_page + PAGE_COUNT - 1) % PAGE_COUNT);
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || (!typing && ImGui::IsKeyPressed(ImGuiKey_PageDown, false)))
                go_page((g_page + 1) % PAGE_COUNT);
            if (!ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
                (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) &&
                g_page != PAGE_PLAY)
                go_page(PAGE_PLAY);
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) || ImGui::IsKeyPressed(ImGuiKey_F5, false)) play_pressed();
            if (g_page == PAGE_LOG) {
                if (ImGui::IsKeyDown(ImGuiKey_GamepadRStickDown)) g_log_scroll += 1;
                if (ImGui::IsKeyDown(ImGuiKey_GamepadRStickUp)) g_log_scroll -= 1;
            }
        }

        ImVec2 size = io.DisplaySize;
        draw_background(size);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
        float header_h = px(64), footer_h = px(56);
        draw_header(size, header_h);
        ImGui::SetCursorScreenPos(ImVec2(0, header_h));
        float body_h = size.y - header_h - footer_h;
        ImGui::BeginChild("page", ImVec2(size.x, body_h), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_None);
        if (g_page == PAGE_PLAY) {
            page_play(body_h);
        } else {
            // A centered column, as in Steam's settings.
            float col = std::min(size.x - px(96), px(1000));
            ImGui::SetCursorPosX((size.x - col) * 0.5f);
            ImGui::BeginGroup();
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.7f);
            ImGui::Dummy(ImVec2(0, px(18)));
            ImGui::SetCursorPosX((size.x - col) * 0.5f);
            ImGui::TextUnformatted(page_title(g_page));
            ImGui::PopFont();
            ImGui::SetCursorPosX((size.x - col) * 0.5f);
            ImGui::BeginChild("column", ImVec2(col, g_page == PAGE_LOG ? body_h - px(80) : 0),
                              (g_page == PAGE_LOG ? 0 : ImGuiChildFlags_AutoResizeY) | ImGuiChildFlags_NavFlattened);
            switch (g_page) {
            case PAGE_GAME: page_game(); break;
            case PAGE_DISPLAY: page_display(); break;
            case PAGE_CONTROLS: page_controls(); break;
            case PAGE_ADVANCED: page_advanced(); break;
            case PAGE_LOG: page_log(); break;
            }
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0, px(28)));
            ImGui::EndGroup();
        }
        ImGui::EndChild();
        ImGui::SetCursorScreenPos(ImVec2(0, size.y - footer_h));
        draw_footer(size, footer_h);
        ImGui::End();
        draw_capture_overlay(size);
        ImGui::Render();

        SDL_SetRenderDrawColor(g_renderer, 14, 20, 27, 255);
        SDL_RenderClear(g_renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
        if (shot && frame == shot_frames) {
            if (SDL_Surface* s = SDL_RenderReadPixels(g_renderer, nullptr)) { SDL_SaveBMP(s, shot); SDL_DestroySurface(s); }
            quit = true;
        }
        SDL_RenderPresent(g_renderer);
        ++frame;
    }
    if (g_dirty) save_settings();
    if (g_game.running) stop_game();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (g_banner.tex) SDL_DestroyTexture(g_banner.tex);
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
