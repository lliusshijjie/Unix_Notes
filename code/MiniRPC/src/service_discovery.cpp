#include "mini_rpc/service_discovery.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace minirpc {

void ServiceDiscovery::registerEndpoint(std::string service, Endpoint endpoint) {
    if (service.empty() || endpoint.host.empty() || endpoint.port == 0) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    std::vector<Endpoint>& endpoints = services_[std::move(service)];
    if (std::find(endpoints.begin(), endpoints.end(), endpoint) != endpoints.end()) {
        return;
    }
    endpoints.push_back(std::move(endpoint));
}

void ServiceDiscovery::unregisterEndpoint(std::string_view service, const Endpoint& endpoint) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto found = services_.find(std::string(service));
    if (found == services_.end()) {
        return;
    }

    auto& endpoints = found->second;
    endpoints.erase(std::remove(endpoints.begin(), endpoints.end(), endpoint), endpoints.end());
    if (endpoints.empty()) {
        services_.erase(found);
    }
}

std::vector<Endpoint> ServiceDiscovery::discover(std::string_view service) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto found = services_.find(std::string(service));
    if (found == services_.end()) {
        return {};
    }
    return found->second;
}

}  // namespace minirpc
