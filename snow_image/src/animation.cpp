#include "snow/image/animation.h"

#include "codec_registry.h"
#include "snow/image/service.h"

#include <snow/memory/pixel_array.h>

#ifdef SNOW_IMAGE_HAS_GIF
#include <gif_lib.h>
#endif
#ifdef SNOW_IMAGE_HAS_WEBP
#include <webp/demux.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace snow::image {
namespace {

thread_local const AnimationCancellationCheck* currentCancellation = nullptr;
thread_local std::stop_token currentStop;
struct CancellationScope final {
    const AnimationCancellationCheck* previous = currentCancellation;
    std::stop_token previousStop = currentStop;
    CancellationScope(const AnimationCancellationCheck& check, std::stop_token stop) {
        currentCancellation = &check;
        currentStop = stop;
    }
    ~CancellationScope() {
        currentCancellation = previous;
        currentStop = previousStop;
    }
};
struct Cancelled final {};
bool cancellationRequested(std::stop_token stop = {}) noexcept {
    if (stop.stop_requested() || currentStop.stop_requested())
        return true;
    try {
        return currentCancellation && *currentCancellation && (*currentCancellation)();
    } catch (...) {
        return true;
    }
}

constexpr PixelFormat kPremultipliedRgba8{SampleType::unsigned_integer, ChannelLayout::rgba,
                                          AlphaMode::premultiplied, 8, true};
constexpr PixelFormat kPremultipliedBgra8{SampleType::unsigned_integer, ChannelLayout::bgra,
                                          AlphaMode::premultiplied, 8, true};

void copyBgraRow(const std::byte* source, std::byte* destination, std::uint32_t width) {
    // Packed loads/stores avoid alignment and aliasing assumptions while allowing
    // the compiler to vectorize the copy and channel swap together.
    for (std::uint32_t x = 0; x < width; ++x) {
        std::uint32_t pixel;
        std::memcpy(&pixel, source + static_cast<std::size_t>(x) * 4U, sizeof(pixel));
        if constexpr (std::endian::native == std::endian::little) {
            const auto redBlue = pixel & 0x00ff00ffU;
            pixel = (pixel & 0xff00ff00U) | (redBlue << 16U) | (redBlue >> 16U);
        } else if constexpr (std::endian::native == std::endian::big) {
            const auto redBlue = pixel & 0xff00ff00U;
            pixel = (pixel & 0x00ff00ffU) | (redBlue << 16U) | (redBlue >> 16U);
        } else {
            auto channels = std::bit_cast<std::array<std::byte, 4>>(pixel);
            std::swap(channels[0], channels[2]);
            pixel = std::bit_cast<std::uint32_t>(channels);
        }
        std::memcpy(destination + static_cast<std::size_t>(x) * 4U, &pixel, sizeof(pixel));
    }
}

Status invalid(const char* message) {
    return Status::error(ErrorCode::corrupt_data, message, "animation");
}

Status limited(const char* message) {
    return Status::error(ErrorCode::limit_exceeded, message, "animation");
}

std::uint32_t be32(std::span<const std::byte> bytes) {
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
           std::to_integer<std::uint32_t>(bytes[3]);
}

std::uint16_t le16(std::span<const std::byte> bytes) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[0]) |
                                      (std::to_integer<unsigned>(bytes[1]) << 8U));
}

std::uint16_t be16(std::span<const std::byte> bytes) {
    return static_cast<std::uint16_t>((std::to_integer<unsigned>(bytes[0]) << 8U) |
                                      std::to_integer<unsigned>(bytes[1]));
}

void append32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (const unsigned shift : {24U, 16U, 8U, 0U})
        bytes.push_back(static_cast<std::byte>((value >> shift) & 255U));
}

std::uint32_t crcUpdate(std::uint32_t value, std::span<const std::byte> bytes) {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t index = 0; index < result.size(); ++index) {
            std::uint32_t entry = index;
            for (int bit = 0; bit < 8; ++bit)
                entry = (entry >> 1U) ^ ((entry & 1U) ? 0xedb88320U : 0U);
            result[index] = entry;
        }
        return result;
    }();
    std::size_t checked = 0;
    for (const auto byte : bytes) {
        if ((checked++ & 65535U) == 0 && cancellationRequested())
            throw Cancelled{};
        value = table[(value ^ std::to_integer<unsigned>(byte)) & 255U] ^ (value >> 8U);
    }
    return value;
}

void appendChunk(std::vector<std::byte>& bytes, const char* type, std::span<const std::byte> data) {
    append32(bytes, static_cast<std::uint32_t>(data.size()));
    const auto begin = bytes.size();
    bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(type),
                 reinterpret_cast<const std::byte*>(type) + 4);
    bytes.insert(bytes.end(), data.begin(), data.end());
    append32(bytes, crcUpdate(0xffffffffU, std::span(bytes).subspan(begin)) ^ 0xffffffffU);
}

struct EncodedSpan final {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::uint32_t png_crc = 0;
};

struct IndexedFrame final {
    FrameInfo info;
    std::vector<EncodedSpan> data;
    bool gif_transparent = false;
};

bool hasCompressedData(const IndexedFrame& frame) {
    return std::any_of(frame.data.begin(), frame.data.end(),
                       [](const EncodedSpan& span) { return span.size > 0; });
}

struct Index final {
    AnimationInfo info;
    std::vector<IndexedFrame> frames;
    std::vector<EncodedSpan> png_common;
    std::array<std::byte, 13> png_header{};
    std::array<std::byte, 4> gif_background{};
};

std::uint64_t frameTableBytes(const Index& index) {
    return index.info.document.frames.capacity() * sizeof(FrameInfo) +
           index.frames.capacity() * sizeof(IndexedFrame);
}

Result<void> reserveFrame(Index& index, const DecodeLimits& limits,
                          std::uint64_t otherMetadata = 0) {
    const auto count = index.frames.size() + 1U;
    const auto current = std::max(index.frames.capacity(), index.info.document.frames.capacity());
    if (count <= current)
        return {};
    if (otherMetadata > limits.maximum_metadata_bytes)
        return limited("Animation index exceeds the metadata limit.");
    const auto maximum = (limits.maximum_metadata_bytes - otherMetadata) /
                         (sizeof(FrameInfo) + sizeof(IndexedFrame));
    if (count > maximum)
        return limited("Animation index exceeds the metadata limit.");
    const auto capacity =
        std::min<std::uint64_t>(std::min<std::uint64_t>(limits.maximum_frames, maximum),
                                std::max<std::uint64_t>(count, current == 0 ? 8U : current * 2U));
    index.info.document.frames.reserve(static_cast<std::size_t>(capacity));
    index.frames.reserve(static_cast<std::size_t>(capacity));
    return {};
}

