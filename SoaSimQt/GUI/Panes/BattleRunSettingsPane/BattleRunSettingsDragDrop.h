#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDataStream>
#include <QtCore/QIODevice>
#include <QtCore/QString>
#include <QtCore/Qt>

class QMimeData;

namespace battlerunsettings {

inline QString presetMimeType()
{
    return QStringLiteral("application/x-soasim-turn-action-preset");
}

constexpr int kPresetIdRole = Qt::UserRole + 1;

inline QByteArray encodePresetId(const qint64 presetId)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << presetId;
    return payload;
}

inline bool decodePresetId(const QMimeData* mimeData, qint64* presetId)
{
    if (!mimeData || !presetId || !mimeData->hasFormat(presetMimeType())) {
        return false;
    }

    QByteArray payload = mimeData->data(presetMimeType());
    QDataStream stream(&payload, QIODevice::ReadOnly);
    qint64 decodedPresetId = 0;
    stream >> decodedPresetId;
    if (stream.status() != QDataStream::Ok || decodedPresetId <= 0) {
        return false;
    }

    *presetId = decodedPresetId;
    return true;
}

} // namespace battlerunsettings
