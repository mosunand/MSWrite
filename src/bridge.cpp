#include "bridge.h"

#include <QJsonDocument>
#include <QStringList>

namespace Bridge {

QString call(const QString &function, const QJsonArray &args)
{
    QStringList parts;
    for (const QJsonValue &v : args) {
        // QJsonDocument 输出保证是合法 JS 字面量(字符串带引号与转义)
        parts << QString::fromUtf8(
            QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
    }
    return QStringLiteral("try{window.msbridge.%1(%2)}catch(e){console.error('%1',e)}"
                          ).arg(function, parts.join(QLatin1Char(',')));
}

} // namespace Bridge
