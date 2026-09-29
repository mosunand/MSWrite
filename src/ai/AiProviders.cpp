// ai/AiProviders.cpp — see ai/AiProviders.h.

#include "ai/AiProviders.h"

#include "ai/AiCcSwitch.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>

namespace {

AiProvider fromJson(const QJsonObject &o)
{
    AiProvider p;
    p.id = o.value(QStringLiteral("id")).toString();
    p.name = o.value(QStringLiteral("name")).toString();
    p.protocol = (o.value(QStringLiteral("protocol")).toString().trimmed().toLower()
                  == QLatin1String("openai"))
                     ? Protocol::OpenAi
                     : Protocol::Anthropic;
    p.apiKey = o.value(QStringLiteral("apiKey")).toString();
    p.baseUrl = o.value(QStringLiteral("baseUrl")).toString();
    // 模型名原样保留([1M] 等后缀是网关约定,由用户自己写进模型名)
    p.model = o.value(QStringLiteral("model")).toString();
    // 历史遗留的独立窗口字段:仅兼容旧文件,发送时不再拼接
    p.contextWindow = o.value(QStringLiteral("contextWindow")).toString().toUpper();
    p.maxTokens = o.value(QStringLiteral("maxTokens")).toInt(0);
    p.importedFrom = o.value(QStringLiteral("importedFrom")).toString();
    p.importedId = o.value(QStringLiteral("importedId")).toString();
    while (p.baseUrl.endsWith(QLatin1Char('/')))
        p.baseUrl.chop(1);
    if (p.id.isEmpty())
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return p;
}

QJsonObject toJson(const AiProvider &p)
{
    return QJsonObject{
        { QStringLiteral("id"), p.id },
        { QStringLiteral("name"), p.name },
        { QStringLiteral("protocol"), protocolName(p.protocol) },
        { QStringLiteral("apiKey"), p.apiKey },
        { QStringLiteral("baseUrl"), p.baseUrl },
        { QStringLiteral("model"), p.model },
        { QStringLiteral("contextWindow"), p.contextWindow },
        { QStringLiteral("maxTokens"), p.maxTokens },
        { QStringLiteral("importedFrom"), p.importedFrom },
        { QStringLiteral("importedId"), p.importedId },
    };
}

// 模型名不再拆分/拼接窗口后缀:原样保存、原样发送
void normalizeModel(AiProvider &)
{
}

bool usable(const AiProvider &p)
{
    return !p.apiKey.trimmed().isEmpty() && !p.baseUrl.trimmed().isEmpty()
        && !p.model.trimmed().isEmpty();
}

QString normalizedUrl(QString url)
{
    url = url.trimmed();
    while (url.endsWith(QLatin1Char('/')))
        url.chop(1);
    return QUrl(url).toString();
}

QString msAgentProvidersFile()
{
    const QByteArray env = qgetenv("MSAGENT_HOME");
    const QString home = env.isEmpty() ? QDir::homePath() + QStringLiteral("/.ms-agent")
                                       : QDir::cleanPath(QString::fromLocal8Bit(env));
    return home + QStringLiteral("/providers.json");
}

} // namespace

QString AiProviderStore::defaultFilePath()
{
    // 手工拼单层目录(与图片池同款):QStandardPaths 会拼出 Mswrite\Mswrite 双层
    const QString base = qEnvironmentVariable("APPDATA")
                         + QStringLiteral("/Mswrite");
    return base + QStringLiteral("/ai-providers.json");
}

AiProviderStore AiProviderStore::load()
{
    AiProviderStore store(defaultFilePath());
    if (store.autoImportCcSwitch_ && store.loadError_.isEmpty())
        store.importFromCcSwitch(nullptr, true);
    return store;
}

