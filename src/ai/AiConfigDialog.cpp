// ai/AiConfigDialog.cpp — see ai/AiConfigDialog.h.
// 布局参考 MS-Agent 的 ConfigWindow(左列表 + 右表单),配色跟随打开它的窗口,
// 与 Mswrite 其它对话框一致。
// 右表单 = 供应商(名称/地址/Key/协议) + 模型列表(一个网址挂多个模型);
// 每个模型有独立的编辑弹窗(AiModelEditDialog,含思考档位自定义)。

#include "ai/AiConfigDialog.h"
#include <limits>
#include "ai/LlmCodec.h" // kDefaultMaxTokens

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSize>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>
#include <functional>
#include <windows.h>

namespace {

QString configQss(const QString &theme)
{
    const bool dark = theme == QLatin1String("dark");
    return QStringLiteral(R"(
QDialog {
  background: %1; color: %2;
  font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif;
  font-size: 13px;
}
QLabel, QCheckBox { color: %2; background: transparent; }
QLabel#brand { color: %2; font-size: 17px; font-weight: 700; }
QLabel#sub, QLabel#hint, QLabel#from { color: %5; font-size: 12px; }
QLabel#badge { color: %5; font-size: 11px; border: 1px solid %4; border-radius: 7px; padding: 1px 7px; }
QLineEdit, QComboBox, QSpinBox {
  background: %3; color: %2;
  border: 1px solid %4; border-radius: 8px; padding: 7px 10px;
  selection-background-color: #2563eb;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid #3b82f6; }
QComboBox:editable { background: %3; }
QComboBox QAbstractItemView {
  background: %3; color: %2;
  border: 1px solid %4; selection-background-color: #2563eb; selection-color:white;
}
QSpinBox::up-button, QSpinBox::down-button { width: 0; border: none; }
QListWidget {
  background: %3; color: %2;
  border: 1px solid %4; border-radius: 10px; padding: 4px; outline: none;
}
QListWidget::item { padding: 8px 10px; border-radius: 8px; margin: 1px; color: %2; }
QListWidget::item:selected { background: #2563eb; color: #ffffff; }
QListWidget::item:hover:!selected { background: %6; }
QListWidget#modelRows::item { padding: 2px 6px; }
QListWidget#modelRows::item:selected { background: %6; color: %2; }
QPushButton {
  background: %3; color: %2;
  border: 1px solid %4; border-radius: 8px; padding: 7px 14px;
}
QPushButton:hover, QPushButton:pressed { background: %6; }
QPushButton#primary { background: #2563eb; border: 1px solid #1d4ed8; color: #fff; font-weight: 600; }
QPushButton#primary:hover { background: #1d4ed8; }
QPushButton#danger { color: %7; border: 1px solid %4; }
QPushButton#danger:hover { background: %6; border-color:%7; }
QPushButton#mini { padding: 3px 9px; font-size: 12px; }
/* 开关:勾选框 + 绿色对勾(用户要求:蓝色块看不懂,改成"框里打勾") */
QCheckBox#switch { spacing: 6; }
QCheckBox#switch::indicator { width: 17px; height: 17px; border-radius: 4px; background: %3; border: 1px solid %4; }
QCheckBox#switch::indicator:checked { image: url(:/icons/check-green.svg); border-color: #22b14c; }
QCheckBox#switch::indicator:hover { border-color: #22b14c; }
/* 普通复选框:默认主题在浅底上几乎看不见指示框,显式画一个 */
QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid %4; border-radius: 4px; background: %3; }
QCheckBox::indicator:checked { image: url(:/icons/check-green.svg); border-color: #22b14c; }
QCheckBox::indicator:hover { border-color: #22b14c; }
/* 推理等级 chips */
QPushButton#chip { border-radius: 12px; padding: 4px 12px; font-size: 12px; background: %6; border: 1px solid %4; }
QPushButton#chip:hover { border-color: %7; color: %7; }
QSplitter::handle { background: %4; }
QScrollBar:vertical { background: transparent; width: 10px; }
QScrollBar::handle:vertical { background: %4; border-radius: 5px; min-height: 36px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
)").arg(dark ? "#0d0d11" : theme == QLatin1String("paper") ? "#fffdf8" : "#f6f7fa",
        dark ? "#e4e4e7" : "#253041", dark ? "#16161b" : "#ffffff",
        dark ? "#33333c" : "#dce1e8", dark ? "#a1a1aa" : "#65738a",
        dark ? "#24242c" : "#edf2fa", dark ? "#f87171" : "#b42333");
}

// 上下文窗口显示徽标:1048576 → 1M,131072 → 128k
QString ctxBadge(int tokens)
{
    if (tokens <= 0)
        return QString();
    if (tokens % 1048576 == 0)
        return QStringLiteral("%1M").arg(tokens / 1048576);
    if (tokens % 1024 == 0)
        return QStringLiteral("%1k").arg(tokens / 1024);
    return QString::number(tokens);
}

// 智能配置的推荐值:从模型 id 的 [xM]/[xk] 后缀推断上下文,推不出就空
int smartContextWindow(const QString &id)
{
    const int l = id.indexOf(QLatin1Char('[')), r = id.indexOf(QLatin1Char(']'), l + 1);
    if (l < 0 || r < 0)
        return 0;
    const QString inside = id.mid(l + 1, r - l - 1).trimmed();
    const qint64 unit = inside.endsWith(QLatin1Char('M'), Qt::CaseInsensitive) ? 1048576
                      : inside.endsWith(QLatin1Char('k'), Qt::CaseInsensitive) ? 1024 : 0;
    if (!unit) return 0;
    bool ok = false;
    const qint64 amount = inside.left(inside.size() - 1).toLongLong(&ok);
    if (!ok || amount <= 0 || amount > std::numeric_limits<int>::max() / unit) return 0;
    return int(amount * unit);
}

} // namespace

// ---------------------------------------------------------------------------
// 单个模型的编辑弹窗(结构对齐 cc-switch 的"编辑模型配置")
// ---------------------------------------------------------------------------

AiModelEditDialog::AiModelEditDialog(QWidget *parent, const QString &theme,
                                     AiModelCfg &cfg, bool isNew)
    : QDialog(parent), cfg_(cfg), dark_(theme == QLatin1String("dark"))
{
    setWindowTitle(isNew ? tr("添加模型") : tr("编辑模型配置"));
    setModal(true);
    setMinimumWidth(480);
    setStyleSheet(configQss(theme));
    using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    static auto setAttribute = dwm ? reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
    if (setAttribute) {
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        const BOOL useDark = dark_;
        setAttribute(hwnd, 20, &useDark, sizeof(useDark));
    }

    // 思考档位的编辑态副本(成员:保存/chips 的 lambda 在 exec() 期间执行,
    // 必须是成员,局部变量在 ctor 返回后就是悬空引用 —— 点保存即崩)
    m_thinkLevels = cfg_.thinkLevels;

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(12);

    // 智能配置:开 = 推荐值且锁定手改
    auto *smartRow = new QHBoxLayout;
    auto *smartLabel = new QLabel(tr("智能配置"), this);
    smart_ = new QCheckBox(this);
    smart_->setObjectName(QStringLiteral("switch"));
    smart_->setToolTip(tr("开:上下文窗口按模型名后缀([1M]/[128k])推断,回复上限用默认值;\n关:下面的数值全部可手动改"));
    smartRow->addWidget(smartLabel);
    smartRow->addStretch();
    smartRow->addWidget(smart_);
    lay->addLayout(smartRow);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);

    idEdit_ = new QLineEdit(this);
    idEdit_->setObjectName(QStringLiteral("mId"));
    idEdit_->setText(cfg_.id);
    idEdit_->setPlaceholderText(tr("glm-5.3 / glm-5.3[1M] / deepseek-flash"));
    form->addRow(tr("模型 ID"), idEdit_);

    ctxSpin_ = new QSpinBox(this);
    ctxSpin_->setRange(0, 100000000);
    ctxSpin_->setSingleStep(1024);
    ctxSpin_->setSpecialValueText(tr("未设置"));
    form->addRow(tr("上下文窗口"), ctxSpin_);

    outSpin_ = new QSpinBox(this);
    outSpin_->setRange(0, 100000000);
    outSpin_->setSingleStep(1024);
    outSpin_->setSpecialValueText(tr("默认 %1").arg(LlmCodec::kDefaultMaxTokens));
    outSpin_->setToolTip(tr("单次回复上限;0 = 默认 %1").arg(LlmCodec::kDefaultMaxTokens));
    form->addRow(tr("最大输出 Token"), outSpin_);
    lay->addLayout(form);

    auto *advTitle = new QLabel(tr("高级配置"), this);
    advTitle->setObjectName(QStringLiteral("sub"));
    lay->addWidget(advTitle);

    // 输入类型(文本恒定可用,锁定)
    auto *inText = new QCheckBox(tr("文本"), this);
    inText->setChecked(true);
    inText->setEnabled(false);
    inImage_ = new QCheckBox(tr("图片"), this);
    inVideo_ = new QCheckBox(tr("视频"), this);
    inPdf_ = new QCheckBox(tr("PDF"), this);
    inImage_->setChecked(cfg_.inImage);
    inVideo_->setChecked(cfg_.inVideo);
    inPdf_->setChecked(cfg_.inPdf);
    lay->addWidget(new QLabel(tr("输入类型"), this));
    {
        auto *row = new QHBoxLayout;
        row->addWidget(inText); row->addWidget(inImage_); row->addWidget(inVideo_);
        row->addWidget(inPdf_); row->addStretch();
        lay->addLayout(row);
    }

    capStructured_ = new QCheckBox(tr("结构化输出"), this);
    capSearch_ = new QCheckBox(tr("原生联网搜索"), this);
    capSystem_ = new QCheckBox(tr("对话中系统消息"), this);
    capStructured_->setChecked(cfg_.capStructured);
    capSearch_->setChecked(cfg_.capSearch);
    capSystem_->setChecked(cfg_.capSystem);
    lay->addWidget(new QLabel(tr("模型能力"), this));
    {
        auto *row = new QHBoxLayout;
        row->addWidget(capStructured_); row->addWidget(capSearch_); row->addWidget(capSystem_);
        row->addStretch();
        lay->addLayout(row);
    }

    // 推理等级(从低到高):chips(点 chip 移除) + "+" 手动输入添加
    lay->addWidget(new QLabel(tr("推理等级(从低到高)"), this));
    auto *chipsWrap = new QWidget(this);
    chipsLay_ = new QVBoxLayout(chipsWrap);
    chipsLay_->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(chipsWrap);
    rebuildChips();

    auto *actions = new QHBoxLayout;
    auto *resetBtn = new QPushButton(tr("重置表单"), this);
    auto *cancelBtn = new QPushButton(tr("取消"), this);
    auto *saveBtn = new QPushButton(tr("保存"), this);
    saveBtn->setObjectName(QStringLiteral("primary"));
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        // 重置 = 回到打开时的值
        idEdit_->setText(cfg_.id);
        smart_->setChecked(cfg_.smartConfig);
        ctxSpin_->setValue(cfg_.smartConfig ? smartContextWindow(cfg_.id) : cfg_.contextWindow);
        outSpin_->setValue(cfg_.maxOutput);
        inImage_->setChecked(cfg_.inImage); inVideo_->setChecked(cfg_.inVideo); inPdf_->setChecked(cfg_.inPdf);
        capStructured_->setChecked(cfg_.capStructured); capSearch_->setChecked(cfg_.capSearch);
        capSystem_->setChecked(cfg_.capSystem);
        m_thinkLevels = cfg_.thinkLevels;
        rebuildChips();
    });
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        const QString id = idEdit_->text().trimmed();
        if (id.isEmpty()) {
            QMessageBox::warning(this, tr("编辑模型配置"), tr("模型 ID 不能为空"));
            return;
        }
        cfg_.id = id;
        cfg_.smartConfig = smart_->isChecked();
        cfg_.contextWindow = cfg_.smartConfig ? smartContextWindow(id) : ctxSpin_->value();
        cfg_.maxOutput = cfg_.smartConfig ? 0 : outSpin_->value();
        cfg_.inImage = inImage_->isChecked();
        cfg_.inVideo = inVideo_->isChecked();
        cfg_.inPdf = inPdf_->isChecked();
        cfg_.capStructured = capStructured_->isChecked();
        cfg_.capSearch = capSearch_->isChecked();
        cfg_.capSystem = capSystem_->isChecked();
        cfg_.thinkLevels = m_thinkLevels;
        if (cfg_.thinkLevels.isEmpty())
            cfg_.thinkLevels = { QStringLiteral("low"), QStringLiteral("high") };
        accept();
    });
    actions->addWidget(resetBtn);
    actions->addStretch();
    actions->addWidget(cancelBtn);
    actions->addWidget(saveBtn);
    lay->addLayout(actions);

    // 智能配置开关:锁定数值区
    smart_->setChecked(cfg_.smartConfig);
    auto applySmart = [this](bool on) {
        ctxSpin_->setEnabled(!on);
        outSpin_->setEnabled(!on);
        if (on) {
            ctxSpin_->setValue(smartContextWindow(idEdit_->text().trimmed()));
            outSpin_->setValue(0);
        }
    };
    connect(smart_, &QCheckBox::toggled, this, applySmart);
    connect(idEdit_, &QLineEdit::textChanged, this, [this] {
        if (smart_->isChecked())
            ctxSpin_->setValue(smartContextWindow(idEdit_->text().trimmed()));
    });
    applySmart(cfg_.smartConfig);
    if (!cfg_.smartConfig) {
        ctxSpin_->setValue(cfg_.contextWindow);
        outSpin_->setValue(cfg_.maxOutput);
    }
}

