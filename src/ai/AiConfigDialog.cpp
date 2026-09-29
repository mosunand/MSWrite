// ai/AiConfigDialog.cpp — see ai/AiConfigDialog.h.
// 布局参考 MS-Agent 的 ConfigWindow(左列表 + 右表单),配色跟随打开它的窗口,
// 与 Mswrite 其它对话框一致。

#include "ai/AiConfigDialog.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
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
QPushButton {
  background: %3; color: %2;
  border: 1px solid %4; border-radius: 8px; padding: 7px 14px;
}
QPushButton:hover, QPushButton:pressed { background: %6; }
QPushButton#primary { background: #2563eb; border: 1px solid #1d4ed8; color: #fff; font-weight: 600; }
QPushButton#primary:hover { background: #1d4ed8; }
QPushButton#danger { color: %7; border: 1px solid %4; }
QPushButton#danger:hover { background: %6; border-color:%7; }
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

} // namespace

AiConfigDialog::AiConfigDialog(AiProviderStore *store, QWidget *parent, const QString &theme)
    : QDialog(parent)
    , store_(store)
    , dark_(theme == QLatin1String("dark"))
{
    setWindowTitle(tr("AI 供应商设置"));
    setMinimumSize(QSize(760, 480));
    resize(880, 560);
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

    auto *autoImport = new QCheckBox(tr("启动时自动导入 cc-switch（含 Key）"), left);
    autoImport->setObjectName(QStringLiteral("autoImportCcSwitch"));
    autoImport->setChecked(store_->autoImportCcSwitch());
    autoImport->setToolTip(tr("读取本机 cc-switch，新增供应商或补全匹配条目的空 Key。\n"
                            "保留已有配置和当前选择；删除的导入项不会自动恢复。"));
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
            setStatus(tr("已关闭自动导入，已有配置保留"));
        }
    });
    leftLay->addWidget(autoImport);

    auto *pathHint = new QLabel(tr("保存到 %1").arg(
        QDir::toNativeSeparators(AiProviderStore::defaultFilePath())), left);
    pathHint->setObjectName(QStringLiteral("hint"));
    pathHint->setWordWrap(true);
    leftLay->addWidget(pathHint);

    // ── 右:表单 ──
    auto *right = new QWidget;
    auto *rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(24, 18, 24, 16);
    rightLay->setSpacing(12);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);

    nameEdit_ = new QLineEdit;
    protocolCombo_ = new QComboBox;
    protocolCombo_->addItem(tr("Anthropic Messages"), QStringLiteral("anthropic"));
    protocolCombo_->addItem(tr("OpenAI Chat Completions"), QStringLiteral("openai"));
    modelEdit_ = new QLineEdit;
    modelEdit_->setPlaceholderText(tr("glm-5.3 / glm-5.3[1M] / deepseek-flash"));
    urlEdit_ = new QLineEdit;
    urlEdit_->setPlaceholderText(tr("https://open.bigmodel.cn/api/anthropic"));

    auto *keyRow = new QWidget;
    auto *keyLay = new QHBoxLayout(keyRow);
    keyLay->setContentsMargins(0, 0, 0, 0);
    keyLay->setSpacing(6);
    keyEdit_ = new QLineEdit;
    keyEdit_->setEchoMode(QLineEdit::Password);
    keyEdit_->setPlaceholderText(tr("API Key"));
    keyToggle_ = new QPushButton(tr("显示"));
    keyToggle_->setFixedWidth(56);
    connect(keyToggle_, &QPushButton::clicked, this, &AiConfigDialog::onToggleKey);
    keyLay->addWidget(keyEdit_, 1);
    keyLay->addWidget(keyToggle_);

    maxTokensSpin_ = new QSpinBox;
    maxTokensSpin_->setRange(0, 1000000);
    maxTokensSpin_->setSingleStep(1024);
    maxTokensSpin_->setSpecialValueText(tr("默认 4096"));
    maxTokensSpin_->setToolTip(tr("单次回复上限;0 = 默认 4096"));

    form->addRow(tr("名称"), nameEdit_);
    form->addRow(tr("协议"), protocolCombo_);
    form->addRow(tr("模型"), modelEdit_);
    form->addRow(tr("API 地址"), urlEdit_);
    form->addRow(tr("回复上限"), maxTokensSpin_);
    form->addRow(tr("API Key"), keyRow);
    rightLay->addLayout(form);

    fromLabel_ = new QLabel(right);
    fromLabel_->setObjectName(QStringLiteral("from"));
    fromLabel_->setWordWrap(true);
    rightLay->addWidget(fromLabel_);

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("hint"));
    status_->setWordWrap(true);
    rightLay->addWidget(status_);
    rightLay->addStretch();

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
        const QString state = p.apiKey.trimmed().isEmpty() ? tr("未配置 API Key")
                            : p.model.trimmed().isEmpty() ? tr("未配置模型")
                            : p.baseUrl.trimmed().isEmpty() ? tr("未配置 API 地址")
                            : p.importedFrom.startsWith(QLatin1String("cc-switch:")) ? tr("来自 cc-switch")
                            : tr("本机配置");
        item->setText(QStringLiteral("%1%2\n%3 · %4\n%5")
                          .arg(on ? QStringLiteral("● ") : QStringLiteral("   "),
                               p.name,
                               p.model,
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
    modelEdit_->setText(p->model);
    urlEdit_->setText(p->baseUrl);
    maxTokensSpin_->setValue(p->maxTokens);
    keyEdit_->setText(p->apiKey);
    keyEdit_->setEchoMode(QLineEdit::Password);
    keyToggle_->setText(tr("显示"));
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
    modelEdit_->clear();
    urlEdit_->clear();
    maxTokensSpin_->setValue(0);
    keyEdit_->clear();
    keyEdit_->setEchoMode(QLineEdit::Password);
    keyToggle_->setText(tr("显示"));
    if (fromLabel_)
        fromLabel_->setText(forNew ? tr("新建供应商") : QString());
    setStatus(forNew ? tr("填写右侧表单后点「保存」") : QString());
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
    p.protocol = (protocolCombo_->currentData().toString() == QLatin1String("openai"))
                     ? Protocol::OpenAi
                     : Protocol::Anthropic;
    p.model = modelEdit_->text().trimmed();
    p.baseUrl = urlEdit_->text().trimmed();
    p.maxTokens = maxTokensSpin_->value();
    p.apiKey = keyEdit_->text().trimmed();
    if (!creating_ && !editingName_.isEmpty()) {
        if (const AiProvider *old = store_->find(editingName_))
            p.id = old->id;
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
