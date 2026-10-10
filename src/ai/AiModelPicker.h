#pragma once
// ai/AiModelPicker.h — 模型切换弹层,对齐 ZCode 模型菜单的视觉与交互:
// 当前模型行(✓ 靠右 + 视觉胶囊徽标) → 分隔 → 供应商行(名称 + 右侧 › ,
// 悬停/点击在行右侧弹出该供应商的模型子菜单) → 分隔 → 管理模型。
// 行 = 水平布局(文字 + stretch + 徽标/箭头),徽标为实底圆角胶囊。
// Qt::Popup 点外部自动关闭。纯头文件组件:宿主只传数据 + 回调。

#include "ai/AiProviders.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPointer>
#include <QRegion>
#include <QScreen>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

#include <windows.h>

namespace aimodelpicker_detail {

inline QColor bgColor(bool dark) { return dark ? QColor("#2c2c2c") : QColor("#ffffff"); }
inline QColor borderColor(bool dark) { return dark ? QColor("#474747") : QColor("#d9dee7"); }
inline QString inkColor(bool dark) { return dark ? "#e9e9e9" : "#1f2937"; }
inline QString subColor(bool dark) { return dark ? "#a7a7a7" : "#687386"; }
inline QString hoverColor(bool dark)
{
    return dark ? "#3a3a3a" : "#eef2f8";
}
inline QString badgeStyle(bool dark)
{
    return QStringLiteral(
               "background:%1;color:%2;border-radius:9px;padding:1px 7px;font-size:11px;")
        .arg(dark ? "#454545" : "#eef1f6", subColor(dark));
}
inline QString rowStyle(bool dark)
{
    return QStringLiteral(
               "QWidget#row { background:transparent; border-radius:7px; }"
               "QWidget#row:hover { background:%1; }"
               "QLabel { background:transparent; font-size:14px; }"
               "QLabel#title { color:%2; }"
               "QLabel#badge { %3 }"
               "QLabel#trail { color:%4; font-size:17px; }")
        .arg(hoverColor(dark), inkColor(dark), badgeStyle(dark), subColor(dark));
}

// ZCode 式行:水平布局 [文字][stretch][胶囊徽标][✓][›]
inline QWidget *makeRow(QWidget *parent, bool dark, const QString &text,
                        const QString &badge, const QString &trail, bool check,
                        bool muted = false)
{
    auto *row = new QWidget(parent);
    row->setObjectName(QStringLiteral("row"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    row->setStyleSheet(rowStyle(dark));
    row->setFixedHeight(46);
    row->setCursor(Qt::PointingHandCursor);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(10, 0, 10, 0);
    lay->setSpacing(8);
    auto *title = new QLabel(text, row);
    title->setObjectName(QStringLiteral("title"));
    title->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    if (muted)
        title->setStyleSheet(QStringLiteral("color:%1;").arg(subColor(dark)));
    lay->addWidget(title);
    if (!badge.isEmpty()) {
        auto *b = new QLabel(badge, row);
        b->setObjectName(QStringLiteral("badge"));
        lay->addWidget(b);
    }
    lay->addStretch();
    if (check) {
        auto *ok = new QLabel(QStringLiteral("✓"), row);
        ok->setObjectName(QStringLiteral("trail"));
        lay->addWidget(ok);
    }
    if (!trail.isEmpty()) {
        auto *ar = new QLabel(trail, row);
        ar->setObjectName(QStringLiteral("trail"));
        lay->addWidget(ar);
    }
    return row;
}

inline QLabel *makeSeparator(QWidget *parent, bool dark)
{
    auto *line = new QLabel(parent);
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background:%1;margin:5px 9px;")
                            .arg(borderColor(dark).name()));
    return line;
}

inline void disableDwmShadow(QWidget *w)
{
    using DwmSetWindowAttributeFn = long(__stdcall *)(void *, unsigned long, const void *, unsigned long);
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dwm)
        return;
    const auto set = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (set) {
        HWND hwnd = reinterpret_cast<HWND>(w->winId());
        BOOL disable = TRUE;
        set(hwnd, 2 /*DWMWA_NCRENDERING_POLICY*/, &disable, sizeof(disable));
    }
    FreeLibrary(dwm);
}

inline QString ctxBadge(int tokens)
{
    if (tokens <= 0) return QString();
    if (tokens % 1048576 == 0) return QStringLiteral("%1M").arg(tokens / 1048576);
    if (tokens % 1024 == 0) return QStringLiteral("%1k").arg(tokens / 1024);
    return QString::number(tokens);
}

} // namespace aimodelpicker_detail

class AiModelPicker : public QFrame {
    Q_OBJECT
public:
    using SwitchHandler = std::function<void(const QString &provider, const QString &model)>;