Result<void> validateFrame(const FrameInfo& frame, const DocumentInfo& document,
                           const DecodeLimits& limits) {
    if (frame.width == 0 || frame.height == 0 || frame.width > document.canvas_width ||
        frame.height > document.canvas_height || frame.x > document.canvas_width - frame.width ||
        frame.y > document.canvas_height - frame.height)
        return invalid("Animation frame rectangle is outside its canvas.");
    if ((document.frames.size() + 1U) * (sizeof(FrameInfo) + sizeof(IndexedFrame)) >
        limits.maximum_metadata_bytes)
        return limited("Animation index exceeds the metadata limit.");
    if (document.frames.size() >= limits.maximum_frames)
        return limited("Animation frame count exceeds the decode limit.");
    return {};
}

[[maybe_unused]] Result<Index> indexGif(std::span<const std::byte> bytes,
                                        const DecodeOptions& options, std::stop_token stop) {
    if (bytes.size() < 13)
        return invalid("GIF header is truncated.");
    Index result;
    auto& document = result.info.document;
    document.format = Format::gif;
    document.canvas_width = le16(bytes.subspan(6, 2));
    document.canvas_height = le16(bytes.subspan(8, 2));
    auto valid = internal::validate_dimensions(document.canvas_width, document.canvas_height,
                                               options.limits);
    if (!valid)
        return valid.error();
    std::size_t position = 13;
    const unsigned packed = std::to_integer<unsigned>(bytes[10]);
    if (packed & 128U) {
        const auto paletteBytes = std::size_t{3} << ((packed & 7U) + 1U);
        if (paletteBytes > bytes.size() - position)
            return invalid("GIF global palette is truncated.");
        const auto background = std::to_integer<unsigned>(bytes[11]) * 3U;
        if (background + 3U <= paletteBytes) {
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(position + background), 3,
                        result.gif_background.begin());
            result.gif_background[3] = std::byte{255};
        }
        position += paletteBytes;
    }
    std::chrono::nanoseconds delay{0};
    FrameDisposal disposal = FrameDisposal::keep;
    bool transparent = false;
    std::uint64_t metadataBytes = 0;
    bool loopSeen = false;
    bool ended = false;
    while (position < bytes.size()) {
        if (cancellationRequested(stop))
            return internal::cancelled_status();
        const unsigned tag = std::to_integer<unsigned>(bytes[position++]);
        if (tag == 0x3bU) {
            ended = true;
            break;
        }
        if (tag == 0x21U) {
            if (position >= bytes.size())
                return invalid("GIF extension is truncated.");
            const unsigned label = std::to_integer<unsigned>(bytes[position++]);
            bool first = true;
            bool loopApplication = false;
            for (;;) {
                if (position >= bytes.size())
                    return invalid("GIF extension block is truncated.");
                const auto length = std::to_integer<unsigned>(bytes[position++]);
                if (length == 0)
                    break;
                if (length > bytes.size() - position)
                    return invalid("GIF extension data is truncated.");
                const auto block = bytes.subspan(position, length);
                if (label == 0xf9U && !first)
                    return invalid("GIF graphic control has extra data blocks.");
                metadataBytes += length;
                if (metadataBytes +
                        document.frames.size() * (sizeof(FrameInfo) + sizeof(IndexedFrame)) >
                    options.limits.maximum_metadata_bytes)
                    return limited("GIF extension data exceeds the metadata limit.");
                if (first && label == 0xf9U) {
                    if (length != 4)
                        return invalid("GIF graphic control extension has an invalid length.");
                    const auto control = std::to_integer<unsigned>(block[0]);
                    transparent = (control & 1U) != 0;
                    const auto mode = (control >> 2U) & 7U;
                    disposal = mode == 2U   ? FrameDisposal::background
                               : mode == 3U ? FrameDisposal::previous
                                            : FrameDisposal::keep;
                    delay = std::chrono::milliseconds(le16(block.subspan(1, 2)) * 10U);
                } else if (first && label == 0xffU && length == 11) {
                    loopApplication = std::memcmp(block.data(), "NETSCAPE2.0", 11) == 0 ||
                                      std::memcmp(block.data(), "ANIMEXTS1.0", 11) == 0;
                } else if (!first && loopApplication && length >= 3 && block[0] == std::byte{1} &&
                           !loopSeen) {
                    const auto repeats = le16(block.subspan(1, 2));
                    result.info.total_play_count = repeats == 0 ? 0 : std::uint64_t{repeats} + 1U;
                    loopSeen = true;
                }
                first = false;
                position += length;
            }
            // Plain text consumes the pending graphic control extension.
            if (label == 1U) {
                delay = {};
                transparent = false;
                disposal = FrameDisposal::keep;
            }
            continue;
        }
        if (tag != 0x2cU || bytes.size() - position < 9)
            return invalid("GIF image descriptor is invalid or truncated.");
        FrameInfo frame;
        frame.x = le16(bytes.subspan(position, 2));
        frame.y = le16(bytes.subspan(position + 2, 2));
        frame.width = le16(bytes.subspan(position + 4, 2));
        frame.height = le16(bytes.subspan(position + 6, 2));
        frame.duration = delay;
        frame.disposal = disposal;
        frame.blend = transparent ? FrameBlend::over : FrameBlend::source;
        frame.native_format = kPremultipliedRgba8;
        frame.has_alpha = transparent;
        valid = validateFrame(frame, document, options.limits);
        if (!valid)
            return valid.error();
        const auto framePacked = std::to_integer<unsigned>(bytes[position + 8]);
        position += 9;
        if (framePacked & 128U) {
            const auto paletteBytes = std::size_t{3} << ((framePacked & 7U) + 1U);
            if (paletteBytes > bytes.size() - position)
                return invalid("GIF local palette is truncated.");
            position += paletteBytes;
        } else if (!(packed & 128U)) {
            return invalid("GIF frame has no color palette.");
        }
        if (position >= bytes.size())
            return invalid("GIF image data is truncated.");
        const auto codeSize = std::to_integer<unsigned>(bytes[position++]);
        if (codeSize < 2 || codeSize > 8)
            return invalid("GIF LZW code size is invalid.");
        bool dataSeen = false;
        for (;;) {
            if (position >= bytes.size())
                return invalid("GIF compressed image block is truncated.");
            const auto length = std::to_integer<unsigned>(bytes[position++]);
            if (length == 0)
                break;
            if (length > bytes.size() - position)
                return invalid("GIF compressed image data is truncated.");
            position += length;
            dataSeen = true;
        }
        if (!dataSeen)
            return invalid("GIF frame has no compressed image data.");
        if (metadataBytes + frameTableBytes(result) > options.limits.maximum_metadata_bytes)
            return limited("GIF index exceeds the metadata limit.");
        valid = reserveFrame(result, options.limits, metadataBytes);
        if (!valid)
            return valid.error();
        document.frames.push_back(frame);
        result.frames.push_back({frame, {}, transparent});
        delay = {};
        transparent = false;
        disposal = FrameDisposal::keep;
    }
    if (!ended || document.frames.empty())
        return invalid("GIF has no complete animation sequence.");
    document.loop_count = static_cast<std::uint32_t>(result.info.total_play_count);
    return result;
}

