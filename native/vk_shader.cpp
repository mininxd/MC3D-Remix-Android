// HLSL to SPIR-V for the Vulkan renderer, through glslang's HLSL front end, with a cache on disk
#include "vk_shader.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <regex>
#include <thread>
#include <unordered_map>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include "core/log.h"

namespace vkshader {

namespace {

std::filesystem::path g_dir;
std::mutex g_lock;
std::mutex g_cache_lock;
std::unordered_map<uint64_t, std::vector<uint32_t>> g_mem_cache;

// Async background writer for packed SPIR-V archive (eliminates Android FUSE open/write stalls on render thread)
struct PendingSpv {
    uint64_t hash;
    std::vector<uint32_t> spirv;
};
std::mutex g_io_lock;
std::condition_variable g_io_cv;
std::deque<PendingSpv> g_io_queue;
bool g_io_started = false;

// bumped when the translation below changes, so older cached modules are not used
constexpr uint32_t kCacheVersion = 2;
constexpr uint32_t kPackMagic = 0x53505650u;  // 'SPVP'

struct Param {
    std::string type, name;
};

bool Opaque(const std::string& type) { return type.find("Texture") != std::string::npos || type.find("Sampler") != std::string::npos; }

std::vector<Param> Params(const std::string& list) {
    std::vector<Param> out;
    static const std::regex one(R"(\s*(.*?)\s*(\w+)\s*(,|$))");
    for (auto it = std::sregex_iterator(list.begin(), list.end(), one); it != std::sregex_iterator(); ++it)
        if ((*it)[2].length()) out.push_back({(*it)[1], (*it)[2]});
    return out;
}

// the index past the brace that closes the one at open
size_t CloseOf(const std::string& s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); i++) {
        if (s[i] == '{') depth++;
        if (s[i] == '}' && --depth == 0) return i + 1;
    }
    return std::string::npos;
}

// glslang without the optimizer hands a texture or sampler parameter over through a function variable, which
// Vulkan's SPIR-V forbids (a store to an opaque type, VUID 06924). Adreno drivers run it all the same; Mali's
// sample garbage through it, which the glow passes do with the SDK's spreadtap and shadow maps with its shadow:
// the glow came out saturated and flickering (issues 4 and 20). Each helper taking them is replaced by a copy per
// call site, named for the globals passed and reading them directly, declared after them, before main
std::string Specialize(const std::string& hlsl) {
    static const std::regex def(R"(\n(\w[\w<>]*)[ \t]+(\w+)\(([^()]*)\)\s*\{)");
    std::string s = hlsl, made;
    size_t from = 0;
    for (std::smatch m; std::regex_search(s.cbegin() + long(from), s.cend(), m, def);) {
        size_t start = from + size_t(m.position(0)), open = start + size_t(m.length(0)) - 1, end = CloseOf(s, open);
        std::vector<Param> params = Params(m[3]);
        size_t opaque = 0;
        while (opaque < params.size() && Opaque(params[opaque].type)) opaque++;
        if (!opaque || end == std::string::npos) {
            from = open;
            continue;
        }
        std::string type = m[1], name = m[2], body = s.substr(open, end - open), rest;
        for (size_t i = opaque; i < params.size(); i++) rest += (i > opaque ? ", " : "") + params[i].type + " " + params[i].name;
        s.erase(start, end - start);
        // the calls: name(global, global, ...) with one identifier per opaque parameter
        std::string pattern = "\\b" + name + "\\(";
        for (size_t i = 0; i < opaque; i++) pattern += R"(\s*(\w+)\s*,)";
        const std::regex call(pattern);
        std::string out;
        size_t at = 0;
        for (std::smatch c; std::regex_search(s.cbegin() + long(at), s.cend(), c, call);) {
            std::string spec = name, copy = body;
            for (size_t i = 0; i < opaque; i++) {
                spec += "_" + c[i + 1].str();
                copy = std::regex_replace(copy, std::regex("\\b" + params[i].name + "\\b"), c[i + 1].str());
            }
            std::string decl = "\n" + type + " " + spec + "(" + rest + ") " + copy + "\n";
            if (made.find(decl) == std::string::npos) made += decl;
            out += s.substr(at, size_t(c.position(0))) + spec + "(";
            at += size_t(c.position(0)) + size_t(c.length(0));
        }
        s = out + s.substr(at);
    }
    if (made.empty()) return hlsl;
    // the pixel shader's entry point follows the texture and sampler declarations
    size_t main = s.rfind("\nfloat4 main(");
    if (main == std::string::npos) return hlsl;
    return s.insert(main, made);
}

// fxc's spellings glslang does not read: 1.#INF for infinity
std::string Adapt(const std::string& hlsl) {
    std::string s = Specialize(hlsl);
    for (size_t at; (at = s.find("1.#INF")) != std::string::npos;) s.replace(at, 6, "asfloat(0x7F800000u)");
    // the vertex inputs at the location of their register, ATTRn: the pipeline's attributes are numbered so
    static const std::regex attr(R"((\n[ \t]*)(\w+ v\d+ : ATTR(\d+);))");
    return std::regex_replace(s, attr, "$1[[vk::location($3)]] $2");
}

std::filesystem::path CachePath(uint64_t hash) {
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx_%u.spv", (unsigned long long)hash, kCacheVersion);
    return g_dir / name;
}

