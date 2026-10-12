#pragma once

#include "snow_canvas_runtime_access.h"
#include "snow_draw_engine.h"

#include <QList>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class SnowCanvasRuntime;

namespace snow_canvas_runtime {

class ClientRegistry final {
    struct Registration {
        Client* client;
    };

  public:
    using DetachedClients = QList<std::shared_ptr<Registration>>;

    ClientRegistry() = default;
    ~ClientRegistry();
    ClientRegistry(const ClientRegistry&) = delete;
    ClientRegistry& operator=(const ClientRegistry&) = delete;

    void registerClient(Client* client);
    void unregisterClient(Client* client);

    DetachedClients detachForRuntimeReplacement(const std::function<bool()>& isCurrent);
    void attachAfterRuntimeReplacement(const DetachedClients& clients, SnowRuntime runtime,
                                       const std::function<bool()>& isCurrent);
    void detachAndReleaseClients(SnowCanvasRuntime& owner);
    void abandonClientsWithoutDetach();
    void clearRenderState();
    void resetDocumentRetainedState();
    void smartEraseChanged();
    bool resetEditingState();
    void syncChangedViewports(std::vector<std::uint64_t> changedViewportIds);

  private:
    // Publications retain registrations, rather than the registry or raw clients.
    // Unregistering cancels an entry even if a new client reuses the same address.
    DetachedClients m_clients;
    // Registrations admitted while detaching still belong to the retiring engine.
    // Nested replacements temporarily own a separate queue on their caller's stack.
    DetachedClients* m_replacementDetachQueue = nullptr;
    std::shared_ptr<const int> m_lifetime = std::make_shared<const int>(0);
};

} // namespace snow_canvas_runtime
