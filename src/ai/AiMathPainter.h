#pragma once
// ai/AiMathPainter.h — 把 LaTeX 公式画成 PNG(QPainter),供聊天气泡嵌入。
// 自绘排版:分数(真横线)、上下标(小字号错位)、根号、∑/∫ 上下限、
// 希腊字母与关系符全部 Unicode 映射;透明底、按主题前景色、2x 抗锯齿。

#include <QColor>
#include <QPixmap>
#include <QString>

namespace AiMathPainter {

// 渲染 LaTeX 片段为位图;fg = 主题文字色。失败返回空 QPixmap
// (调用方退回 MathRender 字符画)。
QPixmap render(const QString &latex, const QColor &fg, double dpr = 2.0);

} // namespace AiMathPainter
