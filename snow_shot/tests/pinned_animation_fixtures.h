#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace pinned_animation_fixtures {
struct Fixture final {
    QString suffix;
    QString mimeType;
    QByteArray bytes;
};

inline QVector<Fixture> images() {
    // Two 2x2 frames, with 100/200 ms durations and an infinite loop.
    return {
        {QStringLiteral("gif"), QStringLiteral("image/gif"),
         QByteArray::fromBase64(
             "R0lGODlhAgACAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQACgAAACwAAAAAAgAC"
             "AAAIBgABCAQQEAAh+QQBFAABACwAAAAAAgACAIEAgAAAAAAAAAAAAAAIBgABCAQQEAA7")},
        {QStringLiteral("webp"), QStringLiteral("image/webp"),
         QByteArray::fromBase64(
             "UklGRsYAAABXRUJQVlA4WAoAAAACAAAAAQAAAQAAQU5JTQYAAAAAAAAAAABBTk1GSgAAAAAAAAAAAAEA"
             "AAEAAGQAAAJWUDggMgAAADABAJ0BKgIAAgABQCYloAADcAD+8ut///mwP/bz/wR6Af//0uD//pcH//S4"
             "P/SkAAAAQU5NRkgAAAAAAAAAAAABAAABAADIAAAAVlA4IDAAAAA0AQCdASoCAAIAAAAmJaAAA3AA/vNpl"
             "//5sD/yD/5B9/v//00j/+mkf/00j5TQAAA=")},
        {QStringLiteral("apng"), QStringLiteral("image/apng"),
         QByteArray::fromBase64(
             "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAACGFjVEwAAAACAAAAAPONk3AAAAAaZmNU"
             "TAAAAAAAAAACAAAAAgAAAAAAAAAAAAEACgAA6FTcAAAAABVJREFUeJxj/M/A8J+BgYGBCUSAMAAfFwIC"
             "AkezFAAAABpmY1RMAAAAAQAAAAIAAAACAAAAAAAAAAAAAQAFAAB4e3HpAAAAGWZkQVQAAAACeJxjZGhg"
             "+M/AwMDABCJAGAAWKAGD+M3+AwAAAABJRU5ErkJggg==")},
    };
}

inline QVector<Fixture> singleFrameImages() {
    return {
        {QStringLiteral("gif"), QStringLiteral("image/gif"),
         QByteArray::fromBase64(
             "R0lGODlhAgACAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQACgAAACwAAAAAAgAC"
             "AAAIBgABCAQQEAA7")},
        // Blue 2x2 poster, excluded from playback; only a red 1x1 frame belongs
        // to the sequence, so the remaining composed canvas stays transparent.
        {QStringLiteral("apng"), QStringLiteral("image/apng"),
         QByteArray::fromBase64(
             "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAACGFjVEwAAAABAAAAALQt6aAAAAAQSURB"
             "VHicY2Bg+P8fgqEMAD/SB/m0Ek/NAAAAGmZjVEwAAAAAAAAAAQAAAAEAAAAAAAAAAAABAAoAAFp/MNAA"
             "AAARZmRBVAAAAAF4nGP4z8DwHwAFAAH/5vS2vwAAAABJRU5ErkJggg==")},
    };
}

inline Fixture hiddenPosterAnimation() {
    // Blue poster is excluded. Playback starts with red at (0,0), transparent
    // elsewhere; the second frame adds green at (1,1).
    return {QStringLiteral("apng"), QStringLiteral("image/apng"),
            QByteArray::fromBase64(
                "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAACGFjVEwAAAACAAAAAPONk3AAAAAQSURB"
                "VHicY2Bg+P8fgqEMAD/SB/m0Ek/NAAAAGmZjVEwAAAAAAAAAAQAAAAEAAAAAAAAAAAABAAoAAFp/MNAA"
                "AAARZmRBVAAAAAF4nGP4z8DwHwAFAAH/5vS2vwAAABpmY1RMAAAAAgAAAAEAAAABAAAAAQAAAAEAAQA"
                "FAADbTHBwAAAAEWZkQVQAAAADeJxjYPjP8B8ABAEB///oCOoAAAAASUVORK5CYII=")};
}
} // namespace pinned_animation_fixtures
