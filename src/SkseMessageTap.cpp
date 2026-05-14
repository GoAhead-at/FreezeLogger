#include "PCH.h"
#include "SkseMessageTap.h"

#include "Config.h"
#include "RingBuffer.h"

namespace FreezeLogger::SkseMessageTap {

    namespace {

        const char* MessageTypeName(std::uint32_t a_type) {
            using MI = SKSE::MessagingInterface;
            switch (a_type) {
            case MI::kPostLoad:           return "kPostLoad";
            case MI::kPostPostLoad:       return "kPostPostLoad";
            case MI::kPreLoadGame:        return "kPreLoadGame";
            case MI::kPostLoadGame:       return "kPostLoadGame";
            case MI::kSaveGame:           return "kSaveGame";
            case MI::kDeleteGame:         return "kDeleteGame";
            case MI::kInputLoaded:        return "kInputLoaded";
            case MI::kNewGame:            return "kNewGame";
            case MI::kDataLoaded:         return "kDataLoaded";
            default:                      return "<other>";
            }
        }

    }

    void Install() {
        const auto& cfg = Config::Get().ringbuffer;
        RingBuffer::Configure(cfg.papyrus_lines, cfg.skse_events);
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        if (!a_msg) return;
        const auto sender = a_msg->sender ? a_msg->sender : "<engine>";
        RingBuffer::Push(
            RingBuffer::Channel::Skse,
            std::format("type={} ({}) sender={}",
                        a_msg->type, MessageTypeName(a_msg->type), sender));
    }

}
