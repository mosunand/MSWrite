// pdfview.cpp — see pdfview.h.
//
// 结构:左侧目录侧栏(可收) + 右侧 PdfMultiPageView(连续滚动、文字选择) + 底部工具栏(翻页/缩放/搜索)。
// 键盘:PageUp/Down 翻页,Home/End 首/末页,Ctrl+F 搜索,Ctrl+=/-/0 缩放。

#include "pdfview.h"
#include "uiicons.h"

#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QQuickWidget>
#include <QQuickItem>
#include <QClipboard>
#include <QMenu>
#include <QSplitter>
#include <QUrl>

#include <QEvent>
#include <QGuiApplication>
#include <QApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QScrollBar>
#include <QFontMetrics>
#include <cmath>
#include <functional>
#include <QtMath>
#include <QSpinBox>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

PdfViewWidget::PdfViewWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(0, 0, 0, 0);
    rootLay->setSpacing(0);

    // ══ 左:目录侧栏(默认隐藏) ══
    m_outlinePanel = new QWidget(this);
    m_outlinePanel->setObjectName(QStringLiteral("pdfOutline"));
    m_outlinePanel->setMinimumWidth(160);
    m_outlinePanel->setStyleSheet(QStringLiteral(
        "background:palette(window); color:palette(text); border-right:1px solid palette(mid);"));
    {
        auto *ol = new QVBoxLayout(m_outlinePanel);
        ol->setContentsMargins(4, 8, 4, 4);
        ol->setSpacing(4);
        auto *title = new QLabel(tr("目录"), m_outlinePanel);
        title->setStyleSheet(QStringLiteral(
            "font-weight:bold; font-size:13px; color:palette(text); padding:2px;"));
        ol->addWidget(title);
        m_outlineTree = new QTreeView(m_outlinePanel);
        m_outlineTree->setHeaderHidden(true);
        m_outlineTree->setStyleSheet(QStringLiteral(
            "QTreeView { background:palette(base); color:palette(text); border:none; outline:none; }"
            "QTreeView::item { padding:4px 6px; border-radius:4px; }"
            "QTreeView::item:hover { background:palette(alternate-base); }"
            "QTreeView::item:selected { background:palette(highlight); color:palette(highlighted-text); }"
            "QScrollBar { background:transparent; border:0; }"
            "QScrollBar:vertical { width:8px; margin:2px 0; }"
            "QScrollBar:horizontal { height:8px; margin:0 2px; }"
            "QScrollBar::handle { background:palette(mid); border-radius:3px; min-width:24px; min-height:24px; }"
            "QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }"
            "QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }"));
        m_outlineTree->setExpandsOnDoubleClick(true);
        m_outlineTree->setTextElideMode(Qt::ElideNone);
        m_outlineTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_outlineTree->header()->setStretchLastSection(false);
        ol->addWidget(m_outlineTree, 1);
    }
    m_outlinePanel->hide();

    // ══ 右:内容区 ══
    auto *content = new QWidget(this);
    auto *cl = new QVBoxLayout(content);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);

    m_doc = new QPdfDocument(this);
    m_qualityTimer = new QTimer(this);
    m_qualityTimer->setSingleShot(true);
    m_qualityTimer->setInterval(60);
    connect(m_qualityTimer, &QTimer::timeout, this, &PdfViewWidget::updateRenderQuality);
    m_outlineTimer = new QTimer(this);
    m_outlineTimer->setSingleShot(true);
    m_outlineTimer->setInterval(0);
    connect(m_outlineTimer, &QTimer::timeout, this, &PdfViewWidget::fitOutlineWidth);
    connect(m_outlineTree, &QTreeView::expanded, this, [this] { m_outlineTimer->start(); });
    connect(m_outlineTree, &QTreeView::collapsed, this, [this] { m_outlineTimer->start(); });
    m_view = new QQuickWidget(content);
    m_view->setObjectName(QStringLiteral("pdfReader"));
    m_view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_view->setSource(QUrl(QStringLiteral("qrc:/pdf/Reader.qml")));
    m_view->setFocusPolicy(Qt::StrongFocus);
    m_view->installEventFilter(this);
    setFocusProxy(m_view);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu(this);
        const QString selection=selectedText();
        const int page=currentPage();
        auto *copy = menu.addAction(tr("复制选中文字"), this, &PdfViewWidget::copySelection);
        copy->setEnabled(!selectedText().isEmpty());
        menu.addAction(tr("全选当前页"), this, &PdfViewWidget::selectAll);
        menu.addSeparator();
        menu.addAction(tr("查找…"), this, &PdfViewWidget::startSearch);
        menu.addSeparator();
        auto *translate=menu.addAction(UiIcons::icon("book",palette().color(QPalette::Text)),tr("AI 翻译"));
        auto *analyze=menu.addAction(UiIcons::icon("sparkle",palette().color(QPalette::Text)),tr("AI 分析"));
        translate->setObjectName(QStringLiteral("pdfAiTranslate"));
        analyze->setObjectName(QStringLiteral("pdfAiAnalyze"));
        translate->setEnabled(!selection.trimmed().isEmpty());
        analyze->setEnabled(!selection.trimmed().isEmpty());
        connect(translate,&QAction::triggered,this,[this,selection,page]{emit aiSelectionRequested(selection,page,true);});
        connect(analyze,&QAction::triggered,this,[this,selection,page]{emit aiSelectionRequested(selection,page,false);});
        menu.exec(m_view->mapToGlobal(pos));
    });
    cl->addWidget(m_view, 1);

    // ── 搜索栏(Ctrl+F 弹出,Esc 收起) ──
    m_searchBar = new QWidget(content);
    {
        auto *sl = new QHBoxLayout(m_searchBar);
        sl->setContentsMargins(12, 4, 12, 4);
        sl->setSpacing(6);
        m_searchBar->setStyleSheet(QStringLiteral(
            "background:palette(window); color:palette(text); border-top:1px solid palette(mid);"));
        m_searchEdit = new QLineEdit(m_searchBar);
        m_searchEdit->installEventFilter(this);
        m_searchEdit->setPlaceholderText(tr("搜索 PDF 内容…"));
        m_searchEdit->setFixedHeight(28);
        m_searchEdit->setStyleSheet(QStringLiteral(
            "QLineEdit { background:palette(base); color:palette(text); border:1px solid palette(mid); border-radius:4px;"
            " padding:2px 8px; font-size:13px; }"
            "QLineEdit:focus { border-color:#2563eb; }"));
        m_searchInfo = new QLabel(m_searchBar);
        m_searchInfo->setStyleSheet(QStringLiteral("color:palette(text); font-size:12px;"));
        m_searchPrevBtn = new QPushButton(tr("↑"), m_searchBar);
        m_searchNextBtn = new QPushButton(tr("↓"), m_searchBar);
        for (auto *b : { m_searchPrevBtn, m_searchNextBtn }) {
            b->setFixedSize(28, 28);
            b->setStyleSheet(QStringLiteral(
                "background:palette(base); color:palette(text); border:1px solid palette(mid); border-radius:4px;"
                " font-size:14px;"));
        }
        auto *closeBtn = new QPushButton(tr("✕"), m_searchBar);
        closeBtn->setFixedSize(28, 28);
        closeBtn->setStyleSheet(QStringLiteral(
            "background:transparent; border:none; color:palette(text); font-size:14px;"));

        sl->addWidget(m_searchEdit, 1);
        sl->addWidget(m_searchInfo);
        sl->addWidget(m_searchPrevBtn);
        sl->addWidget(m_searchNextBtn);
        sl->addWidget(closeBtn);

        auto *searchTimer = new QTimer(this);
        searchTimer->setSingleShot(true);
        searchTimer->setInterval(300);
        connect(m_searchEdit, &QLineEdit::textChanged, this,
                [searchTimer](const QString &) { searchTimer->start(); });
        connect(searchTimer, &QTimer::timeout, this, [this] {
            if (m_searchBar->isVisible()) runSearch(m_searchEdit->text());
        });
        connect(m_searchNextBtn, &QPushButton::clicked, this, [this] { searchNext(); });
        connect(m_searchPrevBtn, &QPushButton::clicked, this, [this] { searchPrev(); });
        connect(closeBtn, &QPushButton::clicked, this, [this] {
            closeSearch();
        });
        // 搜索框回车 = 下一个;Shift+回车 = 上一个
        connect(m_searchEdit, &QLineEdit::returnPressed, this, [this] {
            if (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier)
                searchPrev();
            else
                searchNext();
        });
    }
    m_searchBar->hide();

    // ── 底部工具栏 ──
    auto *bar = new QWidget(this);
    m_toolbar = bar;
    bar->setObjectName(QStringLiteral("pdfToolbar"));
    bar->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    bar->installEventFilter(this);
    {
        auto *bl = new QHBoxLayout(bar);
        bl->setContentsMargins(14, 9, 14, 9);
        bl->setSpacing(8);

        auto mkBtn = [&](const QString &icon, const QString &tip, QWidget *parent) {
            auto *b = new QToolButton(parent);
            b->setProperty("pdfIcon", icon);
            b->setFixedSize(32, 32);
            b->setIconSize(QSize(18,18));
            b->setToolTip(tip);
            b->setAccessibleName(tip);
            b->setCursor(Qt::PointingHandCursor);
            b->setFocusPolicy(Qt::StrongFocus);
            return b;
        };
        auto group = [&](const QString &name) {
            auto *w=new QWidget(bar); w->setObjectName(name);
            auto *l=new QHBoxLayout(w); l->setContentsMargins(4,3,4,3); l->setSpacing(3);
            return w;
        };
        m_outlineBtn = mkBtn("outline", tr("显示/隐藏目录"), bar);
        m_outlineBtn->setCheckable(true);
        m_outlineBtn->setText(tr("目录"));
        m_outlineBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_outlineBtn->setFixedWidth(76);
        connect(m_outlineBtn, &QToolButton::clicked, this, &PdfViewWidget::toggleOutline);
        auto *searchBtn=mkBtn("search",tr("查找文字 (Ctrl+F)"),bar);
        connect(searchBtn, &QToolButton::clicked, this, &PdfViewWidget::startSearch);
        auto *pages=group(QStringLiteral("pdfPagesGroup"));
        auto *pl=qobject_cast<QHBoxLayout *>(pages->layout());
        m_prevBtn = mkBtn("prev", tr("上一页 (PageUp)"), pages);
        connect(m_prevBtn, &QToolButton::clicked, this, [this] {
            const int cur = currentPage();
            if (cur > 0)
                goToPage(cur - 1);
        });

        m_nextBtn = mkBtn("next", tr("下一页 (PageDown)"), pages);
        connect(m_nextBtn, &QToolButton::clicked, this, [this] {
            const int cur = currentPage();
            if (cur < pageCount() - 1)
                goToPage(cur + 1);
        });

        m_pageSpin = new QSpinBox(pages);
        m_pageSpin->setObjectName(QStringLiteral("pdfPageNumber"));
        m_pageSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        m_pageSpin->setKeyboardTracking(false);
        m_pageSpin->setFixedSize(42, 30);
        m_pageSpin->setMinimum(1);
        m_pageSpin->setAlignment(Qt::AlignCenter);
        m_pageSpin->setToolTip(tr("跳到指定页"));
        connect(m_pageSpin, &QSpinBox::valueChanged, this, [this](int v) {
            if (m_loaded && !m_spinGuard)
                goToPage(v - 1);
        });

        m_pageLabel = new QLabel(tr("/ 0"), pages);
        m_pageLabel->setObjectName(QStringLiteral("pdfPageTotal"));
        pl->addWidget(m_prevBtn); pl->addWidget(m_pageSpin); pl->addWidget(m_pageLabel); pl->addWidget(m_nextBtn);
        auto *zoom=group(QStringLiteral("pdfZoomGroup"));
        auto *zl=qobject_cast<QHBoxLayout *>(zoom->layout());
        auto *zoomOutBtn = mkBtn("minus", tr("缩小 (Ctrl+-)"),zoom);
        connect(zoomOutBtn, &QToolButton::clicked, this, &PdfViewWidget::zoomOut);
        auto *zoomInBtn = mkBtn("plus", tr("放大 (Ctrl+=)"),zoom);
        connect(zoomInBtn, &QToolButton::clicked, this, &PdfViewWidget::zoomIn);
        m_zoomLabel=new QLabel(QStringLiteral("100%"),zoom);
        m_zoomLabel->setObjectName(QStringLiteral("pdfZoomValue"));
        m_zoomLabel->setAlignment(Qt::AlignCenter); m_zoomLabel->setFixedWidth(48);
        zl->addWidget(zoomOutBtn); zl->addWidget(m_zoomLabel); zl->addWidget(zoomInBtn);
        auto *fit=group(QStringLiteral("pdfFitGroup"));
        auto *fl=qobject_cast<QHBoxLayout *>(fit->layout());
        m_fitWidthBtn=mkBtn("width",tr("适配宽度 (Ctrl+0)"),fit);
        m_fitPageBtn=mkBtn("page",tr("显示整页"),fit);
        m_fitWidthBtn->setCheckable(true); m_fitPageBtn->setCheckable(true);
        connect(m_fitWidthBtn, &QToolButton::clicked, this, &PdfViewWidget::zoomFitWidth);
        connect(m_fitPageBtn, &QToolButton::clicked, this, &PdfViewWidget::zoomFitPage);
        fl->addWidget(m_fitWidthBtn); fl->addWidget(m_fitPageBtn);
        bl->addWidget(m_outlineBtn); bl->addWidget(searchBtn);
        bl->addStretch();
        bl->addWidget(pages);
        bl->addStretch();
        bl->addWidget(zoom); bl->addWidget(fit);
    }

    cl->addWidget(m_searchBar);
    auto *splitter = new QSplitter(this);
    m_splitter = splitter;
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(3);
    splitter->addWidget(m_outlinePanel);
    splitter->addWidget(content);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({220, 900});
    rootLay->addWidget(splitter, 1);
    rootLay->addWidget(bar);
    connect(splitter, &QSplitter::splitterMoved, this, [this] {
        if (!m_sizingOutline) m_manualOutlineWidth = true;
    });
    if (auto *root = m_view->rootObject()) {
        connect(root, SIGNAL(currentPageChanged()), this, SLOT(updateNavUi()));
        connect(root, SIGNAL(zoomFactorChanged()), this, SLOT(updateNavUi()));
        connect(root, SIGNAL(fitModeChanged()), this, SLOT(updateNavUi()));
        connect(root, SIGNAL(searchCountChanged()), this, SLOT(updateSearchUi()));
        connect(root, SIGNAL(searchIndexChanged()), this, SLOT(updateSearchUi()));
        connect(root, SIGNAL(zoomFactorChanged()), m_qualityTimer, SLOT(start()));
        connect(root, SIGNAL(currentPageChanged()), m_qualityTimer, SLOT(start()));
    }
    setTheme(QStringLiteral("light"));

}

