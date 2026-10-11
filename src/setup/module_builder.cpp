#include "setup/module_builder.h"

#include "port/dol.h"
#include "recomp/ctx.h"
#include "recompiler/recompiler.h"
#include "setup/embedded.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace setup {
namespace {

// Flags for the generated code. Freestanding: no headers, no C runtime (module_rt.cpp
// provides the few symbols the compiler may call), no thread-safe statics (they need the
// CRT; only one guest thread runs at a time). Unwind tables are required: the runtime ends
// guest threads (OSExitThread, OSCancelThread, shutdown) by throwing sched::ThreadExit through
// the module's frames, which Windows can only unwind with .pdata and Linux with .eh_frame
// (+ the .eh_frame_hdr lookup table the linker writes).
#ifdef _WIN32
// Windows: stack probes stay on (module_rt.cpp provides __chkstk: a generated function with
// a frame above a page would otherwise skip the stack guard page and fault; SMS port fix).
const std::vector<std::string> kCompileFlags = {
    "--target=x86_64-pc-windows-msvc", "-std=c++20", "-O2", "-ffreestanding", "-nostdinc", "-fno-exceptions",
    "-fno-rtti", "-fno-threadsafe-statics", "-fno-math-errno", "-fno-stack-protector", "-funwind-tables",
    "-Wno-unused-label", "-Wno-unused-variable"};
const std::vector<std::string> kLinkFlags = {"/dll", "/noentry", "/nodefaultlib", "/machine:x64", "/opt:ref",
                                             "/incremental:no"};
constexpr const char* kModuleFile = "module.dll";
constexpr const char* kObjExt = ".obj";
constexpr const char* kExeSuffix = ".exe";
constexpr const char* kLinkerName = "lld-link";
#else
// Linux: a position-independent shared object. Everything is hidden and bound locally
// (-fvisibility=hidden, -Bsymbolic) except lm_module_init, so the module's g_mem, memcpy, ...
// can never be interposed by the executable's or libc's symbols. -z defs turns a missing
// helper (one the compiler calls that module_rt.cpp does not provide) into a link error
// instead of a dlopen failure.
const std::vector<std::string> kCompileFlags = {
    "--target=x86_64-pc-linux-gnu", "-std=c++20", "-O2", "-fPIC", "-fvisibility=hidden", "-ffreestanding",
    "-nostdinc", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-fno-math-errno",
    "-fno-stack-protector", "-funwind-tables", "-Wno-unused-label", "-Wno-unused-variable"};
const std::vector<std::string> kLinkFlags = {"-shared", "-nostdlib", "--eh-frame-hdr", "-Bsymbolic",
                                             "-z", "defs", "-z", "noexecstack", "-z", "now", "-O1",
                                             "--hash-style=both", "-soname", "lm_game_module.so"};
constexpr const char* kModuleFile = "module.so";
constexpr const char* kObjExt = ".o";
constexpr const char* kExeSuffix = "";
constexpr const char* kLinkerName = "ld.lld";
#endif

bool writeText(const std::filesystem::path& p, std::string_view text) {
    std::ofstream o(p, std::ios::binary);
    o.write(text.data(), std::streamsize(text.size()));
    return bool(o);
}

std::string utf8(const std::filesystem::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string readText(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// Runs a tool and waits; its output goes to `log`. Returns the exit code (-1: not started).
int run(const std::filesystem::path& exe, const std::vector<std::string>& args, const std::filesystem::path& log) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    for (const auto& a : args) cmd += L" \"" + widen(a) + L"\"";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}
#else
// Runs a tool and waits; stdout/stderr go to `log`, stdin is /dev/null. Returns the exit
// code (-1: not started, 128 + n: killed by signal n). The tool runs at a lower priority so
// the setup screen stays responsive while every core compiles.
int run(const std::filesystem::path& exe, const std::vector<std::string>& args, const std::filesystem::path& log) {
    std::vector<std::string> argStore;
    argStore.push_back(exe.string());
    argStore.insert(argStore.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : argStore) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid = -1;
    const int err = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err != 0) return -1;
    setpriority(PRIO_PROCESS, id_t(pid), 10);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}
#endif

std::filesystem::path exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
    return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
}

}  // namespace

