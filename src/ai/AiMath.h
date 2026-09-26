#pragma once
// MathRender.h — LaTeX → terminal art (Unicode + ASCII layout).
// Handles the common subset: greek letters, \frac{a}{b} with a real fraction
// bar, ^{} _{} as unicode super/subscripts where possible (ASCII fallback),
// \sqrt{}, \sum/\int/\lim with limits, \begin{matrix}, display spacing.
// Anything unknown is kept as-is.

#include <QString>
#include <QStringList>

namespace MathRender {

// Render one formula (contents only, no $ delimiters).
// block=true → display style (may span multiple lines, centered-ish).
QString render(const QString& latex, bool block);

} // namespace MathRender