PdfViewWidget::~PdfViewWidget() = default;

bool PdfViewWidget::load(const QString &path)
{
    if (!m_view->rootObject())
        return false;
    const QPdfDocument::Error err = m_doc->load(path);
    if (err != QPdfDocument::Error::None) {
        m_loaded = false;
        return false;
    }
    m_view->rootObject()->setProperty("source", QUrl::fromLocalFile(path));
    m_path = path;
    m_loaded = true;
    m_pageSpin->setMaximum(pageCount());
    rebuildOutline();
    updateNavUi();
    m_qualityTimer->start();
    return true;
}

int PdfViewWidget::pageCount() const
{
    return m_doc->pageCount();
}

int PdfViewWidget::currentPage() const
{
    return m_view->rootObject() ? qMax(0, m_view->rootObject()->property("currentPage").toInt()) : 0;
}

void PdfViewWidget::zoomIn()
{
    if (auto *root = m_view->rootObject())
        QMetaObject::invokeMethod(root, "setZoom", Q_ARG(QVariant,
            qBound(0.25, root->property("zoomFactor").toDouble() + 0.1, 5.0)));
}

void PdfViewWidget::zoomOut()
{
    if (auto *root = m_view->rootObject())
        QMetaObject::invokeMethod(root, "setZoom", Q_ARG(QVariant,
            qBound(0.25, root->property("zoomFactor").toDouble() - 0.1, 5.0)));
}

