#include "fileservice.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStringConverter>
#include <QTextStream>
#include <QDateTime>

#include <algorithm>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr int kMaxRecent = 10;
constexpr qint64 kMaxDocumentBytes = 64 * 1024 * 1024;

QByteArray encodeWith(const QString &text, QStringConverter::Encoding enc)
{
    QStringEncoder encoder(enc);
    return encoder(text);
}

// 本地编码(GBK/GB18030)解码。必须走 Win32 显式代码页:
// 本机 ACP 是 65001(Windows"UTF-8 全球语言支持"Beta),QStringConverter::System
// 在这里就是 UTF-8,拿它解 GBK 只会得到一片 U+FFFD —— 老文档照旧被毁。
// 54936(GB18030)是 GBK 的超集,能同时吃下 GBK 与 GB18030 文本;
// 取不到再退 936(纯 GBK)。
QString decodeLegacy(const QByteArray &raw, bool *valid)
{
    *valid = true;
    if (raw.isEmpty())
        return {};
#ifdef Q_OS_WIN
    for (UINT cp : { 54936u, 936u }) {
        const int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS,
                                          raw.constData(), int(raw.size()), nullptr, 0);
        if (n <= 0)
            continue;
        std::wstring w(size_t(n), L'\0');
        if (MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.constData(), int(raw.size()),
                                w.data(), n) > 0)
            return QString::fromWCharArray(w.data(), n);
    }
#endif
    *valid = false;
    return {};
}

QByteArray encodeLegacy(const QString &text)
{
    if (text.isEmpty())
        return {};
#ifdef Q_OS_WIN
    // 关键:必须用 WC_NO_BEST_FIT_CHARS 询问"能否无损表示"。
    // 若按默认标志(0)询问,Win32 会把任何字符一律当成功,只是把无法表示的
    // 字符替换成 '?' 再返回长度 —— emoji/生僻字会静默毁掉,而保存照常"成功"。
    // GB18030 严格校验 UTF-16；GBK 检查 usedDefault，均拒绝静默替换。
    const auto *w = reinterpret_cast<const wchar_t *>(text.utf16());
    for (UINT cp : { 54936u, 936u }) {
        // GB18030 requires null default-character pointers; GBK needs a flag
        // preventing best-fit substitution. Win32 rejects mixing those rules.
        const DWORD flags = cp == 54936u ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
        BOOL usedDefault = FALSE;
        BOOL *used = cp == 54936u ? nullptr : &usedDefault;
        int n = WideCharToMultiByte(cp, flags, w, int(text.size()), nullptr, 0, nullptr, used);
        if (n <= 0)
            continue;
        QByteArray out(n, Qt::Uninitialized);
        if (WideCharToMultiByte(cp, flags, w, int(text.size()),
                                out.data(), n, nullptr, used) > 0
            && !usedDefault)
            return out;
        // 有字符无法表示(usedDefault)或直接失败:换下一个代码页,都不行则退回 UTF-8
    }
#endif
    return text.toUtf8();
}

// QStringEncoder 对 UTF-16/32 默认会再写一份 BOM;我们自己已经在
// writeFile 里预置了 FF FE / FE FF,再走 Encoder 就会双重 BOM,重开多一个 U+FEFF。
QByteArray encodeUtf16(const QString &text, bool bigEndian)
{
    const int n = text.size();
    QByteArray out(n * 2, Qt::Uninitialized);
    const ushort *src = text.utf16();
    auto *dst = reinterpret_cast<unsigned char *>(out.data());
    for (int i = 0; i < n; ++i) {
        const ushort u = src[i];
        if (bigEndian) {
            dst[i * 2]     = static_cast<unsigned char>(u >> 8);
            dst[i * 2 + 1] = static_cast<unsigned char>(u & 0xff);
        } else {
            dst[i * 2]     = static_cast<unsigned char>(u & 0xff);
            dst[i * 2 + 1] = static_cast<unsigned char>(u >> 8);
        }
    }
    return out;
}
} // namespace

