#pragma once
// ai/AiConfigDialog.h — AI 供应商配置(左列表 + 右表单,布局借鉴 MS-Agent)。

#include <QDialog>

#include "ai/AiProviders.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

class AiConfigDialog : public QDialog {
    Q_OBJECT
public:
    explicit AiConfigDialog(AiProviderStore *store, QWidget *parent = nullptr);

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
    void loadEditor(const QString &name);
    void clearEditor(bool forNew);
    bool saveEditor();
    QString selectedName() const;
    void setStatus(const QString &text, bool error = false);

    AiProviderStore *store_ = nullptr;
    QListWidget *list_ = nullptr;
    QLineEdit *nameEdit_ = nullptr;
    QLineEdit *modelEdit_ = nullptr;
    QLineEdit *urlEdit_ = nullptr;
    QLineEdit *keyEdit_ = nullptr;
    QComboBox *protocolCombo_ = nullptr;
    QSpinBox *maxTokensSpin_ = nullptr;
    QPushButton *keyToggle_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *fromLabel_ = nullptr;
    bool creating_ = false;
    QString editingName_;
};
