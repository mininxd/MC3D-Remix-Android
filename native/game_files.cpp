// The game on Android: its files or a disc image (.iso) in the Black folder on the phone's storage or on an SD card
// (all files access granted) or in the app's own assets folder. The xbe must be the one this build was recompiled from; the Europe xbe
// is turned into the US one by src/common/europe.patch, linked into the library.
#include <jni.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe recompiled into this library, written by CMake

namespace app {

namespace {

namespace fs = std::filesystem;

// the shared storage's folder for the game, readable with all files access: the files or the image in it, or in its
// assets folder
constexpr const char* kShared = "/storage/emulated/0/mc3dr";
constexpr const char* kVolumes = "/storage";

// the Black folders BlackActivity has from Android's storage volumes, which some phones don't show in /storage
std::vector<fs::path> ActivityCardFolders() {
    std::vector<fs::path> out;
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return out;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, "cardFolderPaths", "()[Ljava/lang/String;");
    auto paths = m ? static_cast<jobjectArray>(env->CallObjectMethod(activity, m)) : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        paths = nullptr;
    }
    for (jsize i = 0, n = paths ? env->GetArrayLength(paths) : 0; i < n; i++) {
        auto s = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
        if (!s) continue;
        const char* c = env->GetStringUTFChars(s, nullptr);
        if (c) {
            out.emplace_back(c);
            env->ReleaseStringUTFChars(s, c);
        }
        env->DeleteLocalRef(s);
    }
    if (paths) env->DeleteLocalRef(paths);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return out;
}

// the Black folder on each SD card or USB stick Android has mounted, /storage/<volume id>/Black; all files access
// reads them as it does the phone's own storage
std::vector<fs::path> RemovableFolders() {
    std::vector<fs::path> out = ActivityCardFolders();
    std::error_code ec, dir_ec;
    for (auto& e : fs::directory_iterator(kVolumes, ec)) {
        if (ec) break;
        std::string name = e.path().filename().string();
        if (name == "emulated" || name == "self" || !e.is_directory(dir_ec)) continue;
        out.push_back(e.path() / "mc3dr");
    }
    static bool logged = false;
    if (ec && !logged && ec != std::errc::permission_denied && ec.value() != EACCES) {
        LOGW("game: %s cannot be listed: %s", kVolumes, ec.message().c_str());
        logged = true;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool IsActivityExtracting() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return false;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = env->GetMethodID(cls, "isExtracting", "()Z");
    jboolean res = m ? env->CallBooleanMethod(activity, m) : JNI_FALSE;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        res = JNI_FALSE;
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return res == JNI_TRUE;
}

std::vector<fs::path> AssetFolders() {
    std::vector<fs::path> out = {
        fs::path(kShared),
        fs::path(kShared) / "assets",
        fs::path("/sdcard/mc3dr"),
        fs::path("/sdcard/mc3dr/assets"),
        fs::path("/storage/emulated/0/mc3dr")
    };
    for (const fs::path& card : RemovableFolders()) {
        out.push_back(card);
        out.push_back(card / "assets");
    }
    out.push_back(ExeDir() / "assets");
    return out;
}

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

struct XbeCompatInfo {
    bool compatible = false;
    uint32_t title_id = 0;
    uint32_t region = 0;
    uint32_t version = 0;
};

uint32_t ReadLe32(const std::vector<uint8_t>& buf, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, buf.data() + off, sizeof(v));
    return v;
}

const char* RegionLabel(uint32_t region) {
    if (region == 0x00000004u) return "Europe/PAL";
    if (region == 0x00000001u) return "USA/NTSC-U";
    if (region == 0x00000007u) return "USA/Region-Free";
    if (region & 0x00000001u) return "North America";
    if (region & 0x00000004u) return "Europe/ROW";
    return "Unknown";
}

