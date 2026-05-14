#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

namespace FreezeLogger::RingBuffer {

    enum class Channel : std::uint8_t {
        Papyrus = 0,
        Skse    = 1,
    };

    struct Entry {
        std::uint64_t time_ms;     // GetTickCount64 at capture time
        Channel       channel;
        std::string   text;
    };

    // Thread-safe push. Capacity per channel is set by Configure().
    void Push(Channel a_channel, std::string a_text);

    // Sets the maximum entries per channel; call once after Config::Load().
    void Configure(std::size_t a_papyrusCapacity, std::size_t a_skseCapacity);

    // Returns a copy of the current contents (Papyrus first, then SKSE),
    // each in chronological order (oldest first).
    std::vector<Entry> Snapshot();

    // Convenience: writes the current ring-buffer contents to a stream
    // in the format used by the freeze report.
    void WriteSnapshot(std::ostream& a_os);

}