FileService::FileService(QObject *parent)
    : QObject(parent)
{
}

bool FileService::readBytes(const QString &path, qint64 limit, QByteArray *data)
{
    if (!data) return false;
    data->clear();
    if (limit < 0 || limit > kMaxDocumentBytes || !QFileInfo(path).isFile()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit) return false;
    const QByteArray bytes = file.read(limit + 1);
    if (file.error() != QFile::NoError || bytes.size() > limit || !file.atEnd()) return false;
    *data = bytes;
    return true;
}

QString FileService::readFile(const QString &path, bool *ok, Encoding *encOut, bool *crlfOut)
{
    if (ok) *ok = false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (ok) *ok = false;
        return {};
    }
    // Refuse huge/nonregular files and incomplete reads instead of treating
    // their partial data as a successfully opened document eligible for save.
    if (!QFileInfo(path).isFile() || f.size() > kMaxDocumentBytes) return {};
    QByteArray raw = f.read(kMaxDocumentBytes + 1);
    if (f.error() != QFile::NoError || raw.size() > kMaxDocumentBytes || !f.atEnd()) return {};
    Encoding enc = Encoding::Utf8;
    QString text;

    if (raw.startsWith("\xEF\xBB\xBF")) {
        enc = Encoding::Utf8Bom;
        QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
        text = decoder(raw.mid(3));
        if (decoder.hasError()) return {};
    } else if (raw.startsWith("\xFF\xFE")) {
        if ((raw.size() - 2) % 2 != 0) return {};
        enc = Encoding::Utf16LE;
        QStringDecoder decoder(QStringConverter::Utf16LE, QStringConverter::Flag::Stateless);
        text = decoder(raw.mid(2));
        if (decoder.hasError()) return {};
    } else if (raw.startsWith("\xFE\xFF")) {
        if ((raw.size() - 2) % 2 != 0) return {};
        enc = Encoding::Utf16BE;
        QStringDecoder decoder(QStringConverter::Utf16BE, QStringConverter::Flag::Stateless);
        text = decoder(raw.mid(2));
        if (decoder.hasError()) return {};
    } else {
        // 无 BOM:先按 UTF-8 严格解码,失败说明不是 UTF-8(中文 Windows 上
        // 多为 GBK/936),回退系统 ANSI 代码页 —— 否则整个文件会变乱码,
        // 且保存时按乱码写回,原文件被永久破坏
        QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
        text = utf8(raw);
        if (utf8.hasError()) {
            enc = Encoding::Gbk;
            bool valid = false;
            text = decodeLegacy(raw, &valid);
            if (!valid) return {};
        }
    }

    if (!text.isValidUtf16()) return {};
    // Inspect decoded text: UTF-16 stores a zero byte between CR and LF.
    if (crlfOut) *crlfOut = text.contains(QStringLiteral("\r\n"));
    if (ok) *ok = true;
    if (encOut) *encOut = enc;
    return text;
}

bool FileService::writeFile(const QString &path, const QString &content, Encoding enc)
{
    if (content.size() > kMaxDocumentBytes || !content.isValidUtf16()) return false;
    QByteArray raw;
    switch (enc) {
    case Encoding::Utf8Bom:
        raw = QByteArray("\xEF\xBB\xBF", 3) + encodeWith(content, QStringConverter::Utf8);
        break;
    case Encoding::Utf16LE:
        raw = QByteArray("\xFF\xFE", 2) + encodeUtf16(content, false);
        break;
    case Encoding::Utf16BE:
        raw = QByteArray("\xFE\xFF", 2) + encodeUtf16(content, true);
        break;
    case Encoding::Gbk:
        raw = encodeLegacy(content);
        break;
    case Encoding::Utf8:
        raw = content.toUtf8();
        break;
    default:
        return false; // An invalid enum must not produce an empty successful save.
    }

    // 先写临时文件再原子替换:断电/杀进程时不会留下被截断的半成品覆盖原稿
    if (raw.size() > kMaxDocumentBytes) return false;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    if (f.write(raw) != raw.size())
        return false;
    return f.commit();
}

