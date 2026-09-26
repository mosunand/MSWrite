// ai/AiText.cpp — see ai/AiText.h。(自 MS-Agent util/Text.cpp 提取的两个函数)

#include "ai/AiText.h"

namespace Text {

int charDisplayWidth(QChar c)
{
    const quint32 u = c.unicode();
    if ((u >= 0x1100 && u <= 0x115F)
        || (u >= 0x2E80 && u <= 0xD7AF)
        || (u >= 0xF900 && u <= 0xFAFF)
        || (u >= 0xFE30 && u <= 0xFE4F)
        || (u >= 0xFF00 && u <= 0xFF60)
        || (u >= 0xFFE0 && u <= 0xFFE6)
        || (u >= 0x20000 && u <= 0x3FFFD))
        return 2;
    return 1;
}

namespace {
// CSI 序列终点(供 visibleWidth 跳过 ANSI)
int ansiEnd(const QString &s, int i)
{
    int j = i + 1;
    if (j < s.size() && s.at(j).unicode() == 0x5b) // '['
        ++j;
    for (; j < s.size(); ++j) {
        const quint32 u = s.at(j).unicode();
        if (u >= 0x40 && u <= 0x7E && u != 0x5b)
            return j;
        if (u < 0x20 || u > 0x3f)
            return j;
    }
    return s.size() - 1;
}
} // namespace

int visibleWidth(const QString &s)
{
    int w = 0;
    for (int i = 0; i < s.size(); ++i) {
        if (s.at(i) == QChar(0x1b)) {
            i = ansiEnd(s, i);
            continue;
        }
        w += charDisplayWidth(s.at(i));
    }
    return w;
}

} // namespace Text
