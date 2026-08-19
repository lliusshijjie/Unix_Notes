#pragma once

#include "mini_rpc/endpoint.h"

#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace minirpc {

class ServiceDiscovery {
public:
    void registerEndpoint(std::string service, Endpoint endpoint);
    void unregisterEndpoint(std::string_view service, const Endpoint& endpoint);
    std::vector<Endpoint> discover(std::string_view service) const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::vector<Endpoint>> services_;
};

}  // namespace minirpc