void PdfViewWidget::zoomFitWidth()
{
    if (auto *root = m_view->rootObject())
        QMetaObject::invokeMethod(root, "setFit", Q_ARG(QVariant, 1));
}

void PdfViewWidget::zoomFitPage()
{
    if (auto *root = m_view->rootObject())
        QMetaObject::invokeMethod(root, "setFit", Q_ARG(QVariant, 2));
}

void PdfViewWidget::goToPage(int page)
{
    if (page >= 0 && page < pageCount() && m_view->rootObject())
        QMetaObject::invokeMethod(m_view->rootObject(), "goToPage", Q_ARG(QVariant, page));
}

QString PdfViewWidget::selectedText() const
{
    return m_view->rootObject() ? m_view->rootObject()->property("selectedText").toString() : QString();
}

void PdfViewWidget::copySelection()
{
    if (m_searchBar->isVisible() && m_searchEdit->hasFocus()) {
        m_searchEdit->copy();
        return;
    }
    const QString text = selectedText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void PdfViewWidget::selectAll()
{
    if (m_searchBar->isVisible() && m_searchEdit->hasFocus()) {
        m_searchEdit->selectAll();
        return;
    }
    if (m_view->rootObject())
        QMetaObject::invokeMethod(m_view->rootObject(), "selectAll");
}

void PdfViewWidget::setTheme(const QString &theme)
{
    const bool dark = theme == QLatin1String("dark");
    QPalette p = palette();
    p.setColor(QPalette::Window, QColor(dark ? "#202329" : "#f5f6f8"));
    p.setColor(QPalette::Base, QColor(dark ? "#181b20" : "#ffffff"));
    p.setColor(QPalette::AlternateBase, QColor(dark ? "#2b3038" : "#edf0f5"));
    p.setColor(QPalette::Text, QColor(dark ? "#e5e7eb" : "#253041"));
    p.setColor(QPalette::WindowText, p.color(QPalette::Text));
    p.setColor(QPalette::ButtonText, p.color(QPalette::Text));
    p.setColor(QPalette::Button, p.color(QPalette::Window));
    p.setColor(QPalette::Mid, QColor(dark ? "#404752" : "#d5dbe4"));
    p.setColor(QPalette::Midlight, p.color(QPalette::Mid));
    p.setColor(QPalette::Highlight, QColor(dark ? "#254b78" : "#dbeafe"));
    p.setColor(QPalette::HighlightedText, QColor(dark ? "#ffffff" : "#17457e"));
    setPalette(p);
    // Qt's Windows style can resolve palette() against the system theme even
    // when the widget palette is correct. Materialize the local colors in QSS.
    setStyleSheet(QStringLiteral("PdfViewWidget { background:%1; color:%2; }"
        "QLabel { color:%2; } QSpinBox { background:%3; color:%2; border:1px solid %4; border-radius:5px; padding:2px; }")
        .arg(p.color(QPalette::Window).name(), p.color(QPalette::Text).name(),
             p.color(QPalette::Base).name(), p.color(QPalette::Mid).name()));
    const QList<QPair<QString,QPalette::ColorRole>> roles = {
        {"window", QPalette::Window}, {"text", QPalette::Text}, {"base", QPalette::Base},
        {"alternate-base", QPalette::AlternateBase}, {"mid", QPalette::Mid},
        {"midlight", QPalette::Midlight}, {"highlight", QPalette::Highlight},
        {"highlighted-text", QPalette::HighlightedText}};
    for (auto *child : findChildren<QWidget *>()) {
        child->setPalette(p);
        if (!child->property("pdfStyleTemplate").isValid())
            child->setProperty("pdfStyleTemplate", child->styleSheet());
        QString css = child->property("pdfStyleTemplate").toString();
        if (css.isEmpty()) continue;
        for (const auto &role : roles)
            css.replace(QStringLiteral("palette(%1)").arg(role.first), p.color(role.second).name());
        child->setStyleSheet(css);
    }
    // Applying a stylesheet can resolve the previous theme into the root's
    // palette; finish with the new palette after every stylesheet is updated.
    setPalette(p);
    m_toolbar->setStyleSheet(QStringLiteral(R"(
QWidget#pdfToolbar { background:%1; border-top:1px solid %4; }
QWidget#pdfPagesGroup, QWidget#pdfZoomGroup, QWidget#pdfFitGroup { background:%3; border:1px solid %4; border-radius:10px; }
QToolButton { background:transparent; color:%2; border:0; border-radius:7px; padding:0; font-size:12px; }
QToolButton:hover { background:%5; }
QToolButton:checked { background:%6; }
QToolButton:focus { border:1px solid %7; }
QSpinBox#pdfPageNumber { background:transparent; color:%2; border:0; border-radius:5px; padding:0; font:13px 'Segoe UI'; selection-background-color:%6; }
QSpinBox#pdfPageNumber:focus { background:%5; }
QLabel#pdfPageTotal, QLabel#pdfZoomValue { color:%2; background:transparent; border:0; font:12px 'Segoe UI'; padding:0 2px; }
)").arg(p.color(QPalette::Window).name(),p.color(QPalette::Text).name(),p.color(QPalette::Base).name(),
           dark ? "#353c49" : "#e2e7ef",p.color(QPalette::AlternateBase).name(),
           dark ? "#2d4060" : "#e8efff",dark ? "#8bbcff" : "#5c6bde"));
    // Regenerate icons rather than relying on platform glyphs or emoji fonts.
    for (auto *button : m_toolbar->findChildren<QToolButton *>())
        button->setIcon(UiIcons::icon(button->property("pdfIcon").toString(), QColor(dark ? "#cbd5e1" : "#536277")));
    if (auto *root = m_view->rootObject())
        root->setProperty("color", QColor(dark ? "#111318" : "#e5e7eb"));
}