// Both USA/NTSC-U (v5, retail or widescreen-patched) and Europe/PAL (v6) releases of
// Midnight Club 3: DUB Edition Remix share identical section virtual addresses, sizes,
// and .data layout, so both run natively on the recompiled binary without warnings.
XbeCompatInfo InspectMc3dXbe(const std::vector<uint8_t>& xbe) {
    XbeCompatInfo info{};
    if (xbe.size() < 0x1000u) return info;
    if (std::memcmp(xbe.data(), "XBEH", 4) != 0) return info;

    const uint32_t base = ReadLe32(xbe, 0x104u);
    const uint32_t image_size = ReadLe32(xbe, 0x10Cu);
    const uint32_t cert_va = ReadLe32(xbe, 0x118u);
    const uint32_t num_sections = ReadLe32(xbe, 0x11Cu);
    const uint32_t sec_hdr_va = ReadLe32(xbe, 0x120u);
    if (base != 0x00010000u || image_size != 0x0057A280u || num_sections == 0u || num_sections > 64u) return info;
    if (cert_va < base || sec_hdr_va < base) return info;

    const size_t cert_off = size_t(cert_va - base);
    const size_t sec_off = size_t(sec_hdr_va - base);
    if (cert_off + 0xB0u > xbe.size() || sec_off + size_t(num_sections) * 56u > xbe.size()) return info;

    info.title_id = ReadLe32(xbe, cert_off + 0x08u);
    info.region = ReadLe32(xbe, cert_off + 0xA0u);
    info.version = ReadLe32(xbe, cert_off + 0xACu);
    if (info.title_id != 0x54540079u) return info;

    bool text_ok = false, rdata_ok = false, data_ok = false;
    for (uint32_t i = 0; i < num_sections; ++i) {
        const size_t off = sec_off + size_t(i) * 56u;
        const uint32_t va = ReadLe32(xbe, off + 4u);
        const uint32_t vsz = ReadLe32(xbe, off + 8u);
        const uint32_t name_va = ReadLe32(xbe, off + 20u);
        if (name_va < base) continue;
        const size_t name_off = size_t(name_va - base);
        if (name_off >= xbe.size()) continue;
        const char* name = reinterpret_cast<const char*>(xbe.data() + name_off);
        const size_t max_len = std::min<size_t>(16u, xbe.size() - name_off);
        if (std::strncmp(name, ".text", max_len) == 0 && va == 0x00012000u && vsz == 0x0036811Cu) text_ok = true;
        if (std::strncmp(name, ".rdata", max_len) == 0 && va == 0x004476E0u && vsz == 0x00060A58u) rdata_ok = true;
        if (std::strncmp(name, ".data", max_len) == 0 && va == 0x004A8140u && vsz == 0x000C5D1Cu) data_ok = true;
    }
    info.compatible = text_ok && rdata_ok && data_ok;
    return info;
}

std::string Utf8(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) s += *w < 0x80 ? char(*w) : '?';
    return s;
}

// an image cut short (a copy that stopped, a bad download) still opens, its folders being near the start; the game
// then fails a read later and shows its dirty disc screen. Its files are checked against its size, for the log
void CheckImage(const kernel::Disc& disc, const fs::path& iso) {
    std::error_code ec;
    uint64_t size = fs::file_size(iso, ec);
    if (ec) {
        LOGE("game: %s: its size cannot be read: %s", iso.string().c_str(), ec.message().c_str());
        return;
    }
    uint64_t end = 0;
    size_t files = 0, past = 0;
    std::string first;
    std::vector<std::pair<const kernel::Disc::Entry*, std::string>> todo{{disc.Find(std::string_view()), ""}};
    while (!todo.empty()) {
        auto [dir, path] = todo.back();
        todo.pop_back();
        if (!dir) continue;
        for (const kernel::Disc::Entry& e : dir->children) {
            std::string name = path.empty() ? e.name : path + "\\" + e.name;
            if (e.dir) {
                todo.push_back({&e, name});
                continue;
            }
            files++;
            uint64_t file_end = e.offset + e.size;
            end = std::max(end, file_end);
            if (file_end > size && past++ == 0) first = name;
        }
    }
    LOGR("Game: image %llu bytes, %zu files up to byte %llu", (unsigned long long)size, files, (unsigned long long)end);
    if (past)
        LOGE("game: %s is cut short: %zu of its files end past its %llu bytes, %s the first; copy the image again",
             iso.string().c_str(), past, (unsigned long long)size, first.c_str());
}

// no game yet: the activity's dialog tells the player where it goes and asks for the access; this waits a moment
// before looking again, keeping SDL's events moving
void WaitForGame() {
    static bool logged = false;
    if (!logged) {
        std::error_code ec;
        fs::is_directory(kShared, ec);
        std::string cards;
        for (const fs::path& card : RemovableFolders()) cards += ", " + card.string();
        LOGE("game: no game files in %s%s or %s%s, waiting for them", kShared, cards.c_str(),
             (ExeDir() / "assets").c_str(), ec == std::errc::permission_denied ? ", no access to the first" : "");
        logged = true;
    }
    for (int i = 0; i < 10; i++) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_TERMINATING) {
                logx::Flush();
                std::_Exit(0);
            }
        SDL_Delay(100);
    }
}

