#pragma once

#include <SKSE/Interfaces.h>

namespace FreezeLogger::SkseMessageTap {

    void Install();
    void OnMessage(SKSE::MessagingInterface::Message* a_msg);

}
