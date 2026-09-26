#pragma once
// ai/AiText.h — MathRender 需要的显示宽度计算(与 MS-Agent util/Text 同源,
// 本地化以便 ai/ 模块自包含)。

#include <QChar>
#include <QString>

namespace Text {

int charDisplayWidth(QChar c);
int visibleWidth(const QString &s);

} // namespace Text