std::filesystem::path moduleDir(port::GameVersion v) { return port::dataDir() / "game" / port::gameId(v); }
std::filesystem::path modulePath(port::GameVersion v) { return moduleDir(v) / kModuleFile; }

std::string moduleKey(const Disc& disc) {
    const bool pal = disc.version() == port::GameVersion::PAL;
    std::string text = "abi " + std::to_string(recomp::kModuleAbiVersion) + "\n";
    text += std::string("game ") + port::gameId(disc.version()) + " " + disc.dolSha1() + "\n";
    for (auto& f : kCompileFlags) text += f + " ";
    for (auto& f : kLinkFlags) text += f + " ";
    for (std::string_view part : {embedded::symbols(pal), embedded::splits(pal), embedded::versionMap(pal),
                                  embedded::hooks(), embedded::ctxHeader(), embedded::moduleAbiHeader(),
                                  embedded::moduleRt(), embedded::recompilerSource()}) {
        const auto d = port::dol::sha1(reinterpret_cast<const std::uint8_t*>(part.data()), part.size());
        text += hex(d.data(), d.size()) + "\n";
    }
    const auto d = port::dol::sha1(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return hex(d.data(), d.size());
}

bool moduleUpToDate(const Disc& disc) {
    const auto dir = moduleDir(disc.version());
    std::error_code ec;
    return std::filesystem::exists(dir / kModuleFile, ec) && readText(dir / "module.key") == moduleKey(disc);
}

bool findToolchain(std::filesystem::path& clang, std::filesystem::path& lld, std::string& error) {
    const char* exe = kExeSuffix;
    std::vector<std::filesystem::path> dirs{exeDir() / "llvm" / "bin"};
    if (const char* env = std::getenv("LM_LLVM_DIR")) dirs.emplace_back(env);
#ifdef LM_DEV_LLVM_DIR
    dirs.emplace_back(LM_DEV_LLVM_DIR);
#endif
    if (const char* path = std::getenv("PATH")) {
        std::string p = path;
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        for (size_t pos = 0; pos <= p.size();) {
            size_t end = p.find(sep, pos);
            if (end == std::string::npos) end = p.size();
            if (end > pos) dirs.emplace_back(p.substr(pos, end - pos));
            pos = end + 1;
        }
    }
    for (const auto& d : dirs) {
        std::error_code ec;
        const auto c = d / (std::string("clang") + exe), l = d / (std::string(kLinkerName) + exe);
        if (std::filesystem::exists(c, ec) && std::filesystem::exists(l, ec)) {
            clang = c;
            lld = l;
            return true;
        }
    }
#ifdef _WIN32
    error = "The bundled compiler (llvm\\bin\\clang.exe and lld-link.exe next to the game) is missing. "
            "Reinstall the game, or set LM_LLVM_DIR to an LLVM bin directory.";
#else
    error = "The bundled compiler (llvm/bin/clang and llvm/bin/ld.lld next to the game) is missing. "
            "Reinstall the game, or set LM_LLVM_DIR to an LLVM bin directory.";
#endif
    return false;
}

bool buildModule(const Disc& disc, const Progress& progress, std::string& error) {
    auto report = [&](const std::string& s, float f) {
        if (progress.report) progress.report(s, f);
    };
    auto cancelled = [&] { return progress.cancel && progress.cancel->load(); };
    std::filesystem::path clang, lld;
    if (!findToolchain(clang, lld, error)) return false;
    std::printf("[setup] building the %s game module with %s and %s\n", port::gameId(disc.version()),
                utf8(clang).c_str(), utf8(lld).c_str());
    std::fflush(stdout);
    const auto buildStart = std::chrono::steady_clock::now();

    const bool pal = disc.version() == port::GameVersion::PAL;
    const auto dir = moduleDir(disc.version());
    const auto work = dir / "build";
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work / "src", ec);
    std::filesystem::create_directories(work / "obj", ec);
    std::filesystem::create_directories(work / "include" / "recomp", ec);

    // 1. Recompile.
    report("Translating the game code", 0.0f);
    lmrc::Input in;
    in.dol = disc.dol();
    in.version = port::gameId(disc.version());
    in.symbols = std::string(embedded::symbols(pal));
    in.splits = std::string(embedded::splits(pal));
    in.versionMap = std::string(embedded::versionMap(pal));
    in.hooks = std::string(embedded::hooks());
    lmrc::Output out;
    if (!lmrc::recompile(in, out, error)) {
        error = "Recompiling main.dol failed: " + error;
        return false;
    }
    std::vector<std::filesystem::path> sources;
    for (auto& [name, text] : out.files) {
        if (!writeText(work / "src" / name, text)) {
            error = "Could not write to " + (work / "src").string();
            return false;
        }
        if (name.size() > 4 && name.substr(name.size() - 4) == ".cpp") sources.push_back(work / "src" / name);
    }
    out.files.clear();
    writeText(work / "include" / "recomp" / "ctx.h", embedded::ctxHeader());
    writeText(work / "include" / "recomp" / "module_abi.h", embedded::moduleAbiHeader());
    writeText(work / "src" / "module_rt.cpp", embedded::moduleRt());
    sources.push_back(work / "src" / "module_rt.cpp");
    writeText(work / "recompiler.log", out.summary + "\n");
    if (cancelled()) return false;

    // 2. Compile in parallel (largest files first keeps all cores busy to the end).
    std::sort(sources.begin(), sources.end(), [](auto& a, auto& b) {
        std::error_code e;
        return std::filesystem::file_size(a, e) > std::filesystem::file_size(b, e);
    });
    const unsigned jobs = std::max(1u, std::min<unsigned>(unsigned(sources.size()), std::thread::hardware_concurrency()));
    std::mutex m;
    size_t next = 0, done = 0;
    std::string failure;
    report("Compiling the game", 0.02f);
    auto worker = [&] {
        for (;;) {
            size_t i;
            {
                std::lock_guard lk(m);
                if (next >= sources.size() || !failure.empty() || cancelled()) return;
                i = next++;
            }
            const auto& src = sources[i];
            auto obj = work / "obj" / src.stem();
            obj += kObjExt;
            auto log = work / "obj" / src.stem();
            log += ".log";
            std::vector<std::string> args(kCompileFlags.begin(), kCompileFlags.end());
            args.push_back("-I" + utf8(work / "include"));
            args.push_back("-c");
            args.push_back(utf8(src));
            args.push_back("-o");
            args.push_back(utf8(obj));
            const int rc = run(clang, args, log);
            std::lock_guard lk(m);
            if (rc != 0 && failure.empty())
                failure = "Compiling " + src.filename().string() + " failed (exit " + std::to_string(rc) + "):\n" +
                          readText(log).substr(0, 2000);
            done++;
            report("Compiling the game (" + std::to_string(done) + "/" + std::to_string(sources.size()) + ")",
                   0.02f + 0.93f * float(done) / float(sources.size()));
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < jobs; t++) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    if (cancelled()) {
        error = "Cancelled.";
        return false;
    }
    if (!failure.empty()) {
        error = failure;
        return false;
    }

    // 3. Link.
    report("Linking the game module", 0.96f);
    const auto tmpDll = work / kModuleFile;
    std::vector<std::string> args(kLinkFlags.begin(), kLinkFlags.end());
#ifdef _WIN32
    args.push_back("/out:" + utf8(tmpDll));
    args.push_back("/implib:" + utf8(work / "module.lib"));
#else
    args.push_back("-o");
    args.push_back(utf8(tmpDll));
#endif
    for (const auto& s : sources) {
        auto obj = work / "obj" / s.stem();
        obj += kObjExt;
        args.push_back(utf8(obj));
    }
    if (const int rc = run(lld, args, work / "link.log"); rc != 0) {
        error = "Linking the game module failed (exit " + std::to_string(rc) + "):\n" + readText(work / "link.log").substr(0, 2000);
        return false;
    }
    std::filesystem::remove(dir / kModuleFile, ec);
    std::filesystem::rename(tmpDll, dir / kModuleFile, ec);
    if (ec) {
        error = "Could not install the game module: " + ec.message();
        return false;
    }
    writeText(dir / "module.key", moduleKey(disc));
    writeText(dir / "recompiler.log", out.summary + "\n");
    // Generated code is derived from the user's game; keep it only while building.
    if (!std::getenv("LM_KEEP_BUILD")) std::filesystem::remove_all(work, ec);
    std::printf("[setup] game module built in %.1f s (%u compile jobs)\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - buildStart).count(), jobs);
    std::fflush(stdout);
    report("Done", 1.0f);
    return true;
}

}  // namespace setup
