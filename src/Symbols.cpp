#include "PCH.h"
#include "Symbols.h"

#include "Config.h"

namespace FreezeLogger::Symbols {

    namespace {
        std::mutex   g_mutex;
        std::string  g_searchPath;
        bool         g_initialized = false;
    }

    Lock::Lock()  { g_mutex.lock();   }
    Lock::~Lock() { g_mutex.unlock(); }

    void Init() {
        std::scoped_lock lock(g_mutex);
        if (g_initialized) return;

        const auto cacheDir = Config::ResolvedSymbolCacheDir();
        std::error_code ec;
        if (!cacheDir.empty()) {
            std::filesystem::create_directories(cacheDir, ec);
        }

        std::string path;
        if (!cacheDir.empty()) {
            path += cacheDir.string();
            path += ';';
        }
        if (Config::Get().symbols.use_ms_symbol_server && !cacheDir.empty()) {
            path += "SRV*";
            path += cacheDir.string();
            path += "*https://msdl.microsoft.com/download/symbols";
        }

        g_searchPath = std::move(path);

        ::SymSetOptions(
            SYMOPT_DEFERRED_LOADS |
            SYMOPT_FAIL_CRITICAL_ERRORS |
            SYMOPT_LOAD_LINES |
            SYMOPT_AUTO_PUBLICS |
            SYMOPT_UNDNAME);

        const BOOL ok = ::SymInitialize(
            ::GetCurrentProcess(),
            g_searchPath.empty() ? nullptr : g_searchPath.c_str(),
            /*invadeProcess=*/TRUE);

        if (!ok) {
            logs::error("SymInitialize failed (last-error={}). Stack symbolication will be limited.",
                        ::GetLastError());
            return;
        }

        g_initialized = true;
        logs::info("DbgHelp initialized. Symbol search path: {}",
                   g_searchPath.empty() ? "(default)" : g_searchPath);
    }

    const std::string& SearchPath() {
        return g_searchPath;
    }

    namespace {
        // Body of Resolve() that ASSUMES the caller already holds g_mutex.
        // Used by both Resolve() (which takes the lock) and ResolveLocked()
        // (which does not). Splitting this out is what lets stack-walk
        // callers acquire Symbols::Lock once for the whole walk and then
        // call ResolveLocked() per frame without re-entering the mutex.
        std::string ResolveImpl(std::uintptr_t a_address) {
            if (!g_initialized) {
                return std::format("0x{:x}", a_address);
            }

            char moduleNameBuf[MAX_PATH] = {};
            std::string moduleName{"?"};

            HMODULE hMod = nullptr;
            if (::GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(a_address),
                    &hMod) &&
                ::GetModuleBaseNameA(::GetCurrentProcess(), hMod, moduleNameBuf, MAX_PATH))
            {
                moduleName = moduleNameBuf;
            }

            // SYMBOL_INFO + name buffer
            constexpr DWORD kMaxName = 1024;
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + kMaxName] = {};
            auto* sym       = reinterpret_cast<SYMBOL_INFO*>(buffer);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen   = kMaxName;

            DWORD64 displacement = 0;
            if (::SymFromAddr(::GetCurrentProcess(), a_address, &displacement, sym)) {
                return std::format("{}!{}+0x{:x}", moduleName, sym->Name, displacement);
            }

            const auto base = hMod ? reinterpret_cast<std::uintptr_t>(hMod) : 0;
            const auto off  = base ? (a_address - base) : a_address;
            return std::format("{}+0x{:x}", moduleName, off);
        }
    }

    std::string Resolve(std::uintptr_t a_address) {
        std::scoped_lock lock(g_mutex);
        return ResolveImpl(a_address);
    }

    std::string ResolveLocked(std::uintptr_t a_address) {
        return ResolveImpl(a_address);
    }

}
