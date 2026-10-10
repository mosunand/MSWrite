#pragma once

#include <QJsonObject>
#include <QUrl>
#include <limits>

// Browser-controlled values must pass these checks before reaching native actions.
namespace SecurityPolicy {
inline bool trustedPage(const QUrl &url, const QUrl &expected)
{
    return url.isValid() && expected.isValid() && url.scheme() == QLatin1String("https")
        && expected.scheme() == QLatin1String("https") && !url.host().isEmpty()
        && url.host().compare(expected.host(), Qt::CaseInsensitive) == 0
        && url.port(443) == 443 && expected.port(443) == 443
        && url.userInfo().isEmpty() && url.path() == expected.path();
}

inline bool externalUrl(const QUrl &url)
{
    if (!url.isValid() || !url.userInfo().isEmpty()) return false;
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        return !url.host().isEmpty();
    return scheme == QLatin1String("mailto") && !url.path().isEmpty()
        && !url.toString().contains(QChar('\n')) && !url.toString().contains(QChar('\r'));
}

inline bool trustedNavigation(const QUrl &url, const QUrl &expected)
{
    // The PDF host bootstraps on an explicitly requested empty page. It has no
    // message privileges; only its subsequent HTTPS export page can send messages.
    return trustedPage(url, expected)
        || (url == QUrl(QStringLiteral("about:blank")) && url == expected);
}

inline bool nonNegativeInt(const QJsonValue &value)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble(-1);
    return number >= 0 && number <= std::numeric_limits<int>::max() && number == int(number);
}

inline bool contentReply(const QJsonObject &object, int request)
{
    return object.value(QStringLiteral("md")).isString()
        && nonNegativeInt(object.value(QStringLiteral("rev")))
        && nonNegativeInt(object.value(QStringLiteral("request")))
        && object.value(QStringLiteral("request")).toInt() == request;
}

inline int oneBasedIndex(const QJsonValue &value, int count, int fallback = 1)
{
    if (!value.isUndefined() && !nonNegativeInt(value)) return -1;
    const int requested = value.isUndefined() ? fallback : value.toInt();
    // Validate before subtracting: INT_MIN - 1 is undefined C++ behavior.
    return requested >= 1 && requested <= count ? requested - 1 : -1;
}
}
