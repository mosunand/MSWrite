#pragma once

#include <QString>
#include <QJsonArray>

// C++ -> JS 调用构造:生成 window.msbridge.<fn>(<args>) 并做 JSON 转义
namespace Bridge {

// 例:Bridge::call("setContent", {QJsonValue(md), QJsonValue(true)})
// -> "window.msbridge.setContent(\"...\",true)"
QString call(const QString &function, const QJsonArray &args = {});

inline QString call0(const QString &function) { return call(function); }
inline QString call1(const QString &function, const QString &arg)
{ return call(function, QJsonArray{arg}); }
inline QString call1i(const QString &function, int arg)
{ return call(function, QJsonArray{arg}); }

} // namespace Bridge