    // 在 anchor 上方弹出;providers 里只列出有启用模型的供应商
    static void showFor(QWidget *anchor,
                        const QVector<AiProvider> &providers,
                        const QString &currentProvider,
                        SwitchHandler onSwitch, std::function<void()> onManage, bool dark)
    {
        auto *picker = new AiModelPicker(anchor, dark);
        picker->onSwitch_ = std::move(onSwitch);
        picker->fill(providers, currentProvider);
        picker->finishWithManage(std::move(onManage));
        picker->layout()->activate();
        picker->adjustSize();
        const int w = 264;
        const int h = qMin(picker->sizeHint().height() + 2, 680);
        picker->resize(w, h);
        QPainterPath pickerPath;
        pickerPath.addRoundedRect(QRectF(picker->rect()), 10, 10);
        picker->setMask(QRegion(pickerPath.toFillPolygon().toPolygon()));
        QPoint pos = anchor->mapToGlobal(QPoint(0, -h - 6));
        if (QScreen *scr = anchor->screen()) {
            const QRect avail = scr->availableGeometry();
            if (pos.x() + w > avail.right())
                pos.setX(qMax(avail.left(), avail.right() - w - 8));
            if (pos.y() < avail.top())
                pos.setY(anchor->mapToGlobal(QPoint(0, 0)).y() + anchor->height() + 6);
        }
        picker->move(pos);
        picker->show();
        aimodelpicker_detail::disableDwmShadow(picker);
    }

private:
    explicit AiModelPicker(QWidget *anchor, bool dark)
        : QFrame(anchor->window(), Qt::Popup), dark_(dark)
    {
        setObjectName(QStringLiteral("aiModelPicker"));
        setAttribute(Qt::WA_DeleteOnClose);
        setAttribute(Qt::WA_TranslucentBackground);
        setStyleSheet(QStringLiteral(
            "QFrame#aiModelPicker { background: transparent; border: none; }"
            "QScrollArea { background: transparent; border: none; }"
            "QScrollArea > QWidget > QWidget { background: transparent; }"
            "QScrollBar:vertical { background: transparent; width: 6px; margin: 2px 0; }"
            "QScrollBar::handle:vertical { background:%1; border-radius:3px; min-height:24px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }")
                          .arg(dark_ ? "rgba(232,234,237,0.30)" : "rgba(60,80,120,0.28)"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(aimodelpicker_detail::borderColor(dark_), 1));
        p.setBrush(aimodelpicker_detail::bgColor(dark_));
        p.drawRoundedRect(box, 10, 10);
    }

    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::Enter) {
            for (const HoverRow &h : hoverRows_) {
                if (h.row == obj) {
                    openSubmenuFor(h.provider, h.row);
                    break;
                }
            }
        } else if (event->type() == QEvent::Leave && submenu_) {
            closeSubmenuSoon();
        } else if (event->type() == QEvent::MouseButtonRelease) {
            for (const ClickRow &c : clickedRows_) {
                if (c.row == obj) {
                    c.act();
                    return true;
                }
            }
        }
        return QFrame::eventFilter(obj, event);
    }

