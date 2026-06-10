#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDataStream>
#include <QtCore/QIODevice>
#include <QtCore/QString>
#include <QtCore/Qt>
#include <QtCore/QMimeData>

class QMimeData;

namespace battlerunsettings {

inline QString presetMimeType()
{
    return QStringLiteral("application/x-savor-turn-action-preset");
}

inline QString predicateMimeType()
{
    return QStringLiteral("application/x-savor-predicate-spec");
}

constexpr int kPresetIdRole = Qt::UserRole + 1;
constexpr int kPredicateIdRole = Qt::UserRole + 2;

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

inline QByteArray encodePredicateId(const qint64 predicateId)
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << predicateId;
    return payload;
}

inline bool decodePredicateId(const QMimeData* mimeData, qint64* predicateId)
{
    if (!mimeData || !predicateId || !mimeData->hasFormat(predicateMimeType())) {
        return false;
    }

    QByteArray payload = mimeData->data(predicateMimeType());
    QDataStream stream(&payload, QIODevice::ReadOnly);
    qint64 decodedPredicateId = 0;
    stream >> decodedPredicateId;
    if (stream.status() != QDataStream::Ok || decodedPredicateId <= 0) {
        return false;
    }

    *predicateId = decodedPredicateId;
    return true;
}

} // namespace battlerunsettings