[[maybe_unused]] Result<Index> indexPng(std::span<const std::byte> bytes,
                                        const DecodeOptions& options, std::stop_token stop) {
    if (bytes.size() < 33 || be32(bytes.subspan(8, 4)) != 13 ||
        std::memcmp(bytes.data() + 12, "IHDR", 4) != 0)
        return invalid("PNG header is invalid.");
    Index result;
    auto& document = result.info.document;
    document.format = Format::png;
    document.canvas_width = be32(bytes.subspan(16, 4));
    document.canvas_height = be32(bytes.subspan(20, 4));
    auto valid = internal::validate_dimensions(document.canvas_width, document.canvas_height,
                                               options.limits);
    if (!valid)
        return valid.error();
    std::copy_n(bytes.begin() + 16, 13, result.png_header.begin());
    std::uint32_t declaredFrames = 0;
    std::uint64_t expectedSequence = 0;
    std::uint64_t metadataBytes = 0;
    bool defaultDataEnded = false;
    bool defaultDataSeen = false;
    bool defaultIsFrame = false;
    bool ended = false;
    bool animationSeen = false;
    static constexpr std::array<std::byte, 4> idat{std::byte{'I'}, std::byte{'D'}, std::byte{'A'},
                                                   std::byte{'T'}};
    for (std::size_t position = 8; position < bytes.size();) {
        if (cancellationRequested(stop))
            return internal::cancelled_status();
        if (bytes.size() - position < 12)
            return invalid("PNG chunk header is truncated.");
        const auto length = static_cast<std::size_t>(be32(bytes.subspan(position, 4)));
        if (length > bytes.size() - position - 12)
            return invalid("PNG chunk data is truncated.");
        // Charge index capacity and the transient virtual PNG stream nodes,
        // including their small generated IDAT headers and CRC allocations.
        metadataBytes += 256U;
        if (metadataBytes + frameTableBytes(result) > options.limits.maximum_metadata_bytes)
            return limited("PNG chunk index exceeds the metadata limit.");
        const auto type = bytes.subspan(position + 4, 4);
        const auto data = bytes.subspan(position + 8, length);
        if (defaultDataSeen && std::memcmp(type.data(), "IDAT", 4) != 0)
            defaultDataEnded = true;
        const auto is = [&](const char* name) { return std::memcmp(type.data(), name, 4) == 0; };
        if ((crcUpdate(0xffffffffU, bytes.subspan(position + 4, length + 4)) ^ 0xffffffffU) !=
            be32(bytes.subspan(position + length + 8, 4)))
            return invalid("PNG chunk checksum is invalid.");
        if (is("IHDR")) {
            if (position != 8)
                return invalid("PNG has duplicate image headers.");
        } else if (is("acTL")) {
            if (animationSeen || defaultDataSeen || length != 8)
                return invalid("APNG animation control is invalid.");
            animationSeen = true;
            declaredFrames = be32(data.first(4));
            if (declaredFrames == 0 || declaredFrames > options.limits.maximum_frames)
                return limited("APNG frame count exceeds the decode limit.");
            result.info.total_play_count = be32(data.subspan(4, 4));
            if (result.info.total_play_count > 0x7fffffffU)
                return invalid("APNG play count is outside the PNG integer range.");
        } else if (is("fcTL")) {
            if (!animationSeen || result.frames.size() >= declaredFrames || length != 26 ||
                be32(data.first(4)) != expectedSequence++ ||
                (!result.frames.empty() && !hasCompressedData(result.frames.back())))
                return invalid("APNG frame control sequence is invalid.");
            const auto dispose = std::to_integer<unsigned>(data[24]);
            const auto blend = std::to_integer<unsigned>(data[25]);
            if (dispose > 2 || blend > 1)
                return invalid("APNG frame composition mode is invalid.");
            FrameInfo frame;
            frame.width = be32(data.subspan(4, 4));
            frame.height = be32(data.subspan(8, 4));
            frame.x = be32(data.subspan(12, 4));
            frame.y = be32(data.subspan(16, 4));
            const auto denominator = be16(data.subspan(22, 2));
            frame.duration = std::chrono::nanoseconds(std::uint64_t{be16(data.subspan(20, 2))} *
                                                      1'000'000'000ULL /
                                                      (denominator == 0 ? 100U : denominator));
            frame.blend = blend == 0 ? FrameBlend::source : FrameBlend::over;
            frame.disposal = dispose == 0   ? FrameDisposal::keep
                             : dispose == 1 ? FrameDisposal::background
                                            : FrameDisposal::previous;
            if (document.frames.empty() && frame.disposal == FrameDisposal::previous)
                frame.disposal = FrameDisposal::background;
            frame.native_format = kPremultipliedRgba8;
            frame.has_alpha = true;
            valid = validateFrame(frame, document, options.limits);
            if (!valid)
                return valid.error();
            if (!defaultDataSeen) {
                if (frame.x != 0 || frame.y != 0 || frame.width != document.canvas_width ||
                    frame.height != document.canvas_height)
                    return invalid("APNG default animation frame must cover its canvas.");
                defaultIsFrame = true;
            }
            valid = reserveFrame(result, options.limits, metadataBytes);
            if (!valid)
                return valid.error();
            document.frames.push_back(frame);
            result.frames.push_back({frame, {}, false});
        } else if (is("IDAT")) {
            if (defaultDataEnded)
                return invalid("PNG image data chunks are not consecutive.");
            defaultDataSeen = true;
            if (defaultIsFrame) {
                if (result.frames.size() != 1)
                    return invalid("APNG default image data appears after later frames.");
                result.frames.front().data.push_back(
                    {position + 8, length, be32(bytes.subspan(position + length + 8, 4))});
            }
        } else if (is("fdAT")) {
            if (!animationSeen || !defaultDataSeen || result.frames.empty() || length < 4 ||
                be32(data.first(4)) != expectedSequence++ ||
                (defaultIsFrame && result.frames.size() == 1))
                return invalid("APNG frame data sequence is invalid.");
            result.frames.back().data.push_back(
                {position + 12, length - 4,
                 crcUpdate(crcUpdate(0xffffffffU, idat), data.subspan(4)) ^ 0xffffffffU});
        } else if (is("IEND")) {
            if (length != 0)
                return invalid("PNG end chunk is invalid.");
            ended = true;
            break;
        } else {
            if ((std::to_integer<unsigned>(type[0]) & 32U) == 0 && !is("PLTE"))
                return invalid("PNG has an unknown critical chunk.");
            if (defaultDataSeen && (is("PLTE") || is("tRNS") || is("gAMA") || is("cHRM") ||
                                    is("iCCP") || is("sRGB") || is("sBIT")))
                return invalid("PNG pixel declarations appear after image data.");
            metadataBytes += length;
            if (metadataBytes + frameTableBytes(result) > options.limits.maximum_metadata_bytes)
                return limited("PNG metadata exceeds the decode limit.");
            // A frame keeps pre-image palette, transparency and color declarations.
            if (!defaultDataSeen || is("eXIf") || is("tEXt") || is("iTXt") || is("zTXt") ||
                is("tIME"))
                result.png_common.push_back({position, length + 12, 0});
        }
        position += length + 12;
    }
    if (!animationSeen)
        return Status::error(ErrorCode::unsupported_feature, "PNG has no animation.", "animation");
    if (!ended || !defaultDataSeen || result.frames.size() != declaredFrames ||
        !hasCompressedData(result.frames.back()))
        return invalid("APNG animation frame count or image data is incomplete.");
    document.loop_count = static_cast<std::uint32_t>(result.info.total_play_count);
    return result;
}

