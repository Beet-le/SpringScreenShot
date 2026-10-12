#pragma once

#include "snow/image/codec.h"

#include <memory>
#include <functional>

namespace snow::image {

// Animation metadata is indexed without decompressing the image frames.
// GIF repeat counts are normalized to total plays; zero means infinite.
struct AnimationInfo final {
    DocumentInfo document;
    std::uint64_t total_play_count = 1;
};

// A session owns its input and mutable composition state. All operations,
// including destruction, must be serialized by its caller. It opens before
// frame zero. Advancing does not allocate an owning display-frame copy. Output
// is a full premultiplied RGBA8 canvas in the source orientation; frame selection,
// orientation application and downsampling options are not supported.
using AnimationCancellationCheck = std::function<bool()>;
using AnimationAdmissionCheck = std::function<bool(const AnimationInfo&)>;

class SNOW_IMAGE_API AnimationDecodeSession final {
  public:
    AnimationDecodeSession();
    ~AnimationDecodeSession();
    AnimationDecodeSession(AnimationDecodeSession&&) noexcept;
    AnimationDecodeSession& operator=(AnimationDecodeSession&&) noexcept;
    AnimationDecodeSession(const AnimationDecodeSession&) = delete;
    AnimationDecodeSession& operator=(const AnimationDecodeSession&) = delete;

    [[nodiscard]] const AnimationInfo& info() const noexcept;
    [[nodiscard]] std::int64_t frame_index() const noexcept;
    // False means the complete indexed sequence has ended. A failed/cancelled
    // advance requires reset before further advancement.
    [[nodiscard]] Result<bool> advance(std::stop_token stop = {},
                                       const AnimationCancellationCheck& cancelled = {});
    [[nodiscard]] Result<Frame> snapshot(std::stop_token stop = {}) const;
    // Publishes independent immutable pixels without copying document metadata.
    // Only premultiplied RGBA8 and BGRA8 are supported; color and orientation
    // declarations remain available through info().
    [[nodiscard]] Result<Image> snapshot_pixels(PixelFormat format,
                                                std::stop_token stop = {}) const;
    [[nodiscard]] Result<void> reset(std::stop_token stop = {});

  private:
    class Impl;
    explicit AnimationDecodeSession(std::unique_ptr<Impl> impl);
    [[nodiscard]] static Result<AnimationDecodeSession>
    open(const Input& input, const DecodeOptions& options, std::stop_token stop,
         const AnimationCancellationCheck& cancelled, const AnimationAdmissionCheck& admit);
    std::unique_ptr<Impl> impl_;
    friend class Service;
};

} // namespace snow::image