private:
    void fill(const QVector<AiProvider> &providers, const QString &currentProvider)
    {
        providers_ = providers;
        currentProvider_ = currentProvider;

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(8, 6, 8, 6);
        root->setSpacing(0);

        // 这里是 Mswrite 自己的供应商分组提示，不复制 ZCode 的订阅/当前模型区域。
        auto *section = aimodelpicker_detail::makeRow(
            this, dark_, tr("我的ai供应商"), QString(), QString(), false, true);
        section->setCursor(Qt::ArrowCursor);
        root->addWidget(section);
        root->addWidget(aimodelpicker_detail::makeSeparator(this, dark_));

        // 供应商列表(超过 10 个:内部滚动)
        auto *listHost = new QWidget(this);
        auto *listLay = new QVBoxLayout(listHost);
        listLay->setContentsMargins(0, 2, 0, 2);
        listLay->setSpacing(1);
        int providerCount = 0;
        for (const AiProvider &p : providers) {
            bool any = false;
            for (const AiModelCfg &m : p.models)
                if (m.enabled) { any = true; break; }
            if (!any)
                continue;
            const bool isCur = (p.name == currentProvider);
            auto *row = aimodelpicker_detail::makeRow(
                this, dark_, p.name, QString(), QStringLiteral("›"), isCur);
            const QString provName = p.name;
            row->installEventFilter(this);
            clickedRows_.append({ row, [this, provName, row] { openSubmenuFor(provName, row); } });
            hoverRows_.append({ row, provName });
            listLay->addWidget(row);
            ++providerCount;
        }
        if (providerCount == 0) {
            auto *empty = aimodelpicker_detail::makeRow(
                this, dark_, tr("(还没有可用模型)"), QString(), QString(), false);
            empty->setCursor(Qt::ArrowCursor);
            listLay->addWidget(empty);
        }

        if (providerCount > 10) {
            auto *scroll = new QScrollArea(this);
            scroll->setWidgetResizable(true);
            scroll->setWidget(listHost);
            scroll->setFixedHeight(qMin(providerCount * 47 + 4, 470));
            root->addWidget(scroll);
        } else {
            root->addWidget(listHost);
        }
    }

    void finishWithManage(std::function<void()> onManage)
    {
        layout()->addWidget(aimodelpicker_detail::makeSeparator(this, dark_));
        auto *manage = aimodelpicker_detail::makeRow(
            this, dark_, tr("管理模型"), QString(), QString(), false);
        manage->setCursor(Qt::PointingHandCursor);
        manage->installEventFilter(this);
        clickedRows_.append({ manage, [this, onManage = std::move(onManage)] {
            close();
            if (onManage) onManage();
        } });
        layout()->addWidget(manage);
    }

    // ── 子菜单:该供应商的模型列表,弹出在被 hover 行的右侧 ──
    void openSubmenuFor(const QString &provName, QWidget *anchorRow)
    {
        if (submenu_ && submenuProvider_ == provName && submenu_->isVisible())
            return;
        closeSubmenu();
        for (const AiProvider &p : providers_) {
            if (p.name != provName)
                continue;
            submenu_ = new Submenu(this, p, dark_, p.name == currentProvider_,
                [this](const QString &provider, const QString &model) {
                    close();
                    if (onSwitch_) onSwitch_(provider, model);
                });
            submenuProvider_ = provName;
            // 定位:行右缘外 6px,与行同高;越出屏幕则翻到左侧
            const QPoint rowGlobal = anchorRow->mapToGlobal(QPoint(anchorRow->width(), 0));
            QPoint pos(rowGlobal.x() - 6, rowGlobal.y());
            if (QScreen *scr = this->screen()) {
                const QRect avail = scr->availableGeometry();
                if (pos.x() + submenu_->width() > avail.right())
                    pos.setX(anchorRow->mapToGlobal(QPoint(0, 0)).x()
                             - submenu_->width() + 6);
                pos.setY(qBound(avail.top(), pos.y(),
                               qMax(avail.top(), avail.bottom() - submenu_->height())));
            }
            submenu_->move(pos);
            submenu_->show();
            return;
        }
    }

    void closeSubmenu()
    {
        if (submenu_) {
            submenu_->close();
            submenu_ = nullptr;
        }
        submenuProvider_.clear();
    }
    void closeSubmenuSoon()
    {
        if (!submenu_)
            return;
        QTimer::singleShot(220, this, [this] {
            if (submenu_ && !submenu_->underMouse() && !underMouse())
                closeSubmenu();
        });
    }

    // ZCode 供应商行右侧弹出的模型子菜单(与父菜单同风格)
    class Submenu : public QFrame {
    public:
        Submenu(QWidget *parent, const AiProvider &p, bool dark, bool currentProvider,
                std::function<void(const QString &, const QString &)> onPick)
            : QFrame(parent, Qt::Popup), onPick_(std::move(onPick)), dark_(dark),
              currentProvider_(currentProvider)
        {
            setObjectName(QStringLiteral("aiModelSubmenu"));
            setAttribute(Qt::WA_DeleteOnClose);
            setAttribute(Qt::WA_TranslucentBackground);
            auto *lay = new QVBoxLayout(this);
            lay->setContentsMargins(6, 6, 6, 6);
            lay->setSpacing(1);
            const QString cur = p.effectiveModelId();
            for (const AiModelCfg &m : p.models) {
                if (!m.enabled)
                    continue;
                const bool isCur = (m.id == cur);
                QString badge;
                if (m.inImage)
                    badge = tr("视觉");
                else
                    badge = aimodelpicker_detail::ctxBadge(m.contextWindow);
                auto *row = aimodelpicker_detail::makeRow(
                    this, dark_, m.id, badge, QString(), currentProvider_ && m.id == cur);
                const QString prov = p.name;
                const QString model = m.id;
                row->installEventFilter(this);
                picks_.append({ row, prov, model });
                lay->addWidget(row);
            }
            lay->activate();
            adjustSize();
            setFixedWidth(304);
            QPainterPath submenuPath;
            submenuPath.addRoundedRect(QRectF(rect()), 10, 10);
            setMask(QRegion(submenuPath.toFillPolygon().toPolygon()));
            aimodelpicker_detail::disableDwmShadow(this);
        }
    protected:
        void paintEvent(QPaintEvent *) override
        {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
            p.setPen(QPen(aimodelpicker_detail::borderColor(dark_), 1));
            p.setBrush(aimodelpicker_detail::bgColor(dark_));
            p.drawRoundedRect(box, 10, 10);
        }
        bool eventFilter(QObject *obj, QEvent *ev) override
        {
            if (ev->type() == QEvent::MouseButtonRelease) {
                for (const auto &pick : picks_) {
                    if (pick.row == obj) {
                        close();
                        if (onPick_) onPick_(pick.provider, pick.model);
                        return true;
                    }
                }
            }
            return QFrame::eventFilter(obj, ev);
        }
    private:
        struct Pick { QWidget *row; QString provider; QString model; };
        QList<Pick> picks_;
        std::function<void(const QString &, const QString &)> onPick_;
        bool dark_;
        bool currentProvider_;
    };

    QVector<AiProvider> providers_;
    QString currentProvider_;
    SwitchHandler onSwitch_;
    struct ClickRow { QWidget *row; std::function<void()> act; };
    QList<ClickRow> clickedRows_;
    struct HoverRow { QWidget *row; QString provider; };
    QList<HoverRow> hoverRows_;
    QPointer<Submenu> submenu_;
    QString submenuProvider_;
    bool dark_;
};