// ── 目录 ──

void PdfViewWidget::toggleOutline()
{
    m_outlinePanel->setVisible(!m_outlinePanel->isVisible());
    m_outlineBtn->setChecked(!m_outlinePanel->isHidden());
    m_outlineTimer->start();
}

bool PdfViewWidget::hasOutline() const
{
    return m_outlineTree->model() && m_outlineTree->model()->rowCount() > 0;
}

void PdfViewWidget::rebuildOutline()
{
    if (auto *old = m_outlineTree->model())
        old->deleteLater();
    disconnect(m_outlineTree, &QTreeView::clicked, this, nullptr);
    auto *model = new QPdfBookmarkModel(this);
    model->setDocument(m_doc);
    m_outlineTree->setModel(model);
    m_manualOutlineWidth = false;
    m_outlineTree->expandAll();
    // 点击目录项 → 跳到对应页
    connect(m_outlineTree, &QTreeView::clicked, this,
            [this, model](const QModelIndex &idx) {
        const int page = model->data(idx, int(QPdfBookmarkModel::Role::Page)).toInt();
        const QPointF location = model->data(idx, int(QPdfBookmarkModel::Role::Location)).toPointF();
        QMetaObject::invokeMethod(m_view->rootObject(), "goToLocation", Q_ARG(QVariant, page),
                                  Q_ARG(QVariant, location.x()), Q_ARG(QVariant, location.y()));
    });
    // 有目录的文档默认展开侧栏
    m_outlinePanel->setVisible(model->rowCount() > 0);
    m_outlineBtn->setEnabled(model->rowCount() > 0);
    m_outlineBtn->setChecked(model->rowCount() > 0);
    m_outlineTimer->start();
}

