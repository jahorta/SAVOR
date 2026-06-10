#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QMetaType>

struct StatusToast
{
    enum class Severity {
        Info,
        Success,
        Warn,
        Error
    };

    Severity severity = Severity::Info;
    QString message;
    QString details;
    int count = 1;
    QDateTime createdAt;
    int ttlMs = 4000;
};

Q_DECLARE_METATYPE(StatusToast)

