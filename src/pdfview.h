#pragma once
// pdfview.h — PDF 阅读标签页:连续滚动 + 缩放 + 页码导航 + 目录书签 + 搜索。
// 与 WebViewHost(MD 编辑器)平级,挂在 MainWindow 的 m_stack 里。

#include <QWidget>
#include <QImage>

class QPdfDocument;
class QQuickWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QToolButton;
class QTreeView;
class QSplitter;
class QTimer;

class PdfViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PdfViewWidget(QWidget *parent = nullptr);
    ~PdfViewWidget() override;

    bool load(const QString &path);  // false = 加载失败
    QString path() const { return m_path; }
    int pageCount() const;
    int currentPage() const;         // 0-based

    void zoomIn();                    // +10%
    void zoomOut();                   // -10%
    void zoomFitWidth();              // 适配宽度(默认)
    void zoomFitPage();              // 适配整页
    void goToPage(int page);         // 0-based

    // MainWindow 菜单的"大纲"面板不适用 PDF;这个面板独立弹/收
    void toggleOutline();
    bool hasOutline() const;

    // 搜索(面板内 Ctrl+F)
    void startSearch();
    void searchNext();
    void searchPrev();
    void copySelection();
    void selectAll();
    QString selectedText() const;
    QString pageText(int page) const;
    QImage pageImage(int page) const;
    void setTheme(const QString &theme);

signals:
    void pageChanged(int page, int total); // 状态栏联动
    void zoomChanged(double factor);        // 状态栏联动
    void aiSelectionRequested(const QString &text, int page, bool translate);

protected:
    // Ctrl+滚轮缩放 + PageUp/Down/Home/End 键盘翻页
    bool eventFilter(QObject *obj, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void updateNavUi();
    void updateSearchUi();

private:
    void rebuildOutline();
    void runSearch(const QString &text);
    void closeSearch();
    void fitOutlineWidth();
    void updateRenderQuality();

    QPdfDocument *m_doc = nullptr;
    QQuickWidget *m_view = nullptr;

    // 工具栏
    QLabel *m_pageLabel = nullptr;
    QToolButton *m_prevBtn = nullptr;
    QToolButton *m_nextBtn = nullptr;
    QSpinBox *m_pageSpin = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QWidget *m_toolbar = nullptr;
    QToolButton *m_outlineBtn = nullptr;
    QToolButton *m_fitWidthBtn = nullptr;
    QToolButton *m_fitPageBtn = nullptr;

    // 目录侧栏
    QWidget *m_outlinePanel = nullptr;
    QTreeView *m_outlineTree = nullptr;
    QSplitter *m_splitter = nullptr;
    QTimer *m_outlineTimer = nullptr;
    QTimer *m_qualityTimer = nullptr;
    bool m_manualOutlineWidth = false;
    bool m_sizingOutline = false;

    // 搜索栏
    QWidget *m_searchBar = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QLabel *m_searchInfo = nullptr;
    QPushButton *m_searchNextBtn = nullptr;
    QPushButton *m_searchPrevBtn = nullptr;

    QString m_path;
    bool m_loaded = false;
    bool m_spinGuard = false;
};