bool AiModelEditDialog::edit(QWidget *parent, const QString &theme, AiModelCfg &cfg, bool isNew)
{
    AiModelEditDialog dlg(parent, theme, cfg, isNew);
    return dlg.exec() == QDialog::Accepted;
}

// 推理等级 chips 重建:成员函数,生命周期随对话框(不能用构造器局部
// 变量 —— 那在 exec() 期间早已销毁,点保存就是悬空引用崩溃)
void AiModelEditDialog::rebuildChips()
{
    while (QLayoutItem *it = chipsLay_->takeAt(0)) {
        if (it->widget())
            it->widget()->deleteLater();
        delete it;
    }
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    for (const QString &lv : m_thinkLevels) {
        auto *chip = new QPushButton(lv, this);
        chip->setObjectName(QStringLiteral("chip"));
        chip->setToolTip(tr("点击移除该档位"));
        connect(chip, &QPushButton::clicked, this, [this, lv] {
            m_thinkLevels.removeAll(lv);
            rebuildChips();
        });
        row->addWidget(chip);
    }
    auto *add = new QPushButton(QStringLiteral("+"), this);
    add->setObjectName(QStringLiteral("chip"));
    add->setToolTip(tr("添加自定义档位(手动输入,如 max)"));
    connect(add, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString lv = QInputDialog::getText(this, tr("添加推理等级"),
                                                 tr("档位名(如 max / xhigh):"),
                                                 QLineEdit::Normal, QString(), &ok)
                               .trimmed();
        if (ok && !lv.isEmpty() && !m_thinkLevels.contains(lv)) {
            m_thinkLevels.append(lv);
            rebuildChips();
        }
    });
    row->addWidget(add);
    row->addStretch();
    chipsLay_->addLayout(row);
}