QString PdfViewWidget::pageText(int page) const
{
    return page>=0 && page<pageCount() ? m_doc->getAllText(page).text() : QString();
}

QImage PdfViewWidget::pageImage(int page) const
{
    if(page<0 || page>=pageCount()) return {};
    const QSizeF points=m_doc->pagePointSize(page);
    const double scale=qMin(1600.0/points.width(),2000.0/points.height());
    return m_doc->render(page,QSize(qCeil(points.width()*scale),qCeil(points.height()*scale)));
}

void PdfViewWidget::fitOutlineWidth()
{
    if (!m_splitter || m_outlinePanel->isHidden() || m_manualOutlineWidth) return;
    const auto *model=m_outlineTree->model();
    if (!model) return;
    int widest=0;
    const QFontMetrics metrics(m_outlineTree->font());
    std::function<void(const QModelIndex &,int)> measure = [&](const QModelIndex &parent,int depth) {
        for(int row=0;row<model->rowCount(parent);++row) {
            const auto index=model->index(row,0,parent);
            widest=qMax(widest, metrics.horizontalAdvance(index.data().toString())
                +(depth+1)*m_outlineTree->indentation()+30);
            if(m_outlineTree->isExpanded(index)) measure(index,depth+1);
        }
    };
    measure({},0);
    const int maximum=qMax(160,qMin(int(width()*0.42),width()-420));
    const int desired=qBound(160,widest+24,maximum);
    m_sizingOutline=true;
    m_splitter->setSizes({desired,qMax(0,m_splitter->width()-desired-m_splitter->handleWidth())});
    m_sizingOutline=false;
}