AiProviderStore::AiProviderStore(const QString &path)
    : path_(path)
{
    if (path_.isEmpty())
        return;
    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly)) {
        if (QFileInfo::exists(path_))
            loadError_ = QStringLiteral("无法读取供应商配置，已保留原文件：%1").arg(path_);
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()
        || !doc.object().value(QStringLiteral("providers")).isArray()) {
        loadError_ = QStringLiteral("供应商配置格式错误，已保留原文件：%1").arg(path_);
        return;
    }
    const QJsonObject root = doc.object();
    autoImportCcSwitch_ = root.value(QStringLiteral("autoImportCcSwitch")).toBool(true);
    for (const auto &v : root.value(QStringLiteral("ignoredCcSwitch")).toArray()) {
        if (v.isString())
            ignoredCcSwitch_ << v.toString();
    }
    current_ = root.value(QStringLiteral("current")).toString();
    const QJsonArray arr = root.value(QStringLiteral("providers")).toArray();
    QStringList names;
    for (const QJsonValue &v : arr) {
        AiProvider p = fromJson(v.toObject());
        if (p.name.isEmpty())
            continue;
        // 重名自动加序号,保证唯一
        QString base = p.name;
        if (names.contains(p.name, Qt::CaseInsensitive)) {
            for (int i = 2; i < 1000; ++i) {
                const QString n = base + QLatin1Char('-') + QString::number(i);
                if (!names.contains(n, Qt::CaseInsensitive)) {
                    p.name = n;
                    break;
                }
            }
        }
        names << p.name;
        items_.push_back(p);
    }
    ensureCurrent();
}

void AiProviderStore::ensureCurrent()
{
    if (const AiProvider *p = find(current_); p && usable(*p))
        return;
    current_.clear();
    for (const AiProvider &p : items_) {
        if (usable(p)) {
            current_ = p.name;
            break;
        }
    }
}

const AiProvider *AiProviderStore::current() const
{
    if (current_.isEmpty())
        return nullptr;
    return find(current_);
}

const AiProvider *AiProviderStore::find(const QString &name) const
{
    for (const AiProvider &p : items_) {
        if (p.name.compare(name, Qt::CaseInsensitive) == 0)
            return &p;
    }
    return nullptr;
}

AiProvider *AiProviderStore::findMut(const QString &name)
{
    for (AiProvider &p : items_) {
        if (p.name.compare(name, Qt::CaseInsensitive) == 0)
            return &p;
    }
    return nullptr;
}

bool AiProviderStore::save(QString *err) const
{
    if (!loadError_.isEmpty()) {
        if (err) *err = loadError_;
        return false;
    }
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QJsonArray arr;
    for (const AiProvider &p : items_)
        arr.append(toJson(p));
    QJsonObject root{
        { QStringLiteral("version"), 1 },
        { QStringLiteral("current"), current_ },
        { QStringLiteral("providers"), arr },
        { QStringLiteral("autoImportCcSwitch"), autoImportCcSwitch_ },
        { QStringLiteral("ignoredCcSwitch"), QJsonArray::fromStringList(ignoredCcSwitch_) },
    };
    QSaveFile f(path_);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err)
            *err = QStringLiteral("无法写入 %1").arg(path_);
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (f.write(data) != data.size() || !f.commit()) {
        if (err)
            *err = QStringLiteral("无法提交 %1").arg(path_);
        return false;
    }
    return true;
}

QString AiProviderStore::add(const AiProvider &pin)
{
    AiProvider p = pin;
    p.name = p.name.trimmed();
    if (p.name.isEmpty())
        return QStringLiteral("名称不能为空");
    if (find(p.name))
        return QStringLiteral("供应商已存在:%1").arg(p.name);
    normalizeModel(p);
    if (p.apiKey.isEmpty())
        return QStringLiteral("API Key 不能为空");
    if (p.baseUrl.isEmpty())
        return QStringLiteral("API 地址不能为空");
    if (p.model.isEmpty())
        return QStringLiteral("模型不能为空");
    if (p.id.isEmpty())
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto before = *this;
    items_.push_back(p);
    if (current_.isEmpty())
        current_ = p.name;
    QString err;
    if (!save(&err)) {
        *this = before;
        return err;
    }
    return {};
}