QStringList FileService::markdownFilters()
{
    return { QStringLiteral("Markdown 与 PDF (*.md *.markdown *.mdown *.txt *.pdf)"),
             QStringLiteral("Markdown 文档 (*.md *.markdown *.mdown *.txt)"),
             QStringLiteral("PDF 文档 (*.pdf)"),
             QStringLiteral("所有文件 (*.*)") };
}

// ---------------------------------------------------------------------------
// 默认文件名:文档开头正文 → 安全文件名
// ---------------------------------------------------------------------------

QString FileService::sanitizeFileName(QString name)
{
    // \x00 不能进 PCRE 模式串(C 字符串截断):NUL 单独删,控制字符从 \x01 起。
    // 先折叠空白(\s 含 tab/LF/CR,如 "空白\t制表" → "空白 制表"),再删残余控制符
    static const QRegularExpression blanks(QStringLiteral("\\s+"));
    static const QRegularExpression illegal(
        QStringLiteral("[\\\\/:*?\"<>|\\x01-\\x1f]"));
    name.remove(QChar(0));
    name.replace(blanks, QStringLiteral(" "));
    name.remove(illegal);
    name = name.trimmed();
    if (name.size() > 60) {
        int length = 60;
        if (name.at(length - 1).isHighSurrogate()) --length;
        name = name.left(length).trimmed();
    }
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        name.chop(1);
    static const QRegularExpression reserved(QStringLiteral(
        "^(?:CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\\.|$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name).hasMatch()) name.prepend(QLatin1Char('_'));
    return name;
}

QString FileService::titleFromMarkdown(const QString &markdown)
{
    if (markdown.isEmpty())
        return {};
    // 只看开头:标题候选不会埋在 64KB 之后;超大文档不必全文扫描
    QString prefix = markdown.left(64 * 1024);
    prefix.remove(QChar(0xfeff));
    static const QRegularExpression comments(QStringLiteral("<!--[\\s\\S]*?(?:-->|$)"));
    prefix.remove(comments);
    static const QRegularExpression frontMatter(QStringLiteral(
        "^---[ \\t]*\\r?\\n[\\s\\S]*?\\n(?:---|\\.\\.\\.)[ \\t]*(?:\\r?\\n|$)"));
    prefix.remove(frontMatter);
    const QStringList lines = prefix.split(QRegularExpression(QStringLiteral("\\r\\n|[\\r\\n]")));

    // 行首块级标记:可重复组合(# 标题、> 引用、- 列表、1. 有序、[x] 任务)
    static const QRegularExpression lead(QStringLiteral(
        "^(?:#{1,6}\\s+|>\\s?|[-*+]\\s+|\\d{1,9}[.)]\\s+|\\[[ xX]\\]\\s+)+"));
    // 行内标记(顺序重要:先链接后 HTML,先定界符后通配)
    static const QRegularExpression image(
        QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression link(
        QStringLiteral("\\[([^\\]]*)\\]\\([^)]*\\)"));
    static const QRegularExpression htmlTag(QStringLiteral("<[^>]*>"));
    static const QRegularExpression boldUnder(QStringLiteral("__([^_]+)__"));
    static const QRegularExpression italUnder(QStringLiteral("_([^_\\s]+)_"));
    static const QRegularExpression displayMath(QStringLiteral("\\$\\$([^$]+)\\$\\$"));
    static const QRegularExpression inlineMath(QStringLiteral("\\$([^$]+)\\$"));
    static const QRegularExpression closingHeading(QStringLiteral("\\s+#+\\s*$"));
    static const QRegularExpression markersOnly(QStringLiteral("^[\\s#*_~`$>+\\-=]+$"));
    static const QRegularExpression referenceLink(QStringLiteral("!?\\[([^\\]]+)\\]\\[[^\\]]*\\]"));

    bool inFence = false;
    QChar fenceChar;
    int fenceLength = 0;
    static const QRegularExpression fenceOpen(QStringLiteral("^(`{3,}|~{3,})"));

    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (inFence) {
            // 闭合:行首同类字符长度不小于开围栏,且之后无其他内容
            int len = 0;
            while (len < line.size() && line.at(len) == fenceChar)
                ++len;
            if (len >= fenceLength && line.mid(len).trimmed().isEmpty())
                inFence = false;
            continue;
        }
        const auto fence = fenceOpen.match(line);
        if (fence.hasMatch()) {
            inFence = true;
            fenceChar = line.at(0);
            fenceLength = fence.captured(1).size();
            continue;
        }
        if (line == QLatin1String("$$")) {
            continue;
        }
        if (line.isEmpty() || markersOnly.match(line).hasMatch())
            continue;

        QString t = line;
        t.remove(lead);
        if (line.startsWith(QLatin1Char('#'))) t.remove(closingHeading);
        t.replace(image, QStringLiteral("\\1"));
        t.replace(link, QStringLiteral("\\1"));
        t.replace(referenceLink, QStringLiteral("\\1"));
        t.remove(htmlTag);
        t.replace(displayMath, QStringLiteral("\\1"));
        t.replace(inlineMath, QStringLiteral("\\1"));
        t.replace(boldUnder, QStringLiteral("\\1"));
        t.replace(italUnder, QStringLiteral("\\1"));
        t.remove(QLatin1Char('*'));
        t.remove(QLatin1Char('~'));
        t.remove(QLatin1Char('`'));
        t.remove(QLatin1Char('$'));
        t.remove(QChar(0x200b));
        const QString name = sanitizeFileName(t);
        if (!name.isEmpty())
            return name;
        // 整行剥完为空(纯标记行/空公式):继续找下一行
    }
    return {};
}

void FileService::pushRecentFile(const QString &path)
{
    QSettings s;
    QStringList files = s.value(QStringLiteral("recentFiles")).toStringList();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > kMaxRecent)
        files.removeLast();
    s.setValue(QStringLiteral("recentFiles"), files);
}

QStringList FileService::recentFiles() const
{
    QSettings s;
    QStringList files = s.value(QStringLiteral("recentFiles")).toStringList();
    // 过滤已删除的文件
    files.erase(std::remove_if(files.begin(), files.end(),
                               [](const QString &p) { return !QFileInfo::exists(p); }),
                files.end());
    return files;
}

QString FileService::lastOpenedFile() const
{
    return QSettings().value(QStringLiteral("lastFile")).toString();
}

void FileService::setLastOpenedFile(const QString &path)
{
    QSettings().setValue(QStringLiteral("lastFile"), path);
}

QString FileService::lastWorkspace() const
{
    return QSettings().value(QStringLiteral("workspace")).toString();
}

void FileService::setLastWorkspace(const QString &path)
{
    QSettings().setValue(QStringLiteral("workspace"), path);
}

// 文档默认保存目录:用户改过的用所选;否则 exe 同级 MSWriteData(首次自动创建)
QString FileService::defaultSaveDir() const
{
    QString dir = QSettings().value(QStringLiteral("saveDir")).toString();
    if (!dir.isEmpty() && QDir(dir).mkpath(QStringLiteral(".")))
        return QDir::fromNativeSeparators(dir);
    // 默认:exe 旁的 MSWriteData
    dir = QCoreApplication::applicationDirPath() + QStringLiteral("/MSWriteData");
    QDir().mkpath(dir);
    return dir;
}

void FileService::setDefaultSaveDir(const QString &path)
{
    QSettings().setValue(QStringLiteral("saveDir"), path);
}

QString FileService::lastTheme() const
{
    // 空 = 用户从未手动选过主题,由调用方按系统深浅决定
    return QSettings().value(QStringLiteral("theme")).toString();
}

void FileService::setLastTheme(const QString &theme)
{
    QSettings().setValue(QStringLiteral("theme"), theme);
}
