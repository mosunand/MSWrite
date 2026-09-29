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
    // 桥调用异常不能只进 console(WebView2 下 GUI 不可见)——同时上报宿主,
    // 落进 mswrite.log,排查"静默失效"类问题
    return QStringLiteral(
        "try{window.msbridge.%1(%2)}"
        "catch(e){console.error('%1',e);"
        "try{window.chrome.webview.postMessage({t:'jserror',"
        "msg:String(e&&e.stack||e),src:'bridge.%1',line:0})}catch(_){}}")
        .arg(function, parts.join(QLatin1Char(',')));
}

} // namespace Bridge