QString AiProviderStore::upsert(const AiProvider &pin, const QString &originalName)
{
    const auto before = *this;
    AiProvider p = pin;
    p.name = p.name.trimmed();
    if (p.name.isEmpty())
        return QStringLiteral("名称不能为空");
    normalizeModel(p);
    if (p.apiKey.isEmpty())
        return QStringLiteral("API Key 不能为空");
    if (p.baseUrl.isEmpty())
        return QStringLiteral("API 地址不能为空");
    if (p.model.isEmpty())
        return QStringLiteral("模型不能为空");

    AiProvider *existing = nullptr;
    if (!originalName.isEmpty())
        existing = findMut(originalName);
    if (!existing && !p.id.isEmpty()) {
        for (AiProvider &x : items_) {
            if (x.id == p.id) {
                existing = &x;
                break;
            }
        }
    }
    if (!existing)
        return add(p);

    if (p.name.compare(existing->name, Qt::CaseInsensitive) != 0 && find(p.name))
        return QStringLiteral("供应商已存在:%1").arg(p.name);
    if (p.id.isEmpty())
        p.id = existing->id;
    // Editing a copied provider must retain its source identity for deduplication.
    p.importedFrom = existing->importedFrom;
    p.importedId = existing->importedId;
    const bool wasCurrent = existing->name.compare(current_, Qt::CaseInsensitive) == 0;
    *existing = p;
    if (wasCurrent)
        current_ = p.name;
    QString err;
    if (!save(&err)) {
        *this = before;
        return err;
    }
    return {};
}

QString AiProviderStore::remove(const QString &name)
{
    const AiProvider *p = find(name);
    if (!p)
        return QStringLiteral("未找到供应商:%1").arg(name);
    const auto before = *this;
    const QString source = p->importedId.isEmpty() ? p->importedFrom : p->importedId;
    if (source.startsWith(QLatin1String("cc-switch:")) && !ignoredCcSwitch_.contains(source))
        ignoredCcSwitch_ << source;
    for (int i = 0; i < items_.size(); ++i) {
        if (items_.at(i).name.compare(name, Qt::CaseInsensitive) == 0) {
            items_.removeAt(i);
            break;
        }
    }
    ensureCurrent();
    QString err;
    if (!save(&err)) {
        *this = before;
        return err;
    }
    return {};
}

QString AiProviderStore::setCurrent(const QString &name)
{
    const AiProvider *p = find(name);
    if (!p)
        return QStringLiteral("未找到供应商:%1").arg(name);
    if (!usable(*p))
        return QStringLiteral("供应商 %1 需要填写 API Key、API 地址和模型").arg(p->name);
    const auto before = *this;
    current_ = p->name;
    QString err;
    if (!save(&err)) {
        *this = before;
        return err;
    }
    return {};
}

QString AiProviderStore::setAutoImportCcSwitch(bool enabled)
{
    const auto before = *this;
    autoImportCcSwitch_ = enabled;
    QString err;
    if (!save(&err)) {
        *this = before;
        return err;
    }
    return {};
}