void PdfViewWidget::updateRenderQuality()
{
    auto *root=m_view->rootObject();
    if (!root) return;
    QList<QQuickItem *> items{root};
    for(int i=0;i<items.size();++i) {
        auto *item=items[i]; items.append(item->childItems());
        if(!item->property("msQualityWatch").toBool()) {
            item->setProperty("msQualityWatch",true);
            connect(item,&QQuickItem::childrenChanged,m_qualityTimer,qOverload<>(&QTimer::start));
        }
        if(!QString::fromLatin1(item->metaObject()->className()).contains("PdfPageImage")) continue;
        if(!item->property("msImageWatch").toBool()) {
            item->setProperty("msImageWatch",true);
            connect(item,&QQuickItem::widthChanged,m_qualityTimer,qOverload<>(&QTimer::start));
            connect(item,&QQuickItem::heightChanged,m_qualityTimer,qOverload<>(&QTimer::start));
            item->setProperty("cache",false);
            item->setProperty("smooth",true);
        }
        const double w=item->width(), h=item->height();
        if(w<1 || h<1) continue;
        // Supersample fine glyphs, using the native widget's DPR (Quick's
        // offscreen Screen attachment may still refer to the previous screen).
        // Bound very large pages so repeated zooming cannot exhaust memory.
        const double scale=qMin(qMax(2.0,m_view->devicePixelRatioF()*1.5),
            qMin(8192.0/qMax(w,h),std::sqrt(24000000.0/(w*h))));
        const QSize pixels(qMax(1,qCeil(w*scale)),qMax(1,qCeil(h*scale)));
        if(item->property("sourceSize").toSize()!=pixels) item->setProperty("sourceSize",pixels);
    }
}

