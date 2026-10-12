#pragma once

#include "pinned_animation_native_fixtures.h"

#include <QColorSpace>

namespace pinned_animation_optimization_fixtures {
inline QByteArray xmp(qsizetype payloadBytes) {
    QByteArray result(
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
        "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description "
        "rdf:about=\"\"><snow:payload xmlns:snow=\"https://snow-shot.example/benchmark/\">");
    result.append(QByteArray(payloadBytes, 'a'));
    result.append("</snow:payload></rdf:Description></rdf:RDF></x:xmpmeta>");
    return result;
}

inline void appendLittle32(QByteArray& bytes, quint32 value) {
    for (unsigned shift : {0U, 8U, 16U, 24U})
        bytes.append(static_cast<char>((value >> shift) & 255U));
}

inline void webpChunk(QByteArray& bytes, const char* type, const QByteArray& payload) {
    bytes.append(type, 4);
    appendLittle32(bytes, static_cast<quint32>(payload.size()));
    bytes.append(payload);
    if (payload.size() % 2)
        bytes.append('\0');
}

inline QByteArray webp(qsizetype metadataBytes) {
    auto result = pinned_animation_fixtures::images()[1].bytes;
    const QByteArray profile = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    QByteArray profileChunk;
    webpChunk(profileChunk, "ICCP", profile);
    result.insert(30, profileChunk); // ICCP follows VP8X and precedes ANIM.
    result[20] = static_cast<char>(0x22 | (metadataBytes > 0 ? 0x04 : 0));
    if (metadataBytes > 0)
        webpChunk(result, "XMP ", xmp(metadataBytes));
    QByteArray riffSize;
    appendLittle32(riffSize, static_cast<quint32>(result.size() - 8));
    result.replace(4, 4, riffSize);
    return result;
}

inline QByteArray apng(qsizetype metadataBytes) {
    QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::red);
    image.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        throw std::runtime_error("Could not encode optimization fixture color profile.");
    QByteArray common;
    for (qsizetype position = 8; position <= encoded.size() - 12;) {
        const auto length = static_cast<qsizetype>(
            pinned_animation_native_fixtures::detail::read32(encoded.constData() + position));
        if (length > encoded.size() - position - 12)
            throw std::runtime_error("Optimization profile PNG has an invalid chunk length.");
        const auto type = encoded.mid(position + 4, 4);
        if (type == "iCCP" || type == "sRGB" || type == "gAMA" || type == "cHRM" || type == "cICP")
            common.append(encoded.mid(position, length + 12));
        position += length + 12;
    }
    if (common.isEmpty())
        throw std::runtime_error("Optimization fixture has no color profile declaration.");
    if (metadataBytes > 0) {
        QByteArray text("XML:com.adobe.xmp");
        text.append('\0');
        text.append(QByteArray(4, '\0')); // Uncompressed, method zero, empty language/translation.
        text.append(xmp(metadataBytes));
        pinned_animation_native_fixtures::detail::chunk(common, "iTXt", text);
    }
    auto result = pinned_animation_fixtures::images()[2].bytes;
    result.insert(33, common); // Color and text precede animation control and frame pixels.
    return result;
}
} // namespace pinned_animation_optimization_fixtures