#ifdef SNOW_IMAGE_HAS_WEBP
Result<Index> indexWebp(std::span<const std::byte> bytes, const DecodeOptions& options,
                        std::stop_token stop) {
    WebPData data{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
    std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)> demux(WebPDemux(&data),
                                                                   &WebPDemuxDelete);
    if (!demux)
        return invalid("WebP animation container is invalid.");
    if (!(WebPDemuxGetI(demux.get(), WEBP_FF_FORMAT_FLAGS) & ANIMATION_FLAG))
        return Status::error(ErrorCode::unsupported_feature, "WebP has no animation.", "animation");
    Index result;
    auto& document = result.info.document;
    document.format = Format::webp;
    document.canvas_width = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_WIDTH);
    document.canvas_height = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_HEIGHT);
    document.loop_count = WebPDemuxGetI(demux.get(), WEBP_FF_LOOP_COUNT);
    result.info.total_play_count = document.loop_count;
    auto valid = internal::validate_dimensions(document.canvas_width, document.canvas_height,
                                               options.limits);
    if (!valid)
        return valid.error();
    const auto count = WebPDemuxGetI(demux.get(), WEBP_FF_FRAME_COUNT);
    if (count == 0 || count > options.limits.maximum_frames)
        return limited("WebP frame count exceeds the decode limit.");
    WebPIterator iterator{};
    if (!WebPDemuxGetFrame(demux.get(), 1, &iterator))
        return invalid("WebP animation has no readable frames.");
    struct Guard {
        WebPIterator* iterator;
        ~Guard() {
            WebPDemuxReleaseIterator(iterator);
        }
    } guard{&iterator};
    std::int64_t timelineMilliseconds = 0;
    do {
        if (cancellationRequested(stop))
            return internal::cancelled_status();
        if (!iterator.complete || iterator.duration < 0 || iterator.width <= 0 ||
            iterator.height <= 0 || iterator.x_offset < 0 || iterator.y_offset < 0)
            return invalid("WebP animation frame is incomplete.");
        timelineMilliseconds += iterator.duration;
        if (timelineMilliseconds > std::numeric_limits<int>::max())
            return limited("WebP animation timeline exceeds the signed millisecond limit.");
        FrameInfo frame;
        frame.width = static_cast<std::uint32_t>(iterator.width);
        frame.height = static_cast<std::uint32_t>(iterator.height);
        frame.x = static_cast<std::uint32_t>(iterator.x_offset);
        frame.y = static_cast<std::uint32_t>(iterator.y_offset);
        frame.duration = std::chrono::milliseconds(iterator.duration);
        frame.native_format = kPremultipliedRgba8;
        frame.has_alpha = true;
        valid = validateFrame(frame, document, options.limits);
        if (!valid)
            return valid.error();
        valid = reserveFrame(result, options.limits);
        if (!valid)
            return valid.error();
        document.frames.push_back(frame);
        result.frames.push_back({frame, {}, false});
    } while (WebPDemuxNextFrame(&iterator));
    if (document.frames.size() != count)
        return invalid("WebP animation frame metadata is incomplete.");
    return result;
}
#endif

// The ordinary PNG decoder reads a virtual stream of small generated headers
// and immutable slices of the original compressed frame. No poster inflation,
// compressed-frame copy, or APNG-only libpng symbol is required.
class FramePngSource final : public ByteSource {
  public:
    FramePngSource(std::span<const std::byte> original, const Index& index,
                   const IndexedFrame& frame, bool includeMetadata = false)
        : original_(original) {
        // Each data span produces a header, a borrowed payload and a CRC.
        // Reserve once so vector growth cannot exceed the indexed quota.
        pieces_.reserve(2U + index.png_common.size() + frame.data.size() * 3U);
        std::vector<std::byte> header(original.begin(), original.begin() + 8);
        auto ihdr = index.png_header;
        for (unsigned byte = 0; byte < 4; ++byte) {
            ihdr[byte] = static_cast<std::byte>((frame.info.width >> (24U - byte * 8U)) & 255U);
            ihdr[byte + 4] =
                static_cast<std::byte>((frame.info.height >> (24U - byte * 8U)) & 255U);
        }
        appendChunk(header, "IHDR", ihdr);
        addOwned(std::move(header));
        for (const auto& common : index.png_common) {
            const auto* type = original.data() + common.offset + 4;
            if (includeMetadata || std::memcmp(type, "PLTE", 4) == 0 ||
                std::memcmp(type, "tRNS", 4) == 0)
                addBorrowed(common.offset, common.size);
        }
        for (const auto& data : frame.data) {
            std::vector<std::byte> chunk;
            append32(chunk, static_cast<std::uint32_t>(data.size));
            for (const char character : {'I', 'D', 'A', 'T'})
                chunk.push_back(static_cast<std::byte>(character));
            addOwned(std::move(chunk));
            addBorrowed(data.offset, data.size);
            std::vector<std::byte> crc;
            append32(crc, data.png_crc);
            addOwned(std::move(crc));
        }
        std::vector<std::byte> tail;
        appendChunk(tail, "IEND", {});
        addOwned(std::move(tail));
    }

