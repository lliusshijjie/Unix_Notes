#include "mini_rpc/service_registry.h"

#include <mutex>
#include <utility>

namespace minirpc {

bool ServiceRegistry::registerMethod(std::string serviceName, std::string methodName,
                                     MethodHandler handler) {
    if (serviceName.empty() || methodName.empty() || !handler) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    MethodMap& methods = services_[std::move(serviceName)];
    return methods.emplace(std::move(methodName), std::move(handler)).second;
}

LookupResult ServiceRegistry::findMethod(std::string_view serviceName,
                                         std::string_view methodName) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto service = services_.find(std::string(serviceName));
    if (service == services_.end()) {
        return {LookupStatus::ServiceNotFound, {}};
    }

    const auto method = service->second.find(std::string(methodName));
    if (method == service->second.end()) {
        return {LookupStatus::MethodNotFound, {}};
    }
    return {LookupStatus::Found, method->second};
}

}  // namespace minirpc