std::filesystem::path PackPath() {
    char name[40];
    std::snprintf(name, sizeof(name), "shaders_pack_v%u.bin", kCacheVersion);
    return g_dir / name;
}

void StartIoThreadLocked() {
    if (g_io_started || g_dir.empty()) return;
    g_io_started = true;
    const std::filesystem::path pack_path = PackPath();
    std::thread([pack_path] {
        for (;;) {
            PendingSpv job;
            {
                std::unique_lock lk(g_io_lock);
                g_io_cv.wait(lk, [] { return !g_io_queue.empty(); });
                job = std::move(g_io_queue.front());
                g_io_queue.pop_front();
            }
            std::ofstream f(pack_path, std::ios::binary | std::ios::app);
            if (!f) continue;
            struct EntryHeader {
                uint32_t magic;
                uint32_t word_count;
                uint64_t hash;
            } hdr{kPackMagic, uint32_t(job.spirv.size()), job.hash};
            f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
            f.write(reinterpret_cast<const char*>(job.spirv.data()), std::streamsize(job.spirv.size() * 4));
        }
    }).detach();
}

void LoadPackIntoMemory() {
    if (g_dir.empty()) return;
    std::ifstream f(PackPath(), std::ios::binary);
    if (!f) return;
    size_t loaded = 0;
    struct EntryHeader {
        uint32_t magic;
        uint32_t word_count;
        uint64_t hash;
    } hdr{};
    std::lock_guard lk(g_cache_lock);
    while (f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
        if (hdr.magic != kPackMagic || hdr.word_count == 0 || hdr.word_count > (4u << 20)) break;
        std::vector<uint32_t> spv(hdr.word_count);
        if (!f.read(reinterpret_cast<char*>(spv.data()), std::streamsize(size_t(hdr.word_count) * 4))) break;
        g_mem_cache[hdr.hash] = std::move(spv);
        ++loaded;
    }
    if (loaded > 0) {
        LOGI("[OK] vkshader: preloaded %zu cached SPIR-V shaders from shaders_pack_v%u.bin", loaded, kCacheVersion);
    }
}

bool Load(uint64_t hash, std::vector<uint32_t>& spirv) {
    {
        std::lock_guard lk(g_cache_lock);
        auto it = g_mem_cache.find(hash);
        if (it != g_mem_cache.end()) {
            spirv = it->second;
            return true;
        }
    }
    if (g_dir.empty()) return false;
    std::ifstream f(CachePath(hash), std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    if (n <= 0 || n % 4) return false;
    spirv.resize(size_t(n) / 4);
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(spirv.data()), n)) return false;
    // Migrate legacy individual .spv into packed cache asynchronously
    {
        std::lock_guard lk(g_cache_lock);
        g_mem_cache[hash] = spirv;
    }
    {
        std::lock_guard lk(g_io_lock);
        StartIoThreadLocked();
        g_io_queue.push_back({hash, spirv});
    }
    g_io_cv.notify_one();
    return true;
}

void Store(uint64_t hash, const std::vector<uint32_t>& spirv) {
    {
        std::lock_guard lk(g_cache_lock);
        g_mem_cache[hash] = spirv;
    }
    if (g_dir.empty()) return;
    {
        std::lock_guard lk(g_io_lock);
        StartIoThreadLocked();
        g_io_queue.push_back({hash, spirv});
    }
    g_io_cv.notify_one();
}

}  // namespace

void Init(const std::filesystem::path& cache_dir) {
    std::lock_guard lock(g_lock);
    glslang::InitializeProcess();
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    g_dir = ec ? std::filesystem::path() : cache_dir;
    LoadPackIntoMemory();
}

bool Compile(const std::string& hlsl, const char* entry, Stage stage, uint64_t hash, std::vector<uint32_t>& spirv) {
    if (Load(hash, spirv)) return true;
    std::lock_guard lock(g_lock);
    EShLanguage lang = stage == Stage::Vertex ? EShLangVertex : EShLangFragment;
    std::string text = Adapt(hlsl);
    const char* src = text.c_str();
    glslang::TShader sh(lang);
    sh.setStrings(&src, 1);
    sh.setEntryPoint("main");
    sh.setSourceEntryPoint(entry);
    sh.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    sh.setHlslIoMapping(true);
    sh.setAutoMapBindings(true);
    sh.setAutoMapLocations(true);
    sh.setShiftBinding(glslang::EResUbo, kBindingConstants);
    sh.setShiftBinding(glslang::EResTexture, kBindingTextures);
    sh.setShiftBinding(glslang::EResSampler, kBindingSamplers);
    auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    if (!sh.parse(GetDefaultResources(), 100, false, messages)) {
        LOGE("vulkan: %s %s does not compile: %s", stage == Stage::Vertex ? "vertex" : "pixel", entry, sh.getInfoLog());
        LOGE("%s", text.c_str());
        return false;
    }
    glslang::TProgram program;
    program.addShader(&sh);
    if (!program.link(messages) || !program.mapIO()) {
        LOGE("vulkan: %s does not link: %s", entry, program.getInfoLog());
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    options.validate = false;
    spirv.clear();
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &options);
    if (spirv.empty()) return false;
    Store(hash, spirv);
    return true;
}

}  // namespace vkshader