    Result<std::uint64_t> size() const override {
        return size_;
    }
    Result<std::size_t> read_at(std::uint64_t offset,
                                std::span<std::byte> destination) const override {
        if (cancellationRequested())
            return internal::cancelled_status();
        std::size_t copied = 0;
        auto found =
            std::upper_bound(pieces_.begin(), pieces_.end(), offset,
                             [](std::uint64_t at, const Piece& piece) { return at < piece.begin; });
        if (found != pieces_.begin())
            --found;
        for (; found != pieces_.end() && copied < destination.size(); ++found) {
            const auto bytes = found->owned.empty()
                                   ? original_.subspan(found->offset, found->length)
                                   : std::span<const std::byte>(found->owned);
            if (offset < found->begin || offset >= found->begin + bytes.size())
                continue;
            const auto within = static_cast<std::size_t>(offset - found->begin);
            const auto count = std::min(bytes.size() - within, destination.size() - copied);
            std::memcpy(destination.data() + copied, bytes.data() + within, count);
            copied += count;
            offset += count;
        }
        return copied;
    }

  private:
    struct Piece {
        std::uint64_t begin;
        std::size_t offset;
        std::size_t length;
        std::vector<std::byte> owned;
    };
    void addOwned(std::vector<std::byte> bytes) {
        const auto length = bytes.size();
        pieces_.push_back({size_, 0, length, std::move(bytes)});
        size_ += length;
    }
    void addBorrowed(std::size_t offset, std::size_t length) {
        if (length == 0)
            return;
        pieces_.push_back({size_, offset, length, {}});
        size_ += length;
    }
    std::span<const std::byte> original_;
    std::vector<Piece> pieces_;
    std::uint64_t size_ = 0;
};

unsigned premultiply(unsigned component, unsigned alpha) {
    return (component * alpha + 127U) / 255U;
}

void composeRow(std::span<std::byte> canvas, std::uint32_t canvasWidth, const FrameInfo& frame,
                std::uint32_t row, std::span<const std::byte> pixels) {
    auto* destination =
        canvas.data() + (static_cast<std::size_t>(frame.y + row) * canvasWidth + frame.x) * 4U;
    for (std::uint32_t x = 0; x < frame.width; ++x) {
        const auto* source = pixels.data() + x * 4U;
        const auto alpha = std::to_integer<unsigned>(source[3]);
        if (alpha == 255U) {
            std::memcpy(destination, source, 4);
            destination += 4;
            continue;
        }
        if (alpha == 0U) {
            if (frame.blend == FrameBlend::source)
                std::fill_n(destination, 4, std::byte{});
            destination += 4;
            continue;
        }
        const auto inverse = 255U - alpha;
        for (unsigned channel = 0; channel < 3; ++channel) {
            unsigned value = premultiply(std::to_integer<unsigned>(source[channel]), alpha);
            if (frame.blend == FrameBlend::over)
                value += premultiply(std::to_integer<unsigned>(destination[channel]), inverse);
            destination[channel] = static_cast<std::byte>(std::min(value, 255U));
        }
        destination[3] = static_cast<std::byte>(
            frame.blend == FrameBlend::source
                ? alpha
                : std::min(255U, alpha + premultiply(std::to_integer<unsigned>(destination[3]),
                                                     inverse)));
        destination += 4;
    }
}

class CompositionSink final : public PixelSink {
  public:
    CompositionSink(std::span<std::byte> canvas, std::uint32_t width, const FrameInfo& frame)
        : canvas_(canvas), width_(width), frame_(frame) {}
    Result<void> begin(const DocumentInfo&) override {
        return {};
    }
    Result<void> begin_frame(std::uint32_t, const FrameInfo& frame) override {
        if (frame.width != frame_.width || frame.height != frame_.height ||
            frame.native_format != kRgba8)
            return invalid("APNG patch decoder produced an unexpected layout.");
        return {};
    }
    Result<void> write_rows(std::uint32_t first, std::uint32_t count, std::size_t stride,
                            std::span<const std::byte> pixels) override {
        if (cancellationRequested())
            return internal::cancelled_status();
        if (first > frame_.height || count > frame_.height - first ||
            stride < static_cast<std::size_t>(frame_.width) * 4U || count == 0 ||
            pixels.size() < (count - 1U) * stride + frame_.width * 4U)
            return invalid("APNG patch decoder produced invalid rows.");
        for (std::uint32_t row = 0; row < count; ++row) {
            if (cancellationRequested())
                return internal::cancelled_status();
            composeRow(canvas_, width_, frame_, first + row,
                       pixels.subspan(static_cast<std::size_t>(row) * stride, frame_.width * 4U));
        }
        return {};
    }
    Result<void> end_frame(std::uint32_t) override {
        return {};
    }
    Result<void> end() override {
        return {};
    }

  private:
    std::span<std::byte> canvas_;
    std::uint32_t width_;
    const FrameInfo& frame_;
};

} // namespace

class AnimationDecodeSession::Impl final {
  public:
    Impl(InputBytes bytes, DecodeOptions options, Index index)
        : bytes(std::move(bytes)), options(options), index(std::move(index)) {}
    ~Impl() {
#ifdef SNOW_IMAGE_HAS_GIF
        closeGif();
#endif
#ifdef SNOW_IMAGE_HAS_WEBP
        WebPAnimDecoderDelete(webp);
#endif
    }

