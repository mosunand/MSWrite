#pragma once
// ai/AiConfigDialog.h — AI 供应商配置(左列表 + 右表单,布局借鉴 MS-Agent)。
// 右表单 = 供应商(名称/地址/Key/协议) + 模型列表(一个网址可挂多个模型);
// 每个模型点 ✎ 打开独立的模型配置弹窗(编辑模型配置,见 AiModelEditDialog)。

#include <QDialog>

#include "ai/AiProviders.h"

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

// 单个模型的编辑弹窗(智能配置 / ID / 上下文窗口 / 最大输出 / 输入类型 /
// 模型能力 / 推理等级 chips)
class AiModelEditDialog : public QDialog {
    Q_OBJECT
public:
    // cfg 为待编辑的模型配置;返回是否保存
    static bool edit(QWidget *parent, const QString &theme, AiModelCfg &cfg, bool isNew);

private:
    AiModelEditDialog(QWidget *parent, const QString &theme, AiModelCfg &cfg, bool isNew);
    void rebuildChips();   // 推理等级 chips 重建(成员函数,生命周期跟对话框走)

    AiModelCfg &cfg_;
    QCheckBox *smart_ = nullptr;
    QLineEdit *idEdit_ = nullptr;
    QSpinBox *ctxSpin_ = nullptr;
    QSpinBox *outSpin_ = nullptr;
    QCheckBox *inImage_ = nullptr;
    QCheckBox *inVideo_ = nullptr;
    QCheckBox *inPdf_ = nullptr;
    QCheckBox *capStructured_ = nullptr;
    QCheckBox *capSearch_ = nullptr;
    QCheckBox *capSystem_ = nullptr;
    QVBoxLayout *chipsLay_ = nullptr;   // 推理等级 chips 容器
    QStringList m_thinkLevels;          // 思考档位编辑态副本(exec 期间必须活着!)
    bool dark_ = false;
};

class AiConfigDialog : public QDialog {
    Q_OBJECT
public:
    explicit AiConfigDialog(AiProviderStore *store, QWidget *parent = nullptr,
                            const QString &theme = QStringLiteral("light"));

private slots:
    void onSelect();
    void onAdd();
    void onRemove();
    void onSave();
    void onSetCurrent();
    void onImportMsAgent();
    void onImportCcSwitch();
    void onToggleKey();

private:
    void rebuildList(const QString &selectName);
    void rebuildModelRows();   // 按 editingModels_ 重建右侧模型列表
    void loadEditor(const QString &name);
    void clearEditor(bool forNew);
    bool saveEditor();
    QString selectedName() const;
    void setStatus(const QString &text, bool error = false);

    AiProviderStore *store_ = nullptr;
    QListWidget *list_ = nullptr;
    QLineEdit *nameEdit_ = nullptr;
    QLineEdit *urlEdit_ = nullptr;
    QLineEdit *keyEdit_ = nullptr;
    QComboBox *protocolCombo_ = nullptr;
    QListWidget *modelList_ = nullptr;       // 供应商的模型列表(行内 toggle/编辑)
    QPushButton *keyToggle_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *fromLabel_ = nullptr;
    QVector<AiModelCfg> editingModels_;      // 右侧表单的模型工作副本(保存时落库)
    QString editingCurrentModel_;            // 工作副本中的当前模型 id
    bool creating_ = false;
    bool dark_ = false;
    QString editingName_;
    quint64 modelRowsGeneration_ = 0;
};