int AiProviderStore::importFromCcSwitch(QString *report, bool automatic)
{
    if (!loadError_.isEmpty()) {
        if (report) *report = loadError_;
        return -1;
    }
    const QVector<AiCcProvider> src = AiCcSwitch::listAll();
    if (src.isEmpty()) {
        if (report)
            *report = QStringLiteral("没有读到 cc-switch 供应商(%1)").arg(AiCcSwitch::dbPath());
        return -1;
    }

    const auto before = *this;
    int added = 0;
    int completed = 0;
    bool changed = false;
    QStringList skipped;
    QStringList importedNames;
    QString preferred;
    int preferredRank = 100;

    for (const AiCcProvider &c : src) {
        if (!c.ok || c.apiKey.trimmed().isEmpty() || c.baseUrl.trimmed().isEmpty()) {
            if (!c.name.isEmpty())
                skipped << c.name;
            continue;
        }
        QString wanted = c.name.trimmed();
        if (wanted.isEmpty())
            wanted = c.appType;
        const QString tag = QStringLiteral("cc-switch:%1/%2").arg(c.appType, c.name);
        const QString identity = c.id.isEmpty() ? tag
            : QStringLiteral("cc-switch:%1/%2").arg(c.appType, c.id);
        if (automatic && (ignoredCcSwitch_.contains(identity) || ignoredCcSwitch_.contains(tag))) {
            skipped << wanted;
            continue;
        }
        if (!automatic) {
            changed |= ignoredCcSwitch_.removeAll(identity) > 0;
            changed |= ignoredCcSwitch_.removeAll(tag) > 0;
        }
        AiProvider *existing = nullptr;
        for (AiProvider &p : items_) {
            if (p.importedId == identity || (p.importedId.isEmpty() && p.importedFrom == tag)) {
                existing = &p;
                break;
            }
        }
        if (!existing) {
            // Legacy metadata can regain its key only when name, URL and protocol
            // all match. A same-name provider on another endpoint is separate.
            AiProvider *named = findMut(wanted);
            if (named && named->protocol == c.protocol
                && normalizedUrl(named->baseUrl) == normalizedUrl(c.baseUrl)
                && named->importedId.isEmpty())
                existing = named;
        }
        if (existing) {
            if (existing->importedFrom == tag && existing->importedId.isEmpty()) {
                existing->importedId = identity;
                changed = true;
            }
            if (existing->apiKey.trimmed().isEmpty() && existing->protocol == c.protocol
                && normalizedUrl(existing->baseUrl) == normalizedUrl(c.baseUrl)) {
                existing->apiKey = c.apiKey;
                if (existing->model.trimmed().isEmpty()) existing->model = c.model;
                existing->importedFrom = tag;
                existing->importedId = identity;
                ++completed;
                changed = true;
            } else {
                skipped << wanted;
            }
        } else {
            AiProvider p;
            p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            p.name = wanted;
            const QString disambiguated = wanted + QStringLiteral(" (cc-switch/%1)").arg(c.appType);
            for (int n = 1; find(p.name); ++n)
                p.name = disambiguated + (n == 1 ? QString() : QStringLiteral(" %1").arg(n));
            p.protocol = c.protocol;
            p.apiKey = c.apiKey;
            p.baseUrl = normalizedUrl(c.baseUrl);
            p.model = c.model; // 不猜测源配置缺少的模型
            p.importedFrom = tag;
            p.importedId = identity;
            items_.push_back(p);
            existing = &items_.last();
            importedNames << p.name;
            ++added;
            changed = true;
        }
        const int rank = c.appType.startsWith(QLatin1String("claude")) ? 0
                       : c.appType == QLatin1String("codex") ? 1 : 2;
        if (c.isCurrent && usable(*existing) && rank < preferredRank) {
            preferred = existing->name;
            preferredRank = rank;
        }
    }

    if (current_.isEmpty()) {
        current_ = preferred;
        ensureCurrent();
        changed |= current_ != before.current_;
    }

    QString err;
    if (changed && !save(&err)) {
        *this = before;
        if (report)
            *report = err;
        return -1;
    }
    if (report) {
        QString r = QStringLiteral("新增 %1 个，补全 Key %2 个，跳过 %3 个")
                        .arg(added)
                        .arg(completed)
                        .arg(skipped.size());
        if (!importedNames.isEmpty())
            r += QStringLiteral(";新增:") + importedNames.join(QStringLiteral(", "));
        if (!skipped.isEmpty())
            r += QStringLiteral(";跳过:") + skipped.join(QStringLiteral(", "));
        *report = r;
    }
    return added + completed;
}

int AiProviderStore::importFromMsAgent(QString *report)
{
    const auto before = *this;
    const QString src = msAgentProvidersFile();
    QFile f(src);
    if (!f.open(QIODevice::ReadOnly)) {
        if (report)
            *report = QStringLiteral("找不到 MS-Agent 配置(%1),可先在 MS-Agent 里添加再导入").arg(src);
        return -1;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonArray arr = root.value(QStringLiteral("providers")).toArray();

    int added = 0;
    QStringList skipped;
    QStringList importedNames;
    for (const QJsonValue &v : arr) {
        AiProvider p = fromJson(v.toObject());
        if (p.name.isEmpty() || p.apiKey.isEmpty() || p.baseUrl.isEmpty()) {
            if (!p.name.isEmpty())
                skipped << p.name;
            continue;
        }
        if (find(p.name)) {
            skipped << p.name;
            continue;
        }
        if (p.model.isEmpty())
            p.model = QStringLiteral("glm-5.3");
        items_.push_back(p);
        importedNames << p.name;
        ++added;
    }
    if (current_.isEmpty() && added > 0)
        current_ = importedNames.first();

    QString err;
    if (added > 0 && !save(&err)) {
        *this = before;
        if (report)
            *report = err;
        return -1;
    }
    if (report) {
        QString r = QStringLiteral("已导入 %1 个,跳过 %2 个")
                        .arg(added)
                        .arg(skipped.size());
        if (!importedNames.isEmpty())
            r += QStringLiteral(";新增:") + importedNames.join(QStringLiteral(", "));
        if (!skipped.isEmpty())
            r += QStringLiteral(";跳过:") + skipped.join(QStringLiteral(", "));
        *report = r;
    }
    return added;
}