// ── 搜索 ──

void PdfViewWidget::startSearch()
{
    m_searchBar->show();
    m_searchEdit->setFocus();
    m_searchEdit->selectAll();
    runSearch(m_searchEdit->text());
}

void PdfViewWidget::searchNext()
{
    if (m_view->rootObject()) QMetaObject::invokeMethod(m_view->rootObject(), "searchNext");
}

void PdfViewWidget::searchPrev()
{
    if (m_view->rootObject()) QMetaObject::invokeMethod(m_view->rootObject(), "searchPrev");
}

void PdfViewWidget::runSearch(const QString &text)
{
    if (m_view->rootObject()) m_view->rootObject()->setProperty("searchText", text);
}

void PdfViewWidget::closeSearch()
{
    m_searchBar->hide();
    runSearch(QString());
    m_view->setFocus();
}

void PdfViewWidget::updateSearchUi()
{
    auto *root = m_view->rootObject();
    const int total = root ? root->property("searchCount").toInt() : 0;
    const int cur = root ? root->property("searchIndex").toInt() : -1;
    m_searchPrevBtn->setEnabled(total > 0);
    m_searchNextBtn->setEnabled(total > 0);
    m_searchInfo->setText(m_searchEdit->text().isEmpty() ? QString()
        : total > 0 ? tr("%1 / %2").arg(qMax(0, cur + 1)).arg(total) : tr("无结果"));
}

