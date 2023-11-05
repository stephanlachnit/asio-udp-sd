#include "Manager.hpp"

#include <functional>

#include "CHIRP/exceptions.hpp"

using namespace cnstln::CHIRP;

bool RegisteredService::operator<(const RegisteredService& other) const {
    // Sort first by service id
    auto ord_id = std::to_underlying(identifier) <=> std::to_underlying(other.identifier);
    if (std::is_lt(ord_id)) {
        return true;
    }
    if (std::is_gt(ord_id)) {
        return false;
    }
    // Then by port
    return port < other.port;
}

bool DiscoveredService::operator<(const DiscoveredService& other) const {
    // Ignore IP when sorting, we only care about the name
    auto ord_md5 = name_hash <=> other.name_hash;
    if (std::is_lt(ord_md5)) {
        return true;
    }
    if (std::is_gt(ord_md5)) {
        return false;
    }
    // Same as RegisteredService::operator<
    auto ord_id = std::to_underlying(identifier) <=> std::to_underlying(other.identifier);
    if (std::is_lt(ord_id)) {
        return true;
    }
    if (std::is_gt(ord_id)) {
        return false;
    }
    return port < other.port;
}

Manager::Manager(asio::io_context& io_context, asio::ip::address brd_address, asio::ip::address any_address, std::string_view group, std::string_view name)
  : receiver_(io_context, any_address), sender_(io_context, brd_address),
    group_hash_(MD5Hash(group)), name_hash_(MD5Hash(name)) {}

Manager::~Manager() {
    // First stop Run function
    run_thread_.request_stop();
    if (run_thread_.joinable()) {
        run_thread_.join();
    }
    // Now unregister all services
    UnregisterServices();
}

void Manager::Start() {
    // jthread immediatly starts on construction
    run_thread_ = std::jthread(std::bind_front(&Manager::Run, this));
}

bool Manager::RegisterService(RegisteredService service) {
    std::unique_lock registered_services_lock {registered_services_mutex_};
    const auto insert_ret = registered_services_.insert(service);
    const bool actually_inserted = insert_ret.second;

    // Lock not needed anymore
    registered_services_lock.unlock();
    if (actually_inserted) {
        SendMessage(OFFER, service);
    }
    return actually_inserted;
}

bool Manager::UnregisterService(RegisteredService service) {
    std::unique_lock registered_services_lock {registered_services_mutex_};

    const auto erase_ret = registered_services_.erase(service);
    bool actually_erased = erase_ret > 0 ? true : false;

    // Lock not needed anymore
    registered_services_lock.unlock();
    if (actually_erased) {
        SendMessage(LEAVING, service);
    }
    return actually_erased;
}

void Manager::UnregisterServices() {
    const std::lock_guard registered_services_lock {registered_services_mutex_};
    for (auto service : registered_services_) {
        SendMessage(LEAVING, service);
    }
    registered_services_.clear();
}

std::set<RegisteredService> Manager::GetRegisteredServices() {
    const std::lock_guard registered_services_lock {registered_services_mutex_};
    return registered_services_;
}

bool Manager::RegisterDiscoverCallback(DiscoverCallback* callback, void* data) {
    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
    const auto insert_ret = discover_callbacks_.emplace(callback, data);

    // Return if actually inserted
    return insert_ret.second;
}

bool Manager::UnregisterDiscoverCallback(DiscoverCallback* callback, void* data) {
    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
    const auto erase_ret = discover_callbacks_.erase(std::make_pair(callback, data));

    // Return if actually erased
    return erase_ret > 0;
}

void Manager::UnregisterDiscoverCallbacks() {
    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
    discover_callbacks_.clear();
}

std::set<DiscoveredService> Manager::GetDiscoveredServices() {
    const std::lock_guard discovered_services_lock {discovered_services_mutex_};
    return discovered_services_;
}

void Manager::SendMessage(MessageType type, RegisteredService service) {
    const auto asm_msg = Message(type, group_hash_, name_hash_, service.identifier, service.port).Assemble();
    sender_.SendBroadcast(asm_msg.data(), asm_msg.size());
}

void Manager::Run(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        try {
            auto raw_msg = receiver_.RecvBroadcast();  // TODO: timeout

            auto chirp_msg = Message(AssembledMessage(raw_msg.content));

            if (chirp_msg.GetGroupHash() != group_hash_) {
                // Broadcast from different group, ignore
                continue;
            }
            if (chirp_msg.GetNameHash() != name_hash_) {
                // Broadcast from self, ignore
                continue;
            }

            DiscoveredService discovered_service {raw_msg.ip, chirp_msg.GetNameHash(), chirp_msg.GetServiceIdentifier(), chirp_msg.GetPort()};

            switch (chirp_msg.GetType()) {
            case REQUEST: {
                auto service_id = discovered_service.identifier;
                const std::lock_guard registered_services_lock {registered_services_mutex_};
                // Replay OFFERs for registered services with same service identifier
                for (const auto& service : registered_services_) {
                    if (service.identifier == service_id) {
                        SendMessage(OFFER, service);
                    }
                }
                break;
            }
            case OFFER: {
                std::unique_lock discovered_services_lock {discovered_services_mutex_};
                if (!discovered_services_.contains(discovered_service)) {
                    discovered_services_.insert(discovered_service);

                    // Unlock discovered_services_lock for user callback
                    discovered_services_lock.unlock();
                    // Acquire lock for discover_callbacks_
                    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
                    // Loop over callback and run as detached threads
                    for (const auto& callback_data : discover_callbacks_) {
                        std::thread(callback_data.first, discovered_service, false, callback_data.second).detach();
                    }
                }
                break;
            }
            case LEAVING: {
                std::unique_lock discovered_services_lock {discovered_services_mutex_};
                if (discovered_services_.contains(discovered_service)) {
                    discovered_services_.erase(discovered_service);

                    // Unlock discovered_services_lock for user callback
                    discovered_services_lock.unlock();
                    // Acquire lock for discover_callbacks_
                    const std::lock_guard discover_callbacks_lock {discover_callbacks_mutex_};
                    // Loop over callback and run as detached threads
                    for (const auto& callback_data : discover_callbacks_) {
                        std::thread(callback_data.first, discovered_service, true, callback_data.second).detach();
                    }
                }
                break;
            }
            default:
                std::unreachable();
            }
        }
        catch (const DecodeError& error) {
            continue;
        }
    }
}