    Result<void> initialize(std::stop_token stop) {
        if (cancellationRequested(stop))
            return internal::cancelled_status();
        const auto& document = index.info.document;
        canvasBytes =
            static_cast<std::uint64_t>(document.canvas_width) * document.canvas_height * 4U;
        if (canvasBytes > options.limits.maximum_owned_output_bytes ||
            canvasBytes > std::numeric_limits<std::size_t>::max())
            return limited("Animation canvas exceeds the published image limit.");
        std::uint64_t largestBackup = 0;
        std::uint64_t interlaceScratch = 0;
        for (const auto& frame : index.frames) {
            const auto patch = std::uint64_t{frame.info.width} * frame.info.height * 4U;
            if (frame.info.disposal == FrameDisposal::previous)
                largestBackup = std::max(largestBackup, patch);
            if (document.format == Format::png && index.png_header[12] != std::byte{0})
                interlaceScratch = std::max(interlaceScratch, patch);
        }
        const auto working = document.format == Format::webp
                                 ? canvasBytes * 3U
                                 : canvasBytes + largestBackup + interlaceScratch +
                                       std::uint64_t{document.canvas_width} * 5U;
        if (working > options.limits.maximum_working_bytes)
            return limited("Animation decoder exceeds the working-memory limit.");
        if (document.format != Format::webp) {
            canvas.resize(static_cast<std::size_t>(canvasBytes));
            // Reserve the maximum once so vector growth never exceeds the
            // admitted working set while changing patch dimensions.
            backup.reserve(static_cast<std::size_t>(largestBackup));
        }
#ifdef SNOW_IMAGE_HAS_GIF
        if (document.format == Format::gif) {
            gifRow.reserve(document.canvas_width);
        }
#endif
#ifdef SNOW_IMAGE_HAS_WEBP
        if (document.format == Format::webp) {
            WebPAnimDecoderOptions configuration;
            if (!WebPAnimDecoderOptionsInit(&configuration))
                return invalid("WebP animation decoder ABI is incompatible.");
            configuration.color_mode = MODE_rgbA;
            // A bounded outer decode pool supplies concurrency across pins.
            configuration.use_threads = 0;
            const WebPData data{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
            webp = WebPAnimDecoderNew(&data, &configuration);
            if (!webp)
                return invalid("Could not open the WebP animation decoder.");
        }
#endif
        return reset(stop);
    }

    Result<void> reset(std::stop_token stop) {
        if (cancellationRequested(stop))
            return internal::cancelled_status();
        position = -1;
        failed = false;
        backup.clear();
        const auto& first = index.frames.front().info;
        const bool replacesCanvas =
            first.x == 0 && first.y == 0 && first.width == index.info.document.canvas_width &&
            first.height == index.info.document.canvas_height &&
            first.blend == FrameBlend::source && first.disposal != FrameDisposal::previous;
        if (!replacesCanvas)
            std::fill(canvas.begin(), canvas.end(), std::byte{});
        if (!replacesCanvas && index.info.document.format == Format::gif &&
            !index.frames.front().gif_transparent) {
            for (std::size_t pixel = 0; pixel < canvas.size(); pixel += 4)
                std::copy(index.gif_background.begin(), index.gif_background.end(),
                          canvas.begin() + static_cast<std::ptrdiff_t>(pixel));
        }
#ifdef SNOW_IMAGE_HAS_GIF
        if (index.info.document.format == Format::gif) {
            closeGif();
            gifPosition = 0;
            gifStop = stop;
            int error = 0;
            gif = DGifOpen(this, gifRead, &error);
            if (!gif)
                return invalid("Could not reopen the GIF animation decoder.");
        }
#endif
#ifdef SNOW_IMAGE_HAS_WEBP
        if (webp) {
            WebPAnimDecoderReset(webp);
            webpPixels = nullptr;
        }
#endif
        return {};
    }

    void disposePrevious() {
        if (position < 0 || canvas.empty())
            return;
        const auto& previous = index.frames[static_cast<std::size_t>(position)];
        const auto& frame = previous.info;
        const auto stride = static_cast<std::size_t>(index.info.document.canvas_width) * 4U;
        if (frame.disposal == FrameDisposal::previous) {
            for (std::uint32_t row = 0; row < frame.height; ++row)
                std::memcpy(canvas.data() + static_cast<std::size_t>(frame.y + row) * stride +
                                frame.x * 4U,
                            backup.data() + static_cast<std::size_t>(row) * frame.width * 4U,
                            static_cast<std::size_t>(frame.width) * 4U);
        } else if (frame.disposal == FrameDisposal::background) {
            const auto background =
                index.info.document.format == Format::gif && !previous.gif_transparent
                    ? index.gif_background
                    : std::array<std::byte, 4>{};
            for (std::uint32_t row = 0; row < frame.height; ++row) {
                auto* destination =
                    canvas.data() + static_cast<std::size_t>(frame.y + row) * stride + frame.x * 4U;
                for (std::uint32_t x = 0; x < frame.width; ++x)
                    std::copy(background.begin(), background.end(), destination + x * 4U);
            }
        }
    }

    void savePrevious(const FrameInfo& frame) {
        if (frame.disposal != FrameDisposal::previous)
            return;
        backup.resize(static_cast<std::size_t>(frame.width) * frame.height * 4U);
        const auto stride = static_cast<std::size_t>(index.info.document.canvas_width) * 4U;
        for (std::uint32_t row = 0; row < frame.height; ++row)
            std::memcpy(backup.data() + static_cast<std::size_t>(row) * frame.width * 4U,
                        canvas.data() + static_cast<std::size_t>(frame.y + row) * stride +
                            frame.x * 4U,
                        static_cast<std::size_t>(frame.width) * 4U);
    }

    Result<void> decodePng(std::size_t next, std::stop_token stop) {
        const auto& frame = index.frames[next];
        auto source = std::make_shared<FramePngSource>(bytes.bytes(), index, frame);
        DecodeOptions patchOptions = options;
        patchOptions.orientation = OrientationPolicy::preserve;
        patchOptions.output_format = kRgba8;
        patchOptions.frame_index.reset();
        patchOptions.maximum_extent.reset();
        patchOptions.preserve_metadata = false;
        CompositionSink sink(canvas, index.info.document.canvas_width, frame.info);
        return pngService.decode_to_sink({std::move(source), "animation-frame.png"}, sink,
                                         patchOptions, stop);
    }

#ifdef SNOW_IMAGE_HAS_GIF
    static int gifRead(GifFileType* file, GifByteType* destination, int requested) {
        auto* self = static_cast<Impl*>(file->UserData);
        if (!self || requested < 0 || cancellationRequested(self->gifStop))
            return 0;
        const auto count =
            std::min(static_cast<std::size_t>(requested), self->bytes.size() - self->gifPosition);
        std::memcpy(destination, self->bytes.data() + self->gifPosition, count);
        self->gifPosition += count;
        return static_cast<int>(count);
    }
    void closeGif() {
        if (gif) {
            int error = 0;
            DGifCloseFile(gif, &error);
            gif = nullptr;
        }
    }
    Result<void> decodeGif(std::size_t next, std::stop_token stop) {
        gifStop = stop;
        int transparent = -1;
        for (;;) {
            if (cancellationRequested(stop))
                return internal::cancelled_status();
            GifRecordType record = UNDEFINED_RECORD_TYPE;
            if (DGifGetRecordType(gif, &record) == GIF_ERROR)
                return invalid("GIF record decoding failed.");
            if (record == IMAGE_DESC_RECORD_TYPE)
                break;
            if (record == TERMINATE_RECORD_TYPE)
                return invalid("GIF ended before its indexed frame.");
            if (record == EXTENSION_RECORD_TYPE) {
                int code = 0;
                GifByteType* block = nullptr;
                if (DGifGetExtension(gif, &code, &block) == GIF_ERROR)
                    return invalid("GIF extension decoding failed.");
                if (code == GRAPHICS_EXT_FUNC_CODE && block) {
                    GraphicsControlBlock control{};
                    if (DGifExtensionToGCB(block[0], block + 1, &control) == GIF_ERROR)
                        return invalid("GIF graphic control decoding failed.");
                    transparent = control.TransparentColor;
                } else if (code == PLAINTEXT_EXT_FUNC_CODE) {
                    transparent = -1;
                }
                while (block) {
                    if (DGifGetExtensionNext(gif, &block) == GIF_ERROR)
                        return invalid("GIF extension block decoding failed.");
                }
            }
        }
        if (DGifGetImageDesc(gif) == GIF_ERROR)
            return invalid("GIF image descriptor decoding failed.");
        const auto& frame = index.frames[next].info;
        if (gif->Image.Width != static_cast<int>(frame.width) ||
            gif->Image.Height != static_cast<int>(frame.height) ||
            gif->Image.Left != static_cast<int>(frame.x) ||
            gif->Image.Top != static_cast<int>(frame.y))
            return invalid("GIF frame changed after metadata indexing.");
        const auto* palette = gif->Image.ColorMap ? gif->Image.ColorMap : gif->SColorMap;
        if (!palette || !palette->Colors || palette->ColorCount <= 0 || palette->ColorCount > 256)
            return invalid("GIF palette is unavailable.");
        std::array<std::uint32_t, 256> packedPalette{};
        for (int color = 0; color < palette->ColorCount; ++color) {
            const auto value = palette->Colors[color];
            packedPalette[static_cast<std::size_t>(color)] =
                std::bit_cast<std::uint32_t>(std::array<std::byte, 4>{
                    static_cast<std::byte>(value.Red), static_cast<std::byte>(value.Green),
                    static_cast<std::byte>(value.Blue), std::byte{255}});
        }
        gifRow.resize(frame.width);
        constexpr std::array starts{0, 4, 2, 1};
        constexpr std::array steps{8, 8, 4, 2};
        int pass = 0;
        std::uint32_t row = 0;
        for (std::uint32_t encoded = 0; encoded < frame.height; ++encoded) {
            if (cancellationRequested(stop))
                return internal::cancelled_status();
            if (DGifGetLine(gif, gifRow.data(), static_cast<int>(frame.width)) == GIF_ERROR)
                return invalid("GIF compressed image data is invalid.");
            auto* destination = canvas.data() + (static_cast<std::size_t>(frame.y + row) *
                                                     index.info.document.canvas_width +
                                                 frame.x) *
                                                    4U;
            for (std::uint32_t x = 0; x < frame.width; ++x) {
                const auto color = static_cast<int>(gifRow[x]);
                if (color >= palette->ColorCount)
                    return invalid("GIF palette index is out of range.");
                if (color != transparent) {
                    std::memcpy(destination + static_cast<std::size_t>(x) * 4U,
                                &packedPalette[static_cast<std::size_t>(color)],
                                sizeof(std::uint32_t));
                } else if (frame.blend == FrameBlend::source) {
                    std::fill_n(destination + static_cast<std::size_t>(x) * 4U, 4, std::byte{});
                }
            }
            if (!gif->Image.Interlace) {
                ++row;
            } else {
                row += static_cast<std::uint32_t>(steps[static_cast<std::size_t>(pass)]);
                while (row >= frame.height && pass < 3) {
                    ++pass;
                    row = static_cast<std::uint32_t>(starts[static_cast<std::size_t>(pass)]);
                }
            }
        }
        return {};
    }
#endif

    Result<bool> advance(std::stop_token stop) {
        if (failed)
            return Status::error(ErrorCode::invalid_argument,
                                 "Animation decoder must be reset after a failed advance.");
        if (cancellationRequested(stop))
            return internal::cancelled_status();
        const auto next = static_cast<std::size_t>(position + 1);
        if (next >= index.frames.size())
            return false;
        disposePrevious();
        savePrevious(index.frames[next].info);
        Result<void> decoded;
        switch (index.info.document.format) {
        case Format::png:
            decoded = decodePng(next, stop);
            break;
#ifdef SNOW_IMAGE_HAS_GIF
        case Format::gif:
            decoded = decodeGif(next, stop);
            break;
#endif
#ifdef SNOW_IMAGE_HAS_WEBP
        case Format::webp: {
            int timestamp = 0;
            std::uint8_t* pixels = nullptr;
            if (!WebPAnimDecoderGetNext(webp, &pixels, &timestamp) || !pixels)
                decoded = invalid("WebP animation frame decoding failed.");
            else
                webpPixels = pixels;
            break;
        }
#endif
        default:
            decoded = Status::error(ErrorCode::codec_unavailable,
                                    "Animation decoder is unavailable for this format.");
            break;
        }
        if (cancellationRequested(stop))
            decoded = internal::cancelled_status();
        if (!decoded) {
            failed = true;
            return decoded.error();
        }
        position = static_cast<std::int64_t>(next);
        return true;
    }

    Result<Image> snapshotPixels(PixelFormat format, std::stop_token stop) const {
        if (position < 0 || failed)
            return Status::error(ErrorCode::invalid_argument,
                                 "Animation decoder has no complete current frame.");
        if (format != kPremultipliedRgba8 && format != kPremultipliedBgra8)
            return Status::error(ErrorCode::unsupported_feature,
                                 "Animation snapshots require premultiplied RGBA8 or BGRA8.");
        if (cancellationRequested(stop))
            return internal::cancelled_status();
        const std::byte* pixels = canvas.data();
#ifdef SNOW_IMAGE_HAS_WEBP
        if (webp)
            pixels = reinterpret_cast<const std::byte*>(webpPixels);
#endif
        const auto& document = index.info.document;
        auto copy = MutableImage::allocate(document.canvas_width, document.canvas_height, format);
        if (!copy)
            return copy.error();
        const auto rowBytes = static_cast<std::size_t>(document.canvas_width) * 4U;
        for (std::uint32_t row = 0; row < document.canvas_height; ++row) {
            if (cancellationRequested(stop))
                return internal::cancelled_status();
            const auto* source = pixels + static_cast<std::size_t>(row) * rowBytes;
            auto* destination = copy.value().pixels().data() +
                                static_cast<std::size_t>(row) * copy.value().row_stride();
            if (format == kPremultipliedRgba8)
                std::memcpy(destination, source, rowBytes);
            else
                copyBgraRow(source, destination, document.canvas_width);
        }
        if (cancellationRequested(stop))
            return internal::cancelled_status();
        return std::move(copy).value().freeze();
    }

    Result<Frame> snapshot(std::stop_token stop) const {
        auto pixels = snapshotPixels(kPremultipliedRgba8, stop);
        if (!pixels)
            return pixels.error();
        const auto& document = index.info.document;
        Frame frame;
        frame.image = std::move(pixels).value();
        frame.duration = document.frames[static_cast<std::size_t>(position)].duration;
        frame.color = document.color;
        frame.metadata = document.metadata;
        return frame;
    }

    InputBytes bytes;
    DecodeOptions options;
    Index index;
    Service pngService;
    snow::memory::PixelArray<std::byte> canvas;
    snow::memory::PixelArray<std::byte> backup;
    std::uint64_t canvasBytes = 0;
    std::int64_t position = -1;
    bool failed = false;
#ifdef SNOW_IMAGE_HAS_GIF
    GifFileType* gif = nullptr;
    std::size_t gifPosition = 0;
    std::stop_token gifStop;
    snow::memory::PixelArray<GifByteType> gifRow;
#endif
#ifdef SNOW_IMAGE_HAS_WEBP
    WebPAnimDecoder* webp = nullptr;
    const std::uint8_t* webpPixels = nullptr;
#endif
};

AnimationDecodeSession::AnimationDecodeSession() = default;
AnimationDecodeSession::~AnimationDecodeSession() = default;
AnimationDecodeSession::AnimationDecodeSession(AnimationDecodeSession&&) noexcept = default;
AnimationDecodeSession&
AnimationDecodeSession::operator=(AnimationDecodeSession&&) noexcept = default;
AnimationDecodeSession::AnimationDecodeSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

Result<AnimationDecodeSession>
AnimationDecodeSession::open(const Input& input, const DecodeOptions& options, std::stop_token stop,
                             const AnimationCancellationCheck& cancelled,
                             const AnimationAdmissionCheck& admit) {
    CancellationScope cancellation(cancelled, stop);
    try {
        if (cancellationRequested(stop))
            return internal::cancelled_status();
        if (!input.source)
            return Status::error(ErrorCode::invalid_argument, "Animation input is empty.");
        if (options.orientation != OrientationPolicy::preserve || options.frame_index ||
            options.maximum_extent ||
            (options.output_format && *options.output_format != kPremultipliedRgba8))
            return Status::error(
                ErrorCode::unsupported_feature,
                "Animation sessions require untransformed premultiplied RGBA8 output.");
        auto bytes = read_contiguous(input, options.limits.maximum_input_bytes);
        if (!bytes)
            return bytes.error();
        const auto view = bytes.value().bytes();
        Result<Index> indexed =
            Status::error(ErrorCode::unsupported_format, "Input is not a supported animation.");
        if (view.size() >= 6 && (std::memcmp(view.data(), "GIF87a", 6) == 0 ||
                                 std::memcmp(view.data(), "GIF89a", 6) == 0)) {
#ifdef SNOW_IMAGE_HAS_GIF
            indexed = indexGif(view, options, stop);
#else
            return Status::error(ErrorCode::codec_unavailable, "GIF codec is unavailable.");
#endif
        } else if (view.size() >= 8 && std::memcmp(view.data(), "\x89PNG\r\n\x1a\n", 8) == 0) {
#ifdef SNOW_IMAGE_HAS_PNG
            indexed = indexPng(view, options, stop);
#else
            return Status::error(ErrorCode::codec_unavailable, "PNG codec is unavailable.");
#endif
        } else if (view.size() >= 12 && std::memcmp(view.data(), "RIFF", 4) == 0 &&
                   std::memcmp(view.data() + 8, "WEBP", 4) == 0) {
#ifdef SNOW_IMAGE_HAS_WEBP
            indexed = indexWebp(view, options, stop);
#else
            return Status::error(ErrorCode::codec_unavailable, "WebP codec is unavailable.");
#endif
        }
        if (!indexed)
            return indexed.error();
        auto impl =
            std::make_unique<Impl>(std::move(bytes).value(), options, std::move(indexed).value());
        // Static metadata/color parsing is bounded and never inflates animation pixels.
        if (impl->index.info.document.format != Format::gif) {
            DecodeOptions metadataOptions = options;
            metadataOptions.frame_index = 0;
            metadataOptions.orientation = OrientationPolicy::preserve;
            Input metadataInput = input;
            if (impl->index.info.document.format == Format::png)
                metadataInput = {std::make_shared<FramePngSource>(impl->bytes.bytes(), impl->index,
                                                                  impl->index.frames.front(), true),
                                 "animation-metadata.png"};
            auto metadata = impl->pngService.inspect(metadataInput, metadataOptions, stop);
            if (!metadata)
                return metadata.error();
            impl->index.info.document.color = std::move(metadata.value().color);
            impl->index.info.document.metadata = std::move(metadata.value().metadata);
        }
        if (admit && !admit(impl->index.info))
            return limited("Animation allocation was not admitted.");
        auto initialized = impl->initialize(stop);
        if (!initialized)
            return initialized.error();
        return AnimationDecodeSession(std::move(impl));
    } catch (const Cancelled&) {
        return internal::cancelled_status();
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Animation decoder ran out of memory.");
    } catch (...) {
        return Status::error(ErrorCode::internal_error, "Could not open animation decoder.");
    }
}

Result<AnimationDecodeSession>
Service::open_animation_decoder(const Input& input, const DecodeOptions& options,
                                std::stop_token stop, const AnimationCancellationCheck& cancelled,
                                const AnimationAdmissionCheck& admit) const {
    return AnimationDecodeSession::open(input, options, stop, cancelled, admit);
}

const AnimationInfo& AnimationDecodeSession::info() const noexcept {
    static const AnimationInfo empty;
    return impl_ ? impl_->index.info : empty;
}

std::int64_t AnimationDecodeSession::frame_index() const noexcept {
    return impl_ ? impl_->position : -1;
}

Result<bool> AnimationDecodeSession::advance(std::stop_token stop,
                                             const AnimationCancellationCheck& cancelled) {
    CancellationScope cancellation(cancelled, stop);
    if (!impl_)
        return Status::error(ErrorCode::invalid_argument, "Animation decoder is empty.");
    try {
        return impl_->advance(stop);
    } catch (const Cancelled&) {
        impl_->failed = true;
        return internal::cancelled_status();
    } catch (const std::bad_alloc&) {
        impl_->failed = true;
        return Status::error(ErrorCode::out_of_memory, "Animation advance ran out of memory.");
    } catch (...) {
        impl_->failed = true;
        return Status::error(ErrorCode::internal_error, "Animation advance failed.");
    }
}

Result<Frame> AnimationDecodeSession::snapshot(std::stop_token stop) const {
    if (!impl_)
        return Status::error(ErrorCode::invalid_argument, "Animation decoder is empty.");
    try {
        return impl_->snapshot(stop);
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Animation snapshot ran out of memory.");
    }
}

Result<Image> AnimationDecodeSession::snapshot_pixels(PixelFormat format,
                                                      std::stop_token stop) const {
    if (!impl_)
        return Status::error(ErrorCode::invalid_argument, "Animation decoder is empty.");
    try {
        return impl_->snapshotPixels(format, stop);
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Animation snapshot ran out of memory.");
    }
}

Result<void> AnimationDecodeSession::reset(std::stop_token stop) {
    if (!impl_)
        return Status::error(ErrorCode::invalid_argument, "Animation decoder is empty.");
    return impl_->reset(stop);
}

} // namespace snow::image