// ── 事件 ──

bool PdfViewWidget::eventFilter(QObject *obj, QEvent *event)
{
    if(obj==m_view && event->type()==QEvent::DevicePixelRatioChange) m_qualityTimer->start();
    if(obj==m_toolbar && event->type()==QEvent::Resize) {
        const bool compact=m_toolbar->width()<660;
        m_outlineBtn->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
        m_outlineBtn->setFixedWidth(compact ? 32 : 76);
        m_zoomLabel->setVisible(m_toolbar->width()>=560);
        const int margin=compact ? 8 : 14;
        m_toolbar->layout()->setContentsMargins(margin,9,margin,9);
        m_toolbar->layout()->setSpacing(compact ? 4 : 8);
    }
    if (obj == m_view && event->type() == QEvent::Wheel) {
        auto *we = static_cast<QWheelEvent *>(event);
        if (we->modifiers() & Qt::ControlModifier) {
            if (we->angleDelta().y() > 0)
                zoomIn();
            else
                zoomOut();
            return true;
        }
        // Accelerate discrete mouse-wheel ticks only. Pixel-based touchpad
        // scrolling, horizontal gestures and Ctrl+wheel keep their native path.
        if(we->pixelDelta().isNull() && we->angleDelta().x()==0 && we->angleDelta().y()!=0
            && we->modifiers()==Qt::NoModifier && m_view->rootObject()) {
            const qreal step=110; // 每格滚轮 110px
            QVariant handled;
            QMetaObject::invokeMethod(m_view->rootObject(),"scrollWheel",Q_RETURN_ARG(QVariant,handled),
                Q_ARG(QVariant,-we->angleDelta().y()/120.0*step));
            if(handled.toBool()) {we->accept();return true;}
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape && m_searchBar->isVisible()) {
            closeSearch();
            return true;
        }
        if (obj == m_view) {
            if (key->matches(QKeySequence::Copy)) { copySelection(); return true; }
            if (key->matches(QKeySequence::SelectAll)) { selectAll(); return true; }
            if (key->matches(QKeySequence::Find)) { startSearch(); return true; }
            if (key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown
                || key->key() == Qt::Key_F3
                || ((key->modifiers() & Qt::ControlModifier)
                    && (key->key() == Qt::Key_Home || key->key() == Qt::Key_End))) {
                keyPressEvent(key);
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void PdfViewWidget::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_PageUp:
        goToPage(qMax(0, currentPage() - 1));
        return;
    case Qt::Key_PageDown:
        goToPage(qMin(pageCount() - 1, currentPage() + 1));
        return;
    case Qt::Key_Home:
        goToPage(0);
        return;
    case Qt::Key_End:
        goToPage(pageCount() - 1);
        return;
    case Qt::Key_F3:
        if (event->modifiers() & Qt::ShiftModifier)
            searchPrev();
        else
            searchNext();
        return;
    case Qt::Key_Escape:
        if (m_searchBar->isVisible()) {
            closeSearch();
            return;
        }
        break;
    }
    QWidget::keyPressEvent(event);
}

void PdfViewWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if(m_outlineTimer) m_outlineTimer->start();
    if(m_qualityTimer) m_qualityTimer->start();
}

void PdfViewWidget::updateNavUi()
{
    const int cur = currentPage();
    const int total = pageCount();
    m_pageLabel->setText(tr("/ %1").arg(total));
    m_spinGuard = true;
    m_pageSpin->setValue(cur + 1);
    m_spinGuard = false;
    m_prevBtn->setEnabled(cur > 0);
    m_nextBtn->setEnabled(cur < total - 1);
    emit pageChanged(cur, total);
    if (auto *root=m_view->rootObject()) {
        const double zoom=root->property("zoomFactor").toDouble();
        m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(zoom*100)));
        m_fitWidthBtn->setChecked(root->property("fitMode").toInt()==1);
        m_fitPageBtn->setChecked(root->property("fitMode").toInt()==2);
        emit zoomChanged(zoom);
    }
}
