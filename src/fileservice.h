#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

// 文件读写与用户偏好(QSettings 持久化到注册表 HKCU,不写系统其他位置)
class FileService : public QObject
{
    Q_OBJECT
public:
    explicit FileService(QObject *parent = nullptr);

    // 源文件编码:读取时嗅探,保存时按原编码写回(GBK 老文档不会被改成 UTF-8)
    enum class Encoding { Utf8, Utf8Bom, Utf16LE, Utf16BE, Gbk };

    // 读取 Markdown:BOM -> UTF-16/UTF-8;无 BOM 先严格试 UTF-8,失败回退
    // GBK/GB18030(经 Win32 代码页,与系统 ACP 是否为 UTF-8 Beta 无关)。
    // encOut 回传嗅探结果;crlfOut 回传是否 CRLF 行尾(保存时原样保留,
    // 否则 Windows 老文档一保存整篇 diff)
    static QString readFile(const QString &path, bool *ok = nullptr,
                            Encoding *encOut = nullptr, bool *crlfOut = nullptr);
    // 按指定编码写出(默认 UTF-8 无 BOM,与 Typora 一致)
    static bool writeFile(const QString &path, const QString &content,
                          Encoding enc = Encoding::Utf8);

    // 未命名文档的默认文件名:取文档开头首个非空行(跳过代码围栏与块级
    // 公式整体),剥掉 Markdown 标记(# / ** / $ / > / 列表等)。返回已清理
    // 的安全文件名;文档为空或没有可用行时返回空串(调用方回退默认名)。
    static QString titleFromMarkdown(const QString &markdown);
    // Windows 文件名清理:去非法字符/折叠空白/截断/去尾部点。幂等。
    static QString sanitizeFileName(QString name);

    static QStringList markdownFilters();

    // 最近文件(最多 10 条,读取时过滤已不存在的)
    void pushRecentFile(const QString &path);
    QStringList recentFiles() const;

    QString lastOpenedFile() const;
    void setLastOpenedFile(const QString &path);

    QString lastWorkspace() const;
    void setLastWorkspace(const QString &path);

    // 文档默认保存目录:用户可改;首次为 exe 同级 MSWriteData(自动创建)
    QString defaultSaveDir() const;
    void setDefaultSaveDir(const QString &path);

    QString lastTheme() const;
    void setLastTheme(const QString &theme);
};
