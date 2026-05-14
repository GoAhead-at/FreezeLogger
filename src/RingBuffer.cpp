#include "RingBuffer.h"

#include <algorithm>
#include <format>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#endif

namespace FreezeLogger::RingBuffer {

    namespace {
        std::mutex         g_mutex;
        std::deque<Entry>  g_papyrus;
        std::deque<Entry>  g_skse;
        std::size_t        g_capPapyrus = 100;
        std::size_t        g_capSkse    = 50;

        std::deque<Entry>& Bucket(Channel c) {
            return c == Channel::Papyrus ? g_papyrus : g_skse;
        }

        std::size_t Capacity(Channel c) {
            return c == Channel::Papyrus ? g_capPapyrus : g_capSkse;
        }

        std::uint64_t Now() {
#ifdef _WIN32
            return ::GetTickCount64();
#else
            return 0;
#endif
        }
    }

    void Configure(std::size_t a_papyrusCapacity, std::size_t a_skseCapacity) {
        std::scoped_lock lock(g_mutex);
        g_capPapyrus = a_papyrusCapacity;
        g_capSkse    = a_skseCapacity;
        while (g_papyrus.size() > g_capPapyrus) g_papyrus.pop_front();
        while (g_skse.size()    > g_capSkse)    g_skse.pop_front();
    }

    void Push(Channel a_channel, std::string a_text) {
        Entry entry{Now(), a_channel, std::move(a_text)};
        std::scoped_lock lock(g_mutex);
        auto& bucket = Bucket(a_channel);
        bucket.push_back(std::move(entry));
        while (bucket.size() > Capacity(a_channel)) {
            bucket.pop_front();
        }
    }

    std::vector<Entry> Snapshot() {
        std::scoped_lock lock(g_mutex);
        std::vector<Entry> out;
        out.reserve(g_papyrus.size() + g_skse.size());
        out.insert(out.end(), g_papyrus.begin(), g_papyrus.end());
        out.insert(out.end(), g_skse.begin(),    g_skse.end());
        return out;
    }

    void WriteSnapshot(std::ostream& a_os) {
        const auto entries = Snapshot();

        std::size_t papyrusCount = 0, skseCount = 0;
        for (auto& e : entries) {
            (e.channel == Channel::Papyrus ? papyrusCount : skseCount)++;
        }

        a_os << "Papyrus log (last " << papyrusCount << " lines):\n";
        for (auto& e : entries) {
            if (e.channel != Channel::Papyrus) continue;
            a_os << "  [t=" << e.time_ms << "ms] " << e.text << "\n";
        }
        a_os << "\nSKSE messages (last " << skseCount << "):\n";
        for (auto& e : entries) {
            if (e.channel != Channel::Skse) continue;
            a_os << "  [t=" << e.time_ms << "ms] " << e.text << "\n";
        }
    }

}
