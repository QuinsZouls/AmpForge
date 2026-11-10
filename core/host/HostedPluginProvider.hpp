#pragma once
#include "HostedPlugin.hpp"
#include <string>

namespace ampforge {

class HostedPluginProvider {
public:
    virtual ~HostedPluginProvider() = default;
    virtual HostedPlugin* getHostedPlugin(int slot) = 0;
    virtual void enqueuePluginParam(int slot, uint32_t paramId, float value) = 0;
};

} // namespace ampforge