// ---------------------------------------------------------------------------
// 供应商配置窗口
// ---------------------------------------------------------------------------

AiConfigDialog::AiConfigDialog(AiProviderStore *store, QWidget *parent, const QString &theme)
    : QDialog(parent)
    , store_(store)
    , dark_(theme == QLatin1String("dark"))
{
    setWindowTitle(tr("AI 供应商设置"));
    setMinimumSize(QSize(760, 480));
    resize(900, 620);
    setObjectName(QStringLiteral("aiConfigDialog"));
    QPalette p = palette();
    const QColor bg(dark_ ? "#0d0d11" : theme == QLatin1String("paper") ? "#fffdf8" : "#f6f7fa");
    const QColor fg(dark_ ? "#e4e4e7" : "#253041");
    p.setColor(QPalette::Window, bg); p.setColor(QPalette::WindowText, fg);
    p.setColor(QPalette::Base, QColor(dark_ ? "#16161b" : "#ffffff"));
    p.setColor(QPalette::Text, fg); p.setColor(QPalette::ButtonText, fg);
    p.setColor(QPalette::PlaceholderText, QColor(dark_ ? "#a1a1aa" : "#65738a"));
    setPalette(p);
    setStyleSheet(configQss(theme));
    using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    static auto setAttribute = dwm ? reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
    if (setAttribute) {
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        const BOOL useDark = dark_;
        const COLORREF caption = RGB(bg.red(), bg.green(), bg.blue()), ink = RGB(fg.red(), fg.green(), fg.blue());
        setAttribute(hwnd, 20, &useDark, sizeof(useDark));
        setAttribute(hwnd, 35, &caption, sizeof(caption)); setAttribute(hwnd, 36, &ink, sizeof(ink));
    }

    auto *split = new QSplitter(Qt::Horizontal, this);

    // ── 左:供应商列表 ──
    auto *left = new QWidget;
    left->setMinimumWidth(240);
    left->setMaximumWidth(360);
    auto *leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(14, 14, 14, 14);
    leftLay->setSpacing(8);

    auto *brand = new QLabel(tr("AI 供应商"), left);
    brand->setObjectName(QStringLiteral("brand"));
    auto *sub = new QLabel(tr("只作用于 Mswrite 的 AI 助手"), left);
    sub->setObjectName(QStringLiteral("sub"));
    sub->setWordWrap(true);
    leftLay->addWidget(brand);
    leftLay->addWidget(sub);

    list_ = new QListWidget(left);
    list_->setWordWrap(true);
    connect(list_, &QListWidget::currentItemChanged, this, &AiConfigDialog::onSelect);
    leftLay->addWidget(list_, 1);

    auto *leftBtns = new QHBoxLayout;
    auto *addBtn = new QPushButton(tr("添加"), left);
    auto *importBtn = new QPushButton(tr("导入 MS-Agent"), left);
    auto *ccBtn = new QPushButton(tr("导入 cc-switch"), left);
    connect(addBtn, &QPushButton::clicked, this, &AiConfigDialog::onAdd);
    connect(importBtn, &QPushButton::clicked, this, &AiConfigDialog::onImportMsAgent);
    connect(ccBtn, &QPushButton::clicked, this, &AiConfigDialog::onImportCcSwitch);
    leftBtns->addWidget(addBtn);
    leftBtns->addWidget(importBtn);
    leftBtns->addWidget(ccBtn);
    leftLay->addLayout(leftBtns);

    auto *autoImport = new QCheckBox(tr("启动时自动导入 cc-switch(含 Key)"), left);
    autoImport->setObjectName(QStringLiteral("autoImportCcSwitch"));
    autoImport->setChecked(store_->autoImportCcSwitch());
    autoImport->setToolTip(tr("同步 cc-switch 的供应商配置,并跟随所选来源的当前供应商。\n"
                            "手动添加的供应商保留;删除的导入项可通过导入按钮恢复。"));
    connect(autoImport, &QCheckBox::toggled, this, [this, autoImport](bool enabled) {
        const QString error = store_->setAutoImportCcSwitch(enabled);
        if (!error.isEmpty()) {
            const QSignalBlocker blocker(autoImport);
            autoImport->setChecked(store_->autoImportCcSwitch());
            setStatus(error, true);
            return;
        }
        if (enabled) {
            QString report;
            const int n = store_->importFromCcSwitch(&report, true);
            rebuildList(store_->currentName());
            setStatus(report, n < 0);
        } else {
            setStatus(tr("已关闭自动导入,已有配置保留"));
        }
    });
    leftLay->addWidget(autoImport);
    auto *syncSource = new QComboBox(left);
    syncSource->setObjectName(QStringLiteral("ccSwitchApp"));
    syncSource->addItem(tr("跟随 Claude 当前供应商"), QStringLiteral("claude"));
    syncSource->addItem(tr("跟随 Codex 当前供应商"), QStringLiteral("codex"));
    syncSource->addItem(tr("跟随 Gemini 当前供应商"), QStringLiteral("gemini"));
    syncSource->setCurrentIndex(syncSource->findData(store_->ccSwitchApp()));
    connect(syncSource, &QComboBox::currentIndexChanged, this, [this, syncSource] {
        const QString error = store_->setCcSwitchApp(syncSource->currentData().toString());
        if (!error.isEmpty()) {
            const QSignalBlocker blocker(syncSource);
            syncSource->setCurrentIndex(syncSource->findData(store_->ccSwitchApp()));
            setStatus(error, true); return;
        }
        QString report;
        const int n = store_->importFromCcSwitch(&report, true);
        rebuildList(store_->currentName()); setStatus(report, n < 0);
    });
    leftLay->addWidget(syncSource);

    auto *pathHint = new QLabel(tr("保存到 %1").arg(
        QDir::toNativeSeparators(AiProviderStore::defaultFilePath())), left);
    pathHint->setObjectName(QStringLiteral("hint"));
    pathHint->setWordWrap(true);
    leftLay->addWidget(pathHint);

    // ── 右:供应商表单 + 模型列表 ──
    auto *right = new QWidget;
    auto *rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(24, 18, 24, 16);
    rightLay->setSpacing(12);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);

    nameEdit_ = new QLineEdit;
    nameEdit_->setObjectName(QStringLiteral("cfgName"));
    urlEdit_ = new QLineEdit;
    urlEdit_->setObjectName(QStringLiteral("cfgUrl"));
    urlEdit_->setPlaceholderText(tr("https://open.bigmodel.cn/api/anthropic"));
    protocolCombo_ = new QComboBox;
    protocolCombo_->addItem(tr("Anthropic Messages"), QStringLiteral("anthropic"));
    protocolCombo_->addItem(tr("OpenAI Chat Completions"), QStringLiteral("openai"));
    protocolCombo_->addItem(tr("OpenAI Responses"), QStringLiteral("openai-responses"));
    protocolCombo_->addItem(tr("Gemini"), QStringLiteral("gemini"));
    protocolCombo_->addItem(tr("不支持的来源协议"), QStringLiteral("unsupported"));

    auto *keyRow = new QWidget;
    auto *keyLay = new QHBoxLayout(keyRow);
    keyLay->setContentsMargins(0, 0, 0, 0);
    keyLay->setSpacing(6);
    keyEdit_ = new QLineEdit;
    keyEdit_->setObjectName(QStringLiteral("cfgKey"));
    keyEdit_->setEchoMode(QLineEdit::Password);
    keyEdit_->setPlaceholderText(tr("API Key"));
    keyToggle_ = new QPushButton(tr("显示"));
    keyToggle_->setFixedWidth(56);
    connect(keyToggle_, &QPushButton::clicked, this, &AiConfigDialog::onToggleKey);
    keyLay->addWidget(keyEdit_, 1);
    keyLay->addWidget(keyToggle_);

    form->addRow(tr("名称"), nameEdit_);
    form->addRow(tr("API 地址"), urlEdit_);
    form->addRow(tr("API 格式"), protocolCombo_);
    form->addRow(tr("API Key"), keyRow);
    rightLay->addLayout(form);

    // 模型列表(一个网址可挂多个模型):行 = id + 徽标 + 启用开关 + ✎
    auto *modelHead = new QHBoxLayout;
    auto *modelTitle = new QLabel(tr("模型列表"), right);
    auto *addModel = new QPushButton(tr("+ 添加模型"), right);
    addModel->setObjectName(QStringLiteral("mini"));
    connect(addModel, &QPushButton::clicked, this, [this] {
        AiModelCfg cfg;
        cfg.id.clear();
        if (!AiModelEditDialog::edit(this, dark_ ? QStringLiteral("dark") : QStringLiteral("light"), cfg, true))
            return;
        for (const AiModelCfg &m : editingModels_) {
            if (m.id == cfg.id) {
                setStatus(tr("模型已存在:%1").arg(cfg.id), true);
                return;
            }
        }
        editingModels_.append(cfg);
        if (editingCurrentModel_.isEmpty())
            editingCurrentModel_ = cfg.id;
        rebuildModelRows();
    });
    modelHead->addWidget(modelTitle);
    modelHead->addStretch();
    modelHead->addWidget(addModel);
    rightLay->addLayout(modelHead);

    modelList_ = new QListWidget(right);
    modelList_->setObjectName(QStringLiteral("modelRows"));
    modelList_->setMinimumHeight(150);
    modelList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    rightLay->addWidget(modelList_, 1);

    fromLabel_ = new QLabel(right);
    fromLabel_->setObjectName(QStringLiteral("from"));
    fromLabel_->setWordWrap(true);
    rightLay->addWidget(fromLabel_);

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("hint"));
    status_->setWordWrap(true);
    rightLay->addWidget(status_);

    auto *actions = new QHBoxLayout;
    auto *currentBtn = new QPushButton(tr("设为当前"));
    currentBtn->setObjectName(QStringLiteral("primary"));
    currentBtn->setDefault(true);
    auto *saveBtn = new QPushButton(tr("保存"));
    auto *delBtn = new QPushButton(tr("删除"));
    delBtn->setObjectName(QStringLiteral("danger"));
    auto *closeBtn = new QPushButton(tr("完成"));
    connect(currentBtn, &QPushButton::clicked, this, &AiConfigDialog::onSetCurrent);
    connect(saveBtn, &QPushButton::clicked, this, &AiConfigDialog::onSave);
    connect(delBtn, &QPushButton::clicked, this, &AiConfigDialog::onRemove);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    actions->addWidget(currentBtn);
    actions->addWidget(saveBtn);
    actions->addWidget(delBtn);
    actions->addStretch();
    actions->addWidget(closeBtn);
    rightLay->addLayout(actions);

    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(split);

    rebuildList(store_->currentName());
    if (list_->count() == 0)
        clearEditor(true);
}

