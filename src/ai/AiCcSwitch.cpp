// ai/AiCcSwitch.cpp — see ai/AiCcSwitch.h.

#include "ai/AiCcSwitch.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

QString tomlString(const QString &toml, const QString &key)
{
    const QRegularExpression re(
        QStringLiteral("^\\s*%1\\s*=\\s*\"([^\"]*)\"").arg(QRegularExpression::escape(key)),
        QRegularExpression::MultilineOption);
    const auto m = re.match(toml);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString firstEnv(const QJsonObject &env, const QStringList &names)
{
    for (const QString &n : names) {
        if (env.contains(n) && env.value(n).isString() && !env.value(n).toString().isEmpty())
            return env.value(n).toString();
    }
    return {};
}

AiCcProvider parseSettings(const QString &id, const QString &name,
                           const QString &appType, const QString &settingsJson)
{
    AiCcProvider p;
    p.id = id;
    p.name = name;
    p.appType = appType;

    const QJsonDocument doc = QJsonDocument::fromJson(settingsJson.toUtf8());
    const QJsonObject root = doc.object();
    const QJsonObject env = root.value(QStringLiteral("env")).toObject();
    const QJsonObject auth = root.value(QStringLiteral("auth")).toObject();
    const QString toml = root.value(QStringLiteral("config")).toString();

    if (appType.startsWith(QLatin1String("claude"))) {
        p.protocol = Protocol::Anthropic;
        p.apiKey = firstEnv(env, {
            QStringLiteral("ANTHROPIC_AUTH_TOKEN"),
            QStringLiteral("ANTHROPIC_API_KEY"),
        });
        p.baseUrl = firstEnv(env, { QStringLiteral("ANTHROPIC_BASE_URL") });
        p.model = firstEnv(env, {
            QStringLiteral("ANTHROPIC_DEFAULT_SONNET_MODEL_NAME"),
            QStringLiteral("ANTHROPIC_MODEL"),
            QStringLiteral("ANTHROPIC_DEFAULT_SONNET_MODEL"),
        });
        // 模型名保留 [1M] 后缀:导入时拆成 contextWindow
    } else if (appType == QLatin1String("codex")) {
        p.protocol = Protocol::OpenAi;
        p.apiKey = auth.value(QStringLiteral("OPENAI_API_KEY")).toString();
        if (p.apiKey.isEmpty())
            p.apiKey = firstEnv(env, { QStringLiteral("OPENAI_API_KEY") });
        p.baseUrl = tomlString(toml, QStringLiteral("base_url"));
        p.model = tomlString(toml, QStringLiteral("model"));
    } else if (appType.startsWith(QLatin1String("gemini"))) {
        p.protocol = Protocol::OpenAi;
        p.apiKey = firstEnv(env, {
            QStringLiteral("GEMINI_API_KEY"),
            QStringLiteral("GOOGLE_API_KEY"),
        });
        p.baseUrl = firstEnv(env, { QStringLiteral("GEMINI_BASE_URL") });
        p.model = firstEnv(env, { QStringLiteral("GEMINI_MODEL") });
    } else {
        p.error = QStringLiteral("unsupported cc-switch app_type: %1").arg(appType);
        return p;
    }

    if (p.apiKey.isEmpty()) {
        p.error = QStringLiteral("cc-switch provider \"%1\" has no API key").arg(name);
        return p;
    }
    p.ok = true;
    return p;
}

AiCcProvider loadWhere(const QString &whereSql, const QVariant &bind)
{
    AiCcProvider p;
    const QString dbPath = AiCcSwitch::dbPath();
    if (!QFileInfo::exists(dbPath)) {
        p.error = QStringLiteral("cc-switch db not found: %1").arg(dbPath);
        return p;
    }

    const QString conn = QStringLiteral("mswrite-ccswitch");
    {
        QSqlDatabase db = QSqlDatabase::contains(conn)
            ? QSqlDatabase::database(conn)
            : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            p.error = QStringLiteral("cannot open cc-switch db: %1").arg(db.lastError().text());
            return p;
        }
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT id, name, app_type, settings_config FROM providers WHERE %1 LIMIT 1")
                      .arg(whereSql));
        q.addBindValue(bind);
        if (!q.exec() || !q.next()) {
            p.error = QStringLiteral("no matching cc-switch provider");
            return p;
        }
        p = parseSettings(q.value(0).toString(),
                          q.value(1).toString(),
                          q.value(2).toString(),
                          q.value(3).toString());
    }
    return p;
}

} // namespace

QString AiCcSwitch::dbPath()
{
    const QByteArray env = qgetenv("CC_SWITCH_HOME");
    const QString home = env.isEmpty()
        ? QDir::homePath() + QStringLiteral("/.cc-switch")
        : QDir::cleanPath(QString::fromLocal8Bit(env));
    return home + QStringLiteral("/cc-switch.db");
}

AiCcProvider AiCcSwitch::loadCurrent()
{
    // 优先当前选中的 claude 系供应商
    AiCcProvider p = loadWhere(QStringLiteral("is_current = 1 AND app_type LIKE ?"),
                               QStringLiteral("claude%"));
    if (p.ok)
        return p;
    p = loadWhere(QStringLiteral("is_current = 1 AND app_type = ?"), QStringLiteral("codex"));
    if (p.ok)
        return p;
    p = loadWhere(QStringLiteral("is_current = 1 AND id IS NOT ?"), QString());
    if (p.ok)
        return p;
    if (p.error.isEmpty())
        p.error = QStringLiteral("cc-switch has no current provider");
    return p;
}

QVector<AiCcProvider> AiCcSwitch::listAll()
{
    QVector<AiCcProvider> out;
    const QString dbPath = AiCcSwitch::dbPath();
    if (!QFileInfo::exists(dbPath))
        return out;

    const QString conn = QStringLiteral("mswrite-ccswitch");
    QSqlDatabase db = QSqlDatabase::contains(conn)
        ? QSqlDatabase::database(conn)
        : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(dbPath);
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!db.open())
        return out;

    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT id, name, app_type, settings_config FROM providers ORDER BY name"))) {
        return out;
    }
    while (q.next()) {
        out.push_back(parseSettings(q.value(0).toString(),
                                    q.value(1).toString(),
                                    q.value(2).toString(),
                                    q.value(3).toString()));
    }
    return out;
}
