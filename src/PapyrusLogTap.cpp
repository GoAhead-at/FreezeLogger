#include "PCH.h"
#include "PapyrusLogTap.h"

#include "RingBuffer.h"

namespace FreezeLogger::PapyrusLogTap {

    namespace {

        // Map BSScript::ErrorLogger::Severity -> compact tag for ring-buffer
        // text. The Papyrus.0.log format uses "error:" / "warning:" prefixes;
        // we use bracketed tags here so they line up with our own log style.
        const char* SeverityTag(RE::BSScript::ErrorLogger::Severity a_sev) noexcept {
            using S = RE::BSScript::ErrorLogger::Severity;
            switch (a_sev) {
            case S::kInfo:    return "INFO";
            case S::kWarning: return "WARN";
            case S::kError:   return "ERROR";
            case S::kFatal:   return "FATAL";
            default:          return "?";
            }
        }

        // Singleton sink registered with VirtualMachine::RegisterForLogEvent.
        // ProcessEvent() runs on whatever thread the VM is logging from, so
        // RingBuffer::Push (which is documented as thread-safe) does the
        // synchronisation. We must not throw out of ProcessEvent.
        class LogEventSink : public RE::BSTEventSink<RE::BSScript::LogEvent> {
        public:
            static LogEventSink* GetSingleton() noexcept {
                static LogEventSink instance;
                return &instance;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::BSScript::LogEvent*               a_event,
                RE::BSTEventSource<RE::BSScript::LogEvent>* /*a_source*/) override
            {
                if (a_event && a_event->errorMsg) {
                    try {
                        RingBuffer::Push(
                            RingBuffer::Channel::Papyrus,
                            std::format("[{}] {}", SeverityTag(a_event->severity),
                                        a_event->errorMsg));
                    } catch (...) {
                        // Swallow: the VM's event-source pump must never see
                        // an exception leak out of a sink.
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        std::atomic<bool> g_installed{false};

    }

    void Install() {
        if (g_installed.exchange(true)) {
            return;
        }

        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            logs::error("PapyrusLogTap::Install: VirtualMachine singleton was null.");
            g_installed.store(false);
            return;
        }

        vm->RegisterForLogEvent(LogEventSink::GetSingleton());
        logs::info("PapyrusLogTap installed (VM::RegisterForLogEvent).");
    }

}