void AiConfigDialog::setStatus(const QString &text, bool error)
{
    status_->setText(text);
    status_->setStyleSheet(QStringLiteral("color:%1;").arg(error
        ? (dark_ ? "#f87171" : "#b42333") : (dark_ ? "#34d399" : "#087f5b")));
}

QString AiConfigDialog::selectedName() const
{
    auto *item = list_->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void AiConfigDialog::rebuildList(const QString &selectName)
{
    const QString want = selectName.isEmpty() ? selectedName() : selectName;
    list_->blockSignals(true);
    list_->clear();
    const QString current = store_->currentName();
    for (const AiProvider &p : store_->all()) {
        const bool on = p.name.compare(current, Qt::CaseInsensitive) == 0;
        auto *item = new QListWidgetItem;
        item->setData(Qt::UserRole, p.name);
        const QString state = p.protocol == Protocol::Unsupported ? tr("来源协议不支持")
                            : p.apiKey.trimmed().isEmpty() ? tr("未配置 API Key")
                            : p.effectiveModelId().trimmed().isEmpty() ? tr("未配置模型")
                            : p.baseUrl.trimmed().isEmpty() ? tr("未配置 API 地址")
                            : p.importedFrom.startsWith(QLatin1String("cc-switch:")) ? tr("来自 cc-switch")
                            : tr("本机配置");
        item->setText(QStringLiteral("%1%2\n%3 · %4\n%5")
                          .arg(on ? QStringLiteral("● ") : QStringLiteral("   "),
                               p.name,
                               p.effectiveModelId(),
                               protocolName(p.protocol), state));
        item->setSizeHint(QSize(0, 70));
        item->setToolTip(p.baseUrl);
        list_->addItem(item);
        if (p.name.compare(want, Qt::CaseInsensitive) == 0)
            list_->setCurrentItem(item);
    }
    list_->blockSignals(false);
    if (!list_->currentItem() && list_->count() > 0)
        list_->setCurrentRow(0);
    // 关键:blockSignals 期间 setCurrentItem 不触发 currentItemChanged,
    // 表单不会自己加载 —— 必须手动把选中项载入表单(否则 Key 显示为空,
    // 用户以为配置丢了,切走再切回来才"恢复")
    if (list_->currentItem())
        loadEditor(selectedName());
}

// 模型列表行:模型 id + 上下文/视觉徽标 + 启用开关 + ✎ 编辑
void AiConfigDialog::rebuildModelRows()
{
    const quint64 generation = ++modelRowsGeneration_;
    modelList_->clear();
    for (int i = 0; i < editingModels_.size(); ++i) {
        const AiModelCfg &m = editingModels_.at(i);
        auto *item = new QListWidgetItem;
        auto *row = new QWidget;
        row->setAttribute(Qt::WA_TranslucentBackground);
        auto *lay = new QHBoxLayout(row);
        lay->setContentsMargins(4, 2, 4, 2);
        lay->setSpacing(8);

        auto *name = new QLabel(m.id, row);
        lay->addWidget(name, 1);
        const QString ctx = ctxBadge(m.contextWindow);
        if (!ctx.isEmpty()) {
            auto *b = new QLabel(ctx, row);
            b->setObjectName(QStringLiteral("badge"));
            lay->addWidget(b);
        }
        if (m.inImage) {
            auto *b = new QLabel(tr("视觉"), row);
            b->setObjectName(QStringLiteral("badge"));
            lay->addWidget(b);
        }
        if (m.id == editingCurrentModel_) {
            auto *b = new QLabel(tr("当前"), row);
            b->setObjectName(QStringLiteral("badge"));
            lay->addWidget(b);
        }

        auto *edit = new QPushButton(QStringLiteral("✎"), row);
        edit->setObjectName(QStringLiteral("mini"));
        edit->setToolTip(tr("编辑该模型(上下文/回复上限/思考档位等)"));
        connect(edit, &QPushButton::clicked, this, [this, i, generation] {
            if (generation != modelRowsGeneration_ || i >= editingModels_.size()) return;
            AiModelCfg cfg = editingModels_.at(i);
            const QString oldId = cfg.id;
            if (!AiModelEditDialog::edit(this, dark_ ? QStringLiteral("dark") : QStringLiteral("light"), cfg, false))
                return;
            if (generation != modelRowsGeneration_ || i >= editingModels_.size()) return;
            for (int j = 0; j < editingModels_.size(); ++j) {
                if (j != i && editingModels_.at(j).id == cfg.id) {
                    setStatus(tr("模型已存在:%1").arg(cfg.id), true);
                    return;
                }
            }
            editingModels_[i] = cfg;
            if (editingCurrentModel_ == oldId)
                editingCurrentModel_ = cfg.id;
            rebuildModelRows();
        });
        lay->addWidget(edit);

        auto *sw = new QCheckBox(row);
        sw->setObjectName(QStringLiteral("switch"));
        sw->setChecked(m.enabled);
        sw->setToolTip(tr("启用/停用该模型(停用后不出现在切换列表)"));
        connect(sw, &QCheckBox::toggled, this, [this, i, generation](bool on) {
            if (generation != modelRowsGeneration_ || i >= editingModels_.size()) return;
            editingModels_[i].enabled = on;
            rebuildModelRows();
        });
        lay->addWidget(sw);

        item->setSizeHint(row->sizeHint());
        modelList_->addItem(item);
        modelList_->setItemWidget(item, row);
    }
}

void AiConfigDialog::loadEditor(const QString &name)
{
    const AiProvider *p = store_->find(name);
    if (!p) {
        clearEditor(true);
        return;
    }
    creating_ = false;
    editingName_ = p->name;
    nameEdit_->setText(p->name);
    const int idx = protocolCombo_->findData(protocolName(p->protocol));
    protocolCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
    urlEdit_->setText(p->baseUrl);
    keyEdit_->setText(p->apiKey);
    keyEdit_->setEchoMode(QLineEdit::Password);
    keyToggle_->setText(tr("显示"));
    editingModels_ = p->models;
    editingCurrentModel_ = p->currentModel;
    rebuildModelRows();
    fromLabel_->setText(p->importedFrom.isEmpty()
                            ? tr("来源:手动添加")
                            : tr("来源:%1(副本,不影响原配置)").arg(p->importedFrom));
    setStatus(QString());
}

void AiConfigDialog::clearEditor(bool forNew)
{
    creating_ = forNew;
    editingName_.clear();
    nameEdit_->clear();
    protocolCombo_->setCurrentIndex(0);
    urlEdit_->clear();
    keyEdit_->clear();
    keyEdit_->setEchoMode(QLineEdit::Password);
    keyToggle_->setText(tr("显示"));
    editingModels_.clear();
    editingCurrentModel_.clear();
    rebuildModelRows();
    if (fromLabel_)
        fromLabel_->setText(forNew ? tr("新建供应商") : QString());
    setStatus(forNew ? tr("填写上方表单,并添加至少一个模型") : QString());
    if (forNew)
        nameEdit_->setFocus();
}

void AiConfigDialog::onSelect()
{
    const QString name = selectedName();
    if (name.isEmpty())
        return;
    loadEditor(name);
}

void AiConfigDialog::onAdd()
{
    list_->clearSelection();
    clearEditor(true);
}

void AiConfigDialog::onToggleKey()
{
    if (keyEdit_->echoMode() == QLineEdit::Password) {
        keyEdit_->setEchoMode(QLineEdit::Normal);
        keyToggle_->setText(tr("隐藏"));
    } else {
        keyEdit_->setEchoMode(QLineEdit::Password);
        keyToggle_->setText(tr("显示"));
    }
}

bool AiConfigDialog::saveEditor()
{
    AiProvider p;
    p.name = nameEdit_->text().trimmed();
    p.protocol = protocolFromName(protocolCombo_->currentData().toString());
    p.baseUrl = urlEdit_->text().trimmed();
    p.apiKey = keyEdit_->text().trimmed();
    p.models = editingModels_;
    p.currentModel = editingCurrentModel_;
    if (!creating_ && !editingName_.isEmpty()) {
        if (const AiProvider *old = store_->find(editingName_)) {
            p.id = old->id;
            p.importedFrom = old->importedFrom;
            p.importedId = old->importedId;
            p.sourceConfig = old->sourceConfig;
            p.contextWindow = old->contextWindow;
        }
    }
    const QString err = creating_ ? store_->add(p) : store_->upsert(p, editingName_);
    if (!err.isEmpty()) {
        setStatus(err, true);
        return false;
    }
    creating_ = false;
    editingName_ = p.name;
    rebuildList(p.name);
    setStatus(tr("已保存 %1").arg(p.name));
    return true;
}

void AiConfigDialog::onSave()
{
    saveEditor();
}

void AiConfigDialog::onSetCurrent()
{
    if (!saveEditor())
        return;
    const QString name = editingName_.isEmpty() ? nameEdit_->text().trimmed() : editingName_;
    const QString err = store_->setCurrent(name);
    if (!err.isEmpty()) {
        setStatus(err, true);
        return;
    }
    rebuildList(name);
    setStatus(tr("当前供应商:%1").arg(name));
}

void AiConfigDialog::onRemove()
{
    const QString name = creating_ ? QString() : editingName_;
    if (name.isEmpty()) {
        setStatus(tr("没有可删除的条目"), true);
        return;
    }
    if (QMessageBox::question(this, tr("删除供应商"),
                              tr("删除「%1」?只影响 Mswrite 的 AI 助手。").arg(name))
        != QMessageBox::Yes) {
        return;
    }
    const QString err = store_->remove(name);
    if (!err.isEmpty()) {
        setStatus(err, true);
        return;
    }
    rebuildList(store_->currentName());
    if (list_->count() == 0)
        clearEditor(true);
    else
        loadEditor(selectedName());
    setStatus(tr("已删除 %1").arg(name));
}

void AiConfigDialog::onImportMsAgent()
{
    QString report;
    const int n = store_->importFromMsAgent(&report);
    if (n < 0) {
        setStatus(report, true);
        return;
    }
    rebuildList(store_->currentName());
    setStatus(report);
}

void AiConfigDialog::onImportCcSwitch()
{
    QString report;
    const int n = store_->importFromCcSwitch(&report);
    if (n < 0) {
        setStatus(report, true);
        return;
    }
    rebuildList(store_->currentName());
    setStatus(report);
}
