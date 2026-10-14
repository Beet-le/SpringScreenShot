#include <snow/image/animation.h>
#include <snow/image/service.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
using namespace snow::image;
using Bytes = std::vector<std::byte>;
constexpr PixelFormat kPremultipliedRgba8{SampleType::unsigned_integer, ChannelLayout::rgba,
                                          AlphaMode::premultiplied, 8, true};
constexpr PixelFormat kPremultipliedBgra8{SampleType::unsigned_integer, ChannelLayout::bgra,
                                          AlphaMode::premultiplied, 8, true};
bool hasCodec(Format format) {
    const Service service;
    return std::any_of(
        service.formats().begin(), service.formats().end(),
        [=](const FormatCapability& capability) { return capability.format == format; });
}
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
Bytes base64(std::string_view text) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Bytes result;
    unsigned value = 0;
    int bits = -8;
    for (const char character : text) {
        const auto index = alphabet.find(character);
        if (index == std::string_view::npos)
            continue;
        value = (value << 6U) | static_cast<unsigned>(index);
        bits += 6;
        if (bits >= 0) {
            result.push_back(static_cast<std::byte>((value >> bits) & 255U));
            bits -= 8;
        }
    }
    return result;
}
Bytes gifFixture() {
    return base64("R0lGODlhAgACAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQACgAAACwAAAAAAgAC"
                  "AAAIBgABCAQQEAAh+QQBFAABACwAAAAAAgACAIEAgAAAAAAAAAAAAAAIBgABCAQQEAA7");
}
Bytes webpFixture() {
    return base64(
        "UklGRsYAAABXRUJQVlA4WAoAAAACAAAAAQAAAQAAQU5JTQYAAAAAAAAAAABBTk1GSgAAAAAAAAAAAAEA"
        "AAEAAGQAAAJWUDggMgAAADABAJ0BKgIAAgABQCYloAADcAD+8ut///mwP/bz/wR6Af//0uD//pcH//S4"
        "P/SkAAAAQU5NRkgAAAAAAAAAAAABAAABAADIAAAAVlA4IDAAAAA0AQCdASoCAAIAAAAmJaAAA3AA/vNpl"
        "//5sD/yD/5B9/v//00j/+mkf/00j5TQAAA=");
}
void append32(Bytes& bytes, std::uint32_t value) {
    for (const auto shift : {24U, 16U, 8U, 0U})
        bytes.push_back(static_cast<std::byte>((value >> shift) & 255U));
}
std::uint32_t be32(std::span<const std::byte> bytes) {
    std::uint32_t value = 0;
    for (const auto byte : bytes.first(4))
        value = (value << 8U) | std::to_integer<unsigned>(byte);
    return value;
}
std::uint32_t crc(std::span<const std::byte> bytes) {
    std::uint32_t result = 0xffffffffU;
    for (const auto byte : bytes) {
        result ^= std::to_integer<unsigned>(byte);
        for (int bit = 0; bit < 8; ++bit)
            result = (result >> 1U) ^ ((result & 1U) ? 0xedb88320U : 0U);
    }
    return result ^ 0xffffffffU;
}
void chunk(Bytes& bytes, const char* type, std::span<const std::byte> data) {
    append32(bytes, static_cast<std::uint32_t>(data.size()));
    const auto start = bytes.size();
    bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(type),
                 reinterpret_cast<const std::byte*>(type) + 4);
    bytes.insert(bytes.end(), data.begin(), data.end());
    append32(bytes, crc(std::span(bytes).subspan(start)));
}
Bytes png(std::uint32_t width, std::uint32_t height, std::array<unsigned, 4> color,
          bool interlaced = false) {
    // Sixteen-bit RGBA bypasses palette discovery. Keep opaque alpha at 65534
    // so every patch has the same color type; the display's 8-bit alpha is 255.
    auto image = MutableImage::allocate(width, height, kRgba16);
    require(bool(image), "allocate PNG fixture");
    for (std::size_t pixel = 0; pixel < image.value().pixels().size(); pixel += 8)
        for (std::size_t channel = 0; channel < 4; ++channel) {
            std::uint16_t value = static_cast<std::uint16_t>(color[channel] * 257U);
            if (channel == 3 && value == 65535)
                value = 65534;
            std::memcpy(image.value().pixels().data() + pixel + channel * 2U, &value, 2);
        }
    Document document;
    document.canvas_width = width;
    document.canvas_height = height;
    Frame frame;
    frame.image = std::move(image).value().freeze();
    document.frames.push_back(std::move(frame));
    EncodeOptions options;
    options.format = Format::png;
    options.interlaced = interlaced;
    auto bytes = std::make_shared<Bytes>();
    require(bool(Service{}.encode(document, memory_output(bytes), options)), "encode PNG fixture");
    return *bytes;
}
Frame solid(std::uint32_t width, std::uint32_t height, std::array<unsigned, 4> color) {
    auto image = MutableImage::allocate(width, height, kRgba8);
    require(bool(image), "allocate solid fixture");
    for (std::size_t pixel = 0; pixel < image.value().pixels().size(); pixel += 4)
        for (std::size_t channel = 0; channel < 4; ++channel)
            image.value().pixels()[pixel + channel] = static_cast<std::byte>(color[channel]);
    Frame frame;
    frame.image = std::move(image).value().freeze();
    frame.duration = std::chrono::milliseconds(50);
    return frame;
}
Bytes encode(const Document& document, Format format, bool interlaced = false) {
    EncodeOptions options;
    options.format = format;
    options.lossless = true;
    options.interlaced = interlaced;
    auto bytes = std::make_shared<Bytes>();
    require(bool(Service{}.encode(document, memory_output(bytes), options)), "encode fixture");
    return *bytes;
}
Bytes idat(const Bytes& pngBytes) {
    Bytes result;
    for (std::size_t offset = 8; offset + 12 <= pngBytes.size();) {
        const auto size = be32(std::span(pngBytes).subspan(offset));
        if (std::memcmp(pngBytes.data() + offset + 4, "IDAT", 4) == 0)
            result.insert(result.end(), pngBytes.begin() + static_cast<std::ptrdiff_t>(offset + 8),
                          pngBytes.begin() + static_cast<std::ptrdiff_t>(offset + 8 + size));
        offset += size + 12;
    }
    return result;
}
struct Patch final {
    Bytes encoded;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    unsigned disposal = 0;
    unsigned blend = 0;
};
Bytes apng(const std::vector<Patch>& patches, bool hiddenPoster = false, bool interlaced = false,
           std::uint32_t width = 2, std::uint32_t height = 2) {
    const auto poster = png(width, height, {0, 0, 255, 255}, interlaced);
    Bytes result(poster.begin(), poster.begin() + 33);
    Bytes control;
    append32(control, static_cast<std::uint32_t>(patches.size()));
    append32(control, 3);
    chunk(result, "acTL", control);
    if (hiddenPoster)
        chunk(result, "IDAT", idat(poster));
    std::uint32_t sequence = 0;
    for (std::size_t index = 0; index < patches.size(); ++index) {
        const auto& patch = patches[index];
        Bytes frame;
        append32(frame, sequence++);
        append32(frame, be32(std::span(patch.encoded).subspan(16)));
        append32(frame, be32(std::span(patch.encoded).subspan(20)));
        append32(frame, patch.x);
        append32(frame, patch.y);
        frame.insert(frame.end(),
                     {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                      static_cast<std::byte>(patch.disposal), static_cast<std::byte>(patch.blend)});
        chunk(result, "fcTL", frame);
        auto data = idat(patch.encoded);
        if (index == 0 && !hiddenPoster)
            chunk(result, "IDAT", data);
        else {
            Bytes framed;
            append32(framed, sequence++);
            framed.insert(framed.end(), data.begin(), data.end());
            chunk(result, "fdAT", framed);
        }
    }
    chunk(result, "IEND", {});
    return result;
}
std::array<unsigned, 4> pixel(const Image& image, unsigned x, unsigned y) {
    const auto offset = y * image.row_stride() + x * 4U;
    const auto pixels = image.pixels();
    return {std::to_integer<unsigned>(pixels[offset]),
            std::to_integer<unsigned>(pixels[offset + 1]),
            std::to_integer<unsigned>(pixels[offset + 2]),
            std::to_integer<unsigned>(pixels[offset + 3])};
}
std::array<unsigned, 4> pixel(const Frame& frame, unsigned x, unsigned y) {
    return pixel(frame.image, x, y);
}
void verifyPixelSnapshots(AnimationDecodeSession& session, const Image& native) {
    auto rgba = session.snapshot_pixels(kPremultipliedRgba8);
    auto bgra = session.snapshot_pixels(kPremultipliedBgra8);
    require(bool(rgba) && bool(bgra), "publish pixel-only snapshots in both formats");
    require(rgba.value().format() == kPremultipliedRgba8 &&
                bgra.value().format() == kPremultipliedBgra8 &&
                rgba.value().width() == native.width() &&
                rgba.value().height() == native.height() &&
                bgra.value().width() == native.width() && bgra.value().height() == native.height(),
            "pixel-only snapshot shape and format");
    require(rgba.value().pixels().data() != native.pixels().data() &&
                bgra.value().pixels().data() != native.pixels().data(),
            "pixel snapshots own independently immutable storage");
    for (std::uint32_t y = 0; y < native.height(); ++y) {
        for (std::uint32_t x = 0; x < native.width(); ++x) {
            const auto expected = pixel(native, x, y);
            const auto swapped = pixel(bgra.value(), x, y);
            require(pixel(rgba.value(), x, y) == expected &&
                        swapped == std::array<unsigned, 4>{expected[2], expected[1], expected[0],
                                                           expected[3]},
                    "pixel-only output preserves premultiplied channels and alpha");
        }
    }
}
Frame advance(AnimationDecodeSession& session) {
    auto next = session.advance();
    require(bool(next) && next.value(), "advance animation");
    auto frame = session.snapshot();
    require(bool(frame), "snapshot animation");
    verifyPixelSnapshots(session, frame.value().image);
    return std::move(frame).value();
}
void pixelSnapshotLifetime(const Bytes& encoded) {
    AnimationDecodeSession empty;
    const auto missing = empty.snapshot_pixels(kPremultipliedBgra8);
    require(!missing && missing.error().code == ErrorCode::invalid_argument,
            "empty decoder cannot publish pixels");
    auto opened = Service{}.open_animation_decoder(memory_input(encoded));
    require(bool(opened), "open pixel-only snapshot fixture");
    auto session = std::move(opened).value();
    const auto initial = session.snapshot_pixels(kPremultipliedRgba8);
    require(!initial && initial.error().code == ErrorCode::invalid_argument,
            "pixel-only publication requires a complete current frame");
    const auto first = advance(session);
    auto retained = session.snapshot_pixels(kPremultipliedBgra8);
    require(bool(retained), "retain independent BGRA snapshot");
    const auto retainedPixel = pixel(retained.value(), 0, 0);
    const auto wrongAlpha = session.snapshot_pixels(kRgba8);
    const auto wrongDepth = session.snapshot_pixels(kRgba16);
    require(!wrongAlpha && wrongAlpha.error().code == ErrorCode::unsupported_feature &&
                !wrongDepth && wrongDepth.error().code == ErrorCode::unsupported_feature,
            "pixel-only snapshots reject straight alpha and unsupported depth");
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = session.snapshot_pixels(kPremultipliedBgra8, stop.get_token());
    require(!cancelled && cancelled.error().code == ErrorCode::cancelled &&
                session.frame_index() == 0,
            "cancelled pixel publication preserves the complete decoder frame");
    verifyPixelSnapshots(session, first.image);
    static_cast<void>(advance(session));
    require(pixel(retained.value(), 0, 0) == retainedPixel,
            "pixel-only snapshots survive mutable canvas advancement");
    require(bool(session.reset()), "reset pixel-only snapshot fixture");
    session = AnimationDecodeSession{};
    require(pixel(retained.value(), 0, 0) == retainedPixel,
            "pixel-only snapshots outlive reset and decoder destruction");
}
void basic(const Bytes& encoded, Format format) {
    auto opened = Service{}.open_animation_decoder(memory_input(encoded));
    if (!opened)
        std::cerr << opened.error().message << '\n';
    require(bool(opened), "open animation");
    auto session = std::move(opened).value();
    require(session.frame_index() == -1, "open is before frame zero");
    require(session.info().document.format == format && session.info().document.frames.size() == 2,
            "index two frames");
    require(session.info().total_play_count == 0, "infinite loops preserved");
    require(session.info().document.frames[0].duration.count() == 100'000'000 &&
                session.info().document.frames[1].duration.count() == 200'000'000,
            "source timing preserved");
    const auto first = advance(session);
    const auto firstColor = pixel(first, 0, 0);
    const auto second = advance(session);
    require(pixel(first, 0, 0) == firstColor, "published frame survives advance");
    require(pixel(second, 0, 0) != firstColor, "second frame differs");
    const auto end = session.advance();
    require(bool(end) && !end.value() && session.frame_index() == 1,
            "end does not wrap implicitly");
    require(bool(session.reset()) && session.frame_index() == -1, "reset before frame zero");
    require(pixel(advance(session), 0, 0) == firstColor, "reset replay is deterministic");
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = session.advance(stop.get_token());
    require(!cancelled && cancelled.error().code == ErrorCode::cancelled, "stop token cancels");
    const auto dynamicCancel = session.advance({}, [] { return true; });
    require(!dynamicCancel && dynamicCancel.error().code == ErrorCode::cancelled,
            "dynamic worker cancellation");
    require(bool(session.reset()), "reset after cancellation");
    int checks = 0;
    const auto interrupted =
        session.advance({}, [&] { return ++checks >= (format == Format::webp ? 2 : 4); });
    require(!interrupted && interrupted.error().code == ErrorCode::cancelled,
            "in-progress row/decode cancellation");
    require(!session.snapshot(), "interrupted canvas cannot be published");
    require(!session.snapshot_pixels(kPremultipliedBgra8),
            "interrupted canvas cannot publish pixel-only snapshots");
    require(bool(session.reset()) && bool(session.advance()), "cancelled session replay recovery");
}
void pngComposition() {
    const auto encoded = apng({{png(2, 2, {255, 0, 0, 255})},
                               {png(1, 1, {0, 255, 0, 128}), 0, 0, 2, 1},
                               {png(1, 1, {0, 0, 255, 255}), 1, 0, 1, 0},
                               {png(1, 1, {255, 255, 255, 0}), 0, 1, 0, 0}});
    auto result = Service{}.open_animation_decoder(memory_input(encoded));
    if (!result)
        std::cerr << result.error().message << '\n';
    require(bool(result), "open partial APNG");
    auto session = std::move(result).value();
    require(session.info().total_play_count == 3, "APNG count means total plays");
    require(session.info().document.frames[0].duration.count() == 0, "raw zero delay retained");
    require(pixel(advance(session), 0, 0) == std::array<unsigned, 4>{255, 0, 0, 255},
            "source blend");
    require(pixel(advance(session), 0, 0) == std::array<unsigned, 4>{127, 128, 0, 255},
            "over blend");
    const auto third = advance(session);
    require(pixel(third, 0, 0) == std::array<unsigned, 4>{255, 0, 0, 255}, "previous disposal");
    require(pixel(third, 1, 0) == std::array<unsigned, 4>{0, 0, 255, 255},
            "partial source placement");
    const auto fourth = advance(session);
    require(pixel(fourth, 1, 0)[3] == 0, "background disposal clears alpha");
    require(pixel(fourth, 0, 1)[3] == 0, "transparent source replaces pixel");
    const auto hidden = apng({{png(1, 1, {255, 0, 0, 255})}}, true);
    auto hiddenResult = Service{}.open_animation_decoder(memory_input(hidden));
    require(bool(hiddenResult), "one frame APNG with hidden poster");
    auto hiddenSession = std::move(hiddenResult).value();
    const auto only = advance(hiddenSession);
    require(hiddenSession.info().document.frames.size() == 1, "poster excluded from sequence");
    require(pixel(only, 1, 1)[3] == 0 && pixel(only, 0, 0)[0] == 255, "poster not composited");
    const auto interlaced =
        apng({{png(2, 2, {255, 0, 0, 255}, true)}, {png(1, 1, {0, 255, 0, 128}, true), 0, 0, 0, 1}},
             false, true);
    auto interlaceResult = Service{}.open_animation_decoder(memory_input(interlaced));
    require(bool(interlaceResult), "open Adam7 APNG");
    auto interlaceSession = std::move(interlaceResult).value();
    static_cast<void>(advance(interlaceSession));
    require(pixel(advance(interlaceSession), 0, 0) == std::array<unsigned, 4>{127, 128, 0, 255},
            "Adam7 final patch blended once");
    auto damaged = encoded;
    damaged[damaged.size() - 1] ^= std::byte{1};
    require(!Service{}.open_animation_decoder(memory_input(damaged)), "bad PNG CRC rejected");
    DecodeOptions tiny;
    tiny.limits.maximum_working_bytes = 16;
    const auto limited = Service{}.open_animation_decoder(memory_input(encoded), tiny);
    require(!limited && limited.error().code == ErrorCode::limit_exceeded,
            "working budget enforced");
    bool admitted = false;
    const auto admission = Service{}.open_animation_decoder(
        memory_input(encoded), {}, {}, {}, [&](const AnimationInfo& info) {
            admitted = info.document.frames.size() == 4;
            return false;
        });
    require(admitted && !admission && admission.error().code == ErrorCode::limit_exceeded,
            "indexed admission before canvas allocation");
}
void gifMetadata() {
    auto finite = gifFixture();
    const char* name = "NETSCAPE2.0";
    const auto found =
        std::search(finite.begin(), finite.end(), reinterpret_cast<const std::byte*>(name),
                    reinterpret_cast<const std::byte*>(name) + 11);
    require(found != finite.end(), "loop fixture marker");
    const auto offset = static_cast<std::size_t>(found - finite.begin());
    finite[offset + 13] = std::byte{2};
    auto result = Service{}.open_animation_decoder(memory_input(finite));
    require(bool(result) && result.value().info().total_play_count == 3,
            "GIF repeats plus first play");
    finite.erase(finite.begin() + static_cast<std::ptrdiff_t>(offset - 3),
                 finite.begin() + static_cast<std::ptrdiff_t>(offset + 16));
    result = Service{}.open_animation_decoder(memory_input(finite));
    require(bool(result) && result.value().info().total_play_count == 1,
            "GIF missing loop plays once");
    auto corruptRaster = gifFixture();
    const std::array<std::byte, 8> raster{std::byte{8}, std::byte{6}, std::byte{0},  std::byte{1},
                                          std::byte{8}, std::byte{4}, std::byte{16}, std::byte{16}};
    const auto rasterAt =
        std::search(corruptRaster.begin(), corruptRaster.end(), raster.begin(), raster.end());
    require(rasterAt != corruptRaster.end(), "GIF raster fixture marker");
    std::fill(rasterAt + 2, rasterAt + 8, std::byte{255});
    auto lazy = Service{}.open_animation_decoder(memory_input(corruptRaster));
    require(bool(lazy), "metadata scan does not decompress raster");
    require(!lazy.value().advance(), "corrupt compressed frame fails only on advance");
}
void gifComposition() {
    Document document;
    document.canvas_width = 2;
    document.canvas_height = 8;
    document.loop_count = 2;
    auto first = solid(2, 8, {255, 0, 0, 255});
    document.frames.push_back(first);
    auto green = solid(1, 1, {0, 255, 0, 255});
    green.disposal = FrameDisposal::previous;
    document.frames.push_back(green);
    auto transparent = solid(1, 1, {0, 0, 0, 0});
    transparent.x = 1;
    transparent.disposal = FrameDisposal::background;
    document.frames.push_back(transparent);
    auto blue = solid(1, 1, {0, 0, 255, 255});
    blue.y = 7;
    document.frames.push_back(blue);
    auto result =
        Service{}.open_animation_decoder(memory_input(encode(document, Format::gif, true)));
    require(bool(result), "GIF interlaced partial frames");
    auto session = std::move(result).value();
    const auto red = advance(session);
    const auto redPixel = pixel(red, 0, 0);
    require(redPixel[0] > redPixel[1] && redPixel[0] > redPixel[2], "GIF first color");
    const auto second = advance(session);
    require(pixel(second, 0, 0)[1] > pixel(second, 0, 0)[0], "GIF previous patch color");
    const auto third = advance(session);
    require(pixel(third, 0, 0) == redPixel, "GIF previous restores affected region");
    require(pixel(third, 1, 0) == redPixel, "GIF transparent index leaves underlying pixel");
    const auto fourth = advance(session);
    require(pixel(fourth, 1, 0)[3] == 0, "GIF transparent background clears region");
    require(pixel(fourth, 0, 7)[2] > pixel(fourth, 0, 7)[0], "GIF offset patch placement");

    // Distinct rows exercise all four GIF interlace passes, including short images.
    for (std::uint32_t height : {1U, 2U, 3U, 7U, 8U, 9U}) {
        Document striped;
        striped.canvas_width = 1;
        striped.canvas_height = height;
        auto image = MutableImage::allocate(1, height, kRgba8);
        require(bool(image), "allocate stripe fixture");
        for (std::uint32_t row = 0; row < height; ++row) {
            auto* destination = image.value().pixels().data() + row * 4U;
            destination[0] = static_cast<std::byte>(row * 24U);
            destination[1] = std::byte{0};
            destination[2] = std::byte{0};
            destination[3] = std::byte{255};
        }
        Frame frame;
        frame.image = std::move(image).value().freeze();
        striped.frames.push_back(frame);
        const auto bytes = encode(striped, Format::gif, true);
        auto native = Service{}.decode(memory_input(bytes));
        require(bool(native), "decode stripe reference");
        auto animated = Service{}.open_animation_decoder(memory_input(bytes));
        require(bool(animated), "open stripe GIF");
        auto animation = std::move(animated).value();
        const auto actual = advance(animation);
        for (std::uint32_t row = 0; row < height; ++row)
            require(pixel(actual, 0, row) == pixel(native.value().frames.front(), 0, row),
                    "GIF interlace row order matches independent decoder");
    }
}
void append24(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift : {0U, 8U, 16U})
        bytes.push_back(static_cast<std::byte>((value >> shift) & 255U));
}
void riffChunk(Bytes& bytes, const char* type, std::span<const std::byte> data) {
    bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(type),
                 reinterpret_cast<const std::byte*>(type) + 4);
    const auto size = static_cast<std::uint32_t>(data.size());
    for (unsigned shift : {0U, 8U, 16U, 24U})
        bytes.push_back(static_cast<std::byte>((size >> shift) & 255U));
    bytes.insert(bytes.end(), data.begin(), data.end());
    if (data.size() & 1U)
        bytes.push_back(std::byte{});
}
Bytes webpCompositionFixture() {
    Bytes body{std::byte{'W'}, std::byte{'E'}, std::byte{'B'}, std::byte{'P'}};
    Bytes header{std::byte{0x12}, std::byte{0}, std::byte{0}, std::byte{0}};
    append24(header, 3);
    append24(header, 3);
    riffChunk(body, "VP8X", header);
    const std::array<std::byte, 6> animation{std::byte{}, std::byte{},  std::byte{},
                                             std::byte{}, std::byte{3}, std::byte{0}};
    riffChunk(body, "ANIM", animation);
    const std::array<std::array<unsigned, 4>, 4> colors{
        {{255, 0, 0, 255}, {0, 255, 0, 128}, {0, 0, 255, 255}, {255, 255, 0, 255}}};
    for (std::uint32_t index = 0; index < 4; ++index) {
        const auto width = index == 0 ? 4U : 2U;
        Document patch;
        patch.canvas_width = width;
        patch.canvas_height = width;
        patch.frames.push_back(solid(width, width, colors[index]));
        const auto encoded = encode(patch, Format::webp);
        Bytes frame;
        append24(frame, index == 2 ? 1U : 0U);
        append24(frame, index == 3 ? 1U : 0U);
        append24(frame, width - 1U);
        append24(frame, width - 1U);
        append24(frame, 50);
        frame.push_back(static_cast<std::byte>(index == 1 ? 1U : 2U));
        frame.insert(frame.end(), encoded.begin() + 12, encoded.end());
        riffChunk(body, "ANMF", frame);
    }
    Bytes result{std::byte{'R'}, std::byte{'I'}, std::byte{'F'}, std::byte{'F'}};
    const auto size = static_cast<std::uint32_t>(body.size());
    for (unsigned shift : {0U, 8U, 16U, 24U})
        result.push_back(static_cast<std::byte>((size >> shift) & 255U));
    result.insert(result.end(), body.begin(), body.end());
    return result;
}
void webpComposition() {
    const auto bytes = webpCompositionFixture();
    auto result = Service{}.open_animation_decoder(memory_input(bytes));
    if (!result)
        std::cerr << result.error().message << '\n';
    require(bool(result), "open partial WebP");
    auto session = std::move(result).value();
    require(session.info().total_play_count == 3, "WebP loop count is total plays");
    const auto first = advance(session);
    require(pixel(first, 0, 0) == std::array<unsigned, 4>{255, 0, 0, 255}, "WebP source color");
    const auto second = advance(session);
    const auto blend = pixel(second, 0, 0);
    require(blend[0] >= 126 && blend[0] <= 128 && blend[1] >= 127 && blend[1] <= 129 &&
                blend[2] == 0 && blend[3] == 255,
            "WebP over blend");
    const auto third = advance(session);
    require(pixel(third, 0, 0)[3] == 0, "WebP background disposal clears region");
    require(pixel(third, 2, 0) == std::array<unsigned, 4>{0, 0, 255, 255}, "WebP patch offset");
    static_cast<void>(advance(session));
    require(pixel(first, 0, 0)[0] == 255 && pixel(second, 0, 0) == blend,
            "WebP native reusable canvas never mutates snapshots");
    require(bool(session.reset()), "WebP reset");
    require(pixel(advance(session), 0, 0) == pixel(first, 0, 0), "WebP reset replay");
}
void metadataSnapshots() {
    Document document;
    document.canvas_width = 17;
    document.canvas_height = 3;
    document.loop_count = 0;
    document.color.icc_profile = Bytes(4096, std::byte{7});
    document.metadata.exif = {std::byte{'I'}, std::byte{'I'}, std::byte{42}, std::byte{},
                              std::byte{8},   std::byte{},    std::byte{},   std::byte{},
                              std::byte{},    std::byte{},    std::byte{},   std::byte{},
                              std::byte{},    std::byte{}};
    document.metadata.xmp = Bytes(256 * 1024, std::byte{'x'});
    document.frames.push_back(solid(17, 3, {231, 47, 93, 129}));
    document.frames.push_back(solid(17, 3, {27, 183, 211, 255}));
    const auto encoded = encode(document, Format::webp);
    auto opened = Service{}.open_animation_decoder(memory_input(encoded));
    require(bool(opened), "open metadata-rich pixel snapshot fixture");
    auto session = std::move(opened).value();
    require(session.info().document.color.icc_profile == document.color.icc_profile &&
                session.info().document.metadata.exif == document.metadata.exif &&
                session.info().document.metadata.xmp == document.metadata.xmp,
            "session retains immutable source color and metadata once");
    const auto first = advance(session);
    const auto value = pixel(first, 0, 0);
    require(value[3] == 129 && value[0] <= value[3] && value[1] <= value[3] && value[2] <= value[3],
            "pixel snapshots retain partial-alpha premultiplication");
    static_cast<void>(advance(session));
    session = AnimationDecodeSession{};
    require(first.color.icc_profile == document.color.icc_profile &&
                first.metadata.exif == document.metadata.exif &&
                first.metadata.xmp == document.metadata.xmp,
            "existing Frame snapshot still preserves independently owned source metadata");
    pixelSnapshotLifetime(encoded);
}
void boundsAndTruncation() {
    std::vector<Bytes> fixtures;
    if (hasCodec(Format::gif))
        fixtures.push_back(gifFixture());
    if (hasCodec(Format::webp))
        fixtures.push_back(webpFixture());
    if (hasCodec(Format::png))
        fixtures.push_back(apng({{png(2, 2, {255, 0, 0, 255})}, {png(1, 1, {0, 255, 0, 255})}}));
    for (const auto& bytes : fixtures) {
        for (const auto cut : {std::size_t{0}, std::size_t{12}, bytes.size() / 2U}) {
            const Bytes truncated(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
            require(!Service{}.open_animation_decoder(memory_input(truncated)),
                    "truncated container rejected");
        }
        for (unsigned kind = 0; kind < 5; ++kind) {
            DecodeOptions options;
            switch (kind) {
            case 0:
                options.limits.maximum_input_bytes = bytes.size() - 1U;
                break;
            case 1:
                options.limits.maximum_pixels = 1;
                break;
            case 2:
                options.limits.maximum_frames = 1;
                break;
            case 3:
                options.limits.maximum_metadata_bytes = 1;
                break;
            case 4:
                options.limits.maximum_owned_output_bytes = 1;
                break;
            }
            const auto result = Service{}.open_animation_decoder(memory_input(bytes), options);
            require(!result && result.error().code == ErrorCode::limit_exceeded,
                    "animation input/canvas/frame/metadata/publication limits");
        }
    }
}
} // namespace

int main() {
    if (hasCodec(Format::gif)) {
        basic(gifFixture(), Format::gif);
        pixelSnapshotLifetime(gifFixture());
        gifMetadata();
        gifComposition();
    }
    if (hasCodec(Format::webp)) {
        basic(webpFixture(), Format::webp);
        pixelSnapshotLifetime(webpFixture());
        webpComposition();
        metadataSnapshots();
    }
    if (hasCodec(Format::png)) {
        pngComposition();
        pixelSnapshotLifetime(
            apng({{png(17, 3, {231, 47, 93, 129})}, {png(17, 3, {27, 183, 211, 255})}}, false,
                 false, 17, 3));
    }
    boundsAndTruncation();
    std::cout << "Animation decoder tests passed\n";
}