// the xbes of other releases the player chose to always start, one sha1 a line, so the question is not asked again
fs::path AllowedFile() { return ExeDir() / "other_releases.txt"; }

bool AllowedRelease(const std::string& sha) {
    std::ifstream f(AllowedFile());
    for (std::string line; std::getline(f, line);)
        if (line == sha) return true;
    return false;
}

void AllowRelease(const std::string& sha) {
    std::ofstream f(AllowedFile(), std::ios::app);
    f << sha << "\n";
    if (!f) LOGW("game: %s cannot be written", AllowedFile().string().c_str());
}

bool AskOtherRelease(const fs::path& name, const std::string& sha) {
    if (AllowedRelease(sha)) {
        LOGR("Game: another release, started without asking as the player chose");
        return true;
    }
    std::string text = name.filename().string() +
                       " is a different release of the game than this build was made from, and will most likely "
                       "crash.\n\nStart it anyway?";
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "No"},
        {0, 1, "Yes"},
        {0, 2, "Yes, don't ask again"},
    };
    std::string title = Utf8(kGame.short_title);
    SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, nullptr, title.c_str(), text.c_str(), 3, buttons, nullptr};
    int answer = 0;
    if (!SDL_ShowMessageBox(&box, &answer)) return false;
    if (answer == 2) AllowRelease(sha);
    return answer >= 1;
}

// 1 found, 0 none there, -1 the player turned down another release
int Search(GameFiles& game) {
    if (IsActivityExtracting()) {
        return 0;
    }
    // the first copy with this xbe wins, the extracted files before any image; another release is kept to offer
    GameFiles other;
    std::unique_ptr<kernel::Disc> other_disc;
    fs::path other_name;
    std::string other_sha;
    auto consider = [&](const fs::path& assets, const fs::path& name, std::vector<uint8_t> xbe,
                        std::unique_ptr<kernel::Disc> disc) {
        std::string sha = Sha1(xbe);
        bool exact = sha == kImageSha1;
        XbeCompatInfo info = InspectMc3dXbe(xbe);
        bool match = exact || info.compatible;
        if (match) {
            LOGR("[OK] Game: %s in %s, sha1 %s (%s, region=%s 0x%X, ver=%u)",
                 IMAGE_XBE, name.string().c_str(), sha.c_str(),
                 exact ? "exact build match" : "compatible MC3D Remix release",
                 RegionLabel(info.region), info.region, info.version);
            game = {assets, std::move(xbe)};
            kernel::MountDisc(std::move(disc));
            return true;
        }
        LOGR("Game: %s in %s, sha1 %s, another release", IMAGE_XBE, name.string().c_str(), sha.c_str());
        if (other_name.empty()) {
            other = {assets, std::move(xbe)};
            other_disc = std::move(disc);
            other_name = name;
            other_sha = sha;
        }
        return false;
    };

    std::error_code ec;
    for (const fs::path& assets : AssetFolders()) {
        if (!fs::is_directory(assets, ec)) continue;
        std::vector<uint8_t> xbe;
        if (ReadHostFile(assets / IMAGE_XBE, xbe) && consider(assets, assets, std::move(xbe), nullptr)) return 1;
        std::vector<fs::path> isos;
        for (auto& e : fs::directory_iterator(assets, ec)) {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
        }
        std::sort(isos.begin(), isos.end());
        for (const fs::path& iso : isos) {
            auto disc = std::make_unique<kernel::Disc>();
            std::string error;
            if (!disc->Open(iso, error)) {
                LOGW("game: %s: %s", iso.string().c_str(), error.c_str());
                continue;
            }
            if (!disc->Load(IMAGE_XBE, xbe)) {
                LOGW("game: %s has no %s", iso.string().c_str(), IMAGE_XBE);
                continue;
            }
            CheckImage(*disc, iso);
            if (consider(assets, iso, std::move(xbe), std::move(disc))) return 1;
        }
    }

    if (other_name.empty()) return 0;
    if (!AskOtherRelease(other_name, other_sha)) return -1;
    game = std::move(other);
    kernel::MountDisc(std::move(other_disc));
    return 1;
}

}  // namespace

bool FindGame(GameFiles& game) {
    for (;;) {
        int found = Search(game);
        if (found) return found > 0;
        WaitForGame();
    }
}

}  // namespace app
