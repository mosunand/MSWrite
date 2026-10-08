// ai/AiCcSwitch.cpp — see ai/AiCcSwitch.h.

#include "ai/AiCcSwitch.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QUuid>

namespace {

QString tomlString(const QString &toml, const QString &key, const QString &section = QString())
{
    const QRegularExpression assignment(QStringLiteral("^\\s*%1\\s*=\\s*(.*)$").arg(QRegularExpression::escape(key)));
    QString active;
    for (const QString &line : toml.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1Char('['))) {
            const int end = trimmed.indexOf(QLatin1Char(']'));
            active = trimmed.mid(1, end - 1);
            active.remove(QLatin1Char('"')); active.remove(QLatin1Char('\''));
            continue;
        }
        if (active != section) continue;
        const auto match = assignment.match(line);
        if (!match.hasMatch()) continue;
        const QString value = match.captured(1).trimmed();
        if (value.startsWith(QLatin1Char('\''))) {
            const int end = value.indexOf(QLatin1Char('\''), 1);
            return end > 0 ? value.mid(1, end - 1) : QString();
        }
        if (value.startsWith(QLatin1Char('"'))) {
            int end = 1;
            for (; end < value.size(); ++end) {
                if (value[end] == QLatin1Char('\\')) { ++end; continue; }
                if (value[end] == QLatin1Char('"')) break;
            }
            const auto parsed = QJsonDocument::fromJson((QLatin1Char('[') + value.left(end + 1) + QLatin1Char(']')).toUtf8());
            return parsed.isArray() ? parsed.array().first().toString() : QString();
        }
    }
    return {};
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

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(settingsJson.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        p.error = QStringLiteral("Invalid cc-switch settings: %1").arg(name);
        return p;
    }
    const QJsonObject root = doc.object();
    p.sourceConfig = root;
    const QJsonObject env = root.value(QStringLiteral("env")).toObject();
    const QJsonObject auth = root.value(QStringLiteral("auth")).toObject();
    const QString toml = root.value(QStringLiteral("config")).toString();

    if (appType == QLatin1String("claude")) {
        p.protocol = Protocol::Anthropic;
        p.apiKey = firstEnv(env, {
            QStringLiteral("ANTHROPIC_AUTH_TOKEN"),
            QStringLiteral("ANTHROPIC_API_KEY"),
        });
        p.baseUrl = firstEnv(env, { QStringLiteral("ANTHROPIC_BASE_URL") });
        p.model = firstEnv(env, {
            QStringLiteral("ANTHROPIC_MODEL"),
            QStringLiteral("ANTHROPIC_DEFAULT_SONNET_MODEL"),
        });
        // 模型名原样保留，包括网关后缀。
    } else if (appType == QLatin1String("codex")) {
        const QString provider = tomlString(toml, QStringLiteral("model_provider"));
        const QString section = QStringLiteral("model_providers.") + provider;
        const QString wire = tomlString(toml, QStringLiteral("wire_api"), section);
        p.protocol = wire == QLatin1String("chat") ? Protocol::OpenAi
                     : wire.isEmpty() || wire == QLatin1String("responses") ? Protocol::OpenAiResponses
                     : Protocol::Unsupported;
        p.apiKey = auth.value(QStringLiteral("OPENAI_API_KEY")).toString();
        if (p.apiKey.isEmpty())
            p.apiKey = firstEnv(env, { QStringLiteral("OPENAI_API_KEY") });
        const QString envKey = tomlString(toml, QStringLiteral("env_key"), section);
        if (!envKey.isEmpty()) p.apiKey = firstEnv(env, {envKey});
        p.baseUrl = tomlString(toml, QStringLiteral("base_url"), section);
        if (p.baseUrl.isEmpty() && (provider.isEmpty() || provider == QLatin1String("openai")))
            p.baseUrl = QStringLiteral("https://api.openai.com/v1");
        p.model = tomlString(toml, QStringLiteral("model"));
    } else if (appType.startsWith(QLatin1String("gemini"))) {
        p.protocol = Protocol::Gemini;
        p.apiKey = firstEnv(env, {
            QStringLiteral("GEMINI_API_KEY"),
            QStringLiteral("GOOGLE_API_KEY"),
        });
        p.baseUrl = firstEnv(env, { QStringLiteral("GOOGLE_GEMINI_BASE_URL"), QStringLiteral("GEMINI_BASE_URL") });
        if (p.baseUrl.isEmpty()) p.baseUrl = QStringLiteral("https://generativelanguage.googleapis.com");
        p.model = firstEnv(env, { QStringLiteral("GEMINI_MODEL") });
    } else {
        p.protocol = Protocol::Unsupported;
        p.error = QStringLiteral("unsupported cc-switch app_type: %1").arg(appType);
    }

    p.ok = true;
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
    AiCcProvider selected;
    int bestRank = 100;
    for (const AiCcProvider &p : listAll()) {
        const int rank = p.appType.startsWith(QLatin1String("claude")) ? 0
                       : p.appType == QLatin1String("codex") ? 1 : 2;
        if (p.ok && p.isCurrent && rank < bestRank) {
            selected = p;
            bestRank = rank;
        }
    }
    if (!selected.ok)
        selected.error = QStringLiteral("cc-switch has no current provider");
    return selected;
}

QVector<AiCcProvider> AiCcSwitch::listAll(QString *error)
{
    if (error) error->clear();
    QVector<AiCcProvider> out;
    const QString dbPath = AiCcSwitch::dbPath();
    if (!QFileInfo::exists(dbPath)) {
        if (error) *error = QStringLiteral("找不到 cc-switch 配置：%1").arg(dbPath);
        return out;
    }

    const QString conn = QStringLiteral("mswrite-ccswitch-") + QUuid::createUuid().toString();
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        // Bound startup delay when cc-switch is writing; retry on the next launch.
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=250"));
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral(
                    "SELECT id, name, app_type, settings_config, is_current FROM providers ORDER BY name, app_type, id"))) {
                while (q.next()) {
                    AiCcProvider p = parseSettings(q.value(0).toString(), q.value(1).toString(),
                                                  q.value(2).toString(), q.value(3).toString());
                    p.isCurrent = q.value(4).toBool();
                    out.push_back(p);
                }
            } else if (error) *error = QStringLiteral("读取 cc-switch 失败：%1").arg(q.lastError().text());
        } else if (error) *error = QStringLiteral("打开 cc-switch 失败：%1").arg(db.lastError().text());
    }
    // Release SQLite handles after every read; no persistent connection to the user's DB.
    QSqlDatabase::removeDatabase(conn);
    return out;
}
