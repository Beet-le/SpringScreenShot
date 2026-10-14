#ifndef SNOW_SHOT_PRESENTATION_WINDOWCAPTUREEXCLUSION_H
#define SNOW_SHOT_PRESENTATION_WINDOWCAPTUREEXCLUSION_H

#include <QPointer>
#include <QWidget>
#include <QVector>
#include <cstdint>
#include <optional>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace snow_shot::presentation {

class WindowCaptureExclusion final {
  public:
    using Setter = std::function<bool(QWidget*, bool)>;

    explicit WindowCaptureExclusion(Setter setter = {}) : m_setter(std::move(setter)) {}
    WindowCaptureExclusion(const WindowCaptureExclusion&) = delete;
    WindowCaptureExclusion& operator=(const WindowCaptureExclusion&) = delete;
    ~WindowCaptureExclusion() {
        restore();
    }

    // Exclusion is best effort and must never change a window's visibility.
    bool exclude(QWidget* window) {
        if (window == nullptr || !m_setter)
            return false;
        window = window->window();
        for (const auto& existing : m_windows) {
            if (existing->window == window)
                return true;
        }
        // Revalidate every new owner: the QWidget can survive replacement of its
        // native surface, which then needs its own native exclusion state.
        if (!m_setter(window, true))
            return false;
        auto& shared = sharedWindows();
        for (auto it = shared.begin(); it != shared.end();) {
            if (auto existing = it->lock()) {
                if (existing->window == window) {
                    m_windows.push_back(std::move(existing));
                    return true;
                }
                ++it;
            } else {
                it = shared.erase(it);
            }
        }
        auto lease = std::make_shared<WindowLease>(window);
        shared.push_back(lease);
        m_windows.push_back(std::move(lease));
        return true;
    }

    using IdResolver = std::function<std::optional<std::uint32_t>(QWidget*)>;
    QVector<std::uint32_t> windowIds(const IdResolver& resolve) const {
        QVector<std::uint32_t> ids;
        for (const auto& lease : m_windows) {
            const auto& window = lease->window;
            if (!window)
                continue;
            if (auto id = resolve(window.data()); id && !ids.contains(*id))
                ids.push_back(*id);
        }
        return ids;
    }

    void restore() {
        for (auto lease = m_windows.rbegin(); lease != m_windows.rend(); ++lease) {
            // A retired capture can finish after a new capture starts on the
            // same window. Only the final owner may restore native sharing.
            // Use that owner's setter, never a callback borrowed from an owner
            // whose controller may already have been destroyed.
            if (lease->use_count() == 1 && !(*lease)->window.isNull())
                static_cast<void>(m_setter((*lease)->window.data(), false));
            lease->reset();
        }
        m_windows.clear();
    }

  private:
    struct WindowLease {
        explicit WindowLease(QWidget* value) : window(value) {}
        QPointer<QWidget> window;
    };
    // QWidget/native mutation and every lease operation run on the GUI thread.
    // Weak entries neither extend window lifetime nor retain controller callbacks.
    static std::vector<std::weak_ptr<WindowLease>>& sharedWindows() {
        static std::vector<std::weak_ptr<WindowLease>> windows;
        return windows;
    }
    Setter m_setter;
    std::vector<std::shared_ptr<WindowLease>> m_windows;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_WINDOWCAPTUREEXCLUSION_H
