#include "pdfview.h"
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QQuickItem>
#include <QQuickWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTest>
#include <QLineEdit>
#include <QMimeData>
#include <QTreeView>
#include <QTimer>
#include <QSplitter>
#include <QSpinBox>
#include <QToolButton>
#include <QElapsedTimer>
#include <QMenu>
#include <QSignalSpy>
#include <QWheelEvent>
#include <functional>
#include <objbase.h>

static bool waitFor(const std::function<bool()> &condition)
{
    for (int i=0; i<500; ++i) {
        if (condition()) return true;
        QTest::qWait(10);
    }
    return false;
}

int main(int argc, char **argv)
{
    OleInitialize(nullptr);
    QApplication app(argc, argv);
    const bool clipboardAvailable=OpenClipboard(nullptr);
    if(clipboardAvailable) CloseClipboard();
    auto *savedClipboard = new QMimeData;
    if (clipboardAvailable) if (const auto *original = QApplication::clipboard()->mimeData())
        for (const auto &format : original->formats())
            savedClipboard->setData(format, original->data(format));
    struct RestoreClipboard {
        QMimeData *data;
        bool available;
        ~RestoreClipboard() { if(available)QApplication::clipboard()->setMimeData(data);else delete data; }
    } restoreClipboard{savedClipboard,clipboardAvailable};
    int result = 1;
    QTimer::singleShot(0, &app, [&] {
    QTemporaryDir temp;
    const QString path = temp.filePath(QStringLiteral("selectable.pdf"));
    {
        QPdfWriter writer(path);
        writer.setResolution(72);
        QPainter p(&writer);
        p.setFont(QFont(QStringLiteral("Arial"), 18));
        p.drawText(60, 100, QStringLiteral("Selectable PDF text, page one."));
        p.drawText(60, 140, QStringLiteral("Search target Alpha."));
        writer.newPage();
        p.drawText(60, 100, QStringLiteral("Page two contains Beta."));
    }
    int passed=0, failed=0;
    QTextStream out(stdout);
    auto check = [&](bool ok, const char *name) {
        out << (ok ? "PASS " : "FAIL ") << name << '\n'; out.flush();
        ok ? ++passed : ++failed;
    };
    PdfViewWidget view;
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.resize(950, 800);
    view.show();
    check(view.load(path), "load PDF");
    auto *quick = view.findChild<QQuickWidget *>();
    auto *root = quick->rootObject();
    if (!root) { app.exit(1); return; }
    auto pageImage = [&]() -> QQuickItem * {
        QList<QQuickItem *> items{root};
        for(int i=0;i<items.size();++i) {
            auto *item=items[i]; items.append(item->childItems());
            if(QString::fromLatin1(item->metaObject()->className()).contains("PdfPageImage")
                && item->property("currentFrame").toInt()==view.currentPage()) return item;
        }
        return nullptr;
    };
    check(waitFor([&]{return root->property("pageReady").toBool();}), "page rendered");
    QTest::qWait(300); // Initial fit-to-width follows document readiness.
    check(waitFor([&]{auto *img=pageImage(); return img && img->property("sourceSize").toSize().width()
        >= img->width()*qMax(2.0,quick->devicePixelRatioF()*1.5)-1;}), "PDF raster resolution follows physical pixels with supersampling");
    if(auto *img=pageImage()) {
        const QSize pixels=img->property("sourceSize").toSize();
        out << "PDF_RASTER " << img->width() << "x" << img->height() << " -> "
            << pixels.width() << "x" << pixels.height() << " DPR=" << quick->devicePixelRatioF() << '\n';
        QTest::qWait(250);
        check(img->property("sourceSize").toSize()==pixels, "idle page resolution remains stable");
    }
    QMetaObject::invokeMethod(root,"setZoom",Q_ARG(QVariant,5.0));
    QTest::qWait(200);
    check(waitFor([&]{auto *img=pageImage();if(!img)return false;const auto size=img->property("sourceSize").toSize();
        return size.width()>2000 && size.width()<=8192 && size.height()<=8192
            && qint64(size.width())*size.height()<=24016384;}),"large zoom keeps raster memory bounded");
    view.zoomFitWidth(); QTest::qWait(250);
    check(view.pageCount()==2, "page count");
    view.selectAll();
    QTest::qWait(50);
    const QString selectedForAi=view.selectedText();
    QSignalSpy aiActions(&view,&PdfViewWidget::aiSelectionRequested);
    QTimer::singleShot(40,[&]{
        auto *menu=view.findChild<QMenu *>();
        if(!menu) return;
        auto *translate=menu->findChild<QAction *>("pdfAiTranslate");
        auto *analyze=menu->findChild<QAction *>("pdfAiAnalyze");
        check(translate && analyze && translate->isEnabled() && analyze->isEnabled(),"PDF selection menu offers translation and analysis");
        if(translate) translate->trigger();
        menu->close();
    });
    QMetaObject::invokeMethod(quick,"customContextMenuRequested",Q_ARG(QPoint,QPoint(80,80)));
    check(aiActions.size()==1 && aiActions.at(0).at(0).toString()==selectedForAi && aiActions.at(0).at(1).toInt()==0 && aiActions.at(0).at(2).toBool(),"PDF context action preserves exact selection and page");
    check(waitFor([&]{return view.selectedText().contains("Selectable");}), "select all current page");
    if(clipboardAvailable) {
        view.copySelection();
        check(waitFor([&]{return QApplication::clipboard()->text().contains("Selectable");}), "copy selected text");
    } else out<<"SKIP copy selected text: Windows OpenClipboard unavailable\n";

    // Drag over actual PDF glyphs in the real QQuickWidget, using the extracted
    // first line only to locate its screen coordinates.
    QPdfDocument doc;
    doc.load(path);
    const QRectF textRect = doc.getSelectionAtIndex(0, 0, 15).boundingRectangle();
    QQuickItem *selection = nullptr;
    QList<QQuickItem *> items{root};
    for (int i=0;i<items.size();++i) items.append(items[i]->childItems());
    for (auto *item : items) {
        if (QString::fromLatin1(item->metaObject()->className()).contains("PdfSelection")
            && item->property("page").toInt()==0) { selection=item; break; }
    }
    check(selection != nullptr, "text selection layer available");
    if (selection) {
        const qreal scale = root->property("zoomFactor").toDouble();
        const QPoint from = selection->mapToScene(QPointF(textRect.left()-2, textRect.center().y()) * scale).toPoint();
        const QPoint to = selection->mapToScene(QPointF(textRect.right(), textRect.center().y()) * scale).toPoint();
        QTest::mousePress(quick, Qt::LeftButton, Qt::NoModifier, from);
        for (int i=1;i<=12;++i) QTest::mouseMove(quick, from+(to-from)*i/12, 20);
        QTest::mouseRelease(quick, Qt::LeftButton, Qt::NoModifier, to);
        check(waitFor([&]{return view.selectedText().contains("Selectable") && view.selectedText().size()<29;}), "mouse drag selects partial text");
        if(clipboardAvailable) {
            QTest::keyClick(quick, Qt::Key_C, Qt::ControlModifier);
            check(QApplication::clipboard()->text()==view.selectedText(), "Ctrl+C copies dragged selection");
        } else out<<"SKIP Ctrl+C clipboard check: Windows OpenClipboard unavailable\n";
    }
    const double before=root->property("zoomFactor").toDouble();
    auto *table=root->property("pageTable").value<QObject *>();
    auto wheel=[&](const QPoint &angle,const QPoint &pixels=QPoint(),Qt::KeyboardModifiers modifiers=Qt::NoModifier) {
        const QPoint pos=quick->rect().center();
        QWheelEvent event(pos,quick->mapToGlobal(pos),pixels,angle,Qt::NoButton,modifiers,Qt::NoScrollPhase,false);
        QApplication::sendEvent(quick,&event);QTest::qWait(100);
    };
    if(table) {
        view.goToPage(0);QTest::qWait(100);
        const qreal start=table->property("contentY").toDouble();
        wheel(QPoint(0,-120));
        const qreal distance=table->property("contentY").toDouble()-start;
        check(distance>=80 && distance<=240,"one PDF wheel tick advances a useful bounded reading distance");
        check(qFuzzyCompare(root->property("zoomFactor").toDouble(),before),"normal wheel scrolling preserves PDF zoom");
        wheel(QPoint(0,120));
        check(qAbs(table->property("contentY").toDouble()-start)<2,"wheel reverse returns to the same reading position");
        wheel(QPoint(0,1200));
        check(table->property("contentY").toDouble()>=table->property("originY").toDouble(),"accelerated wheel cannot overscroll above the first page");
        wheel(QPoint(0,-12000));
        check(table->property("contentY").toDouble()<=table->property("originY").toDouble()+table->property("contentHeight").toDouble()-table->property("height").toDouble()+2,"accelerated wheel stays within the last page");
        view.goToPage(0);QTest::qWait(100);
        wheel(QPoint(0,120),QPoint(),Qt::ControlModifier);
        check(root->property("zoomFactor").toDouble()>before,"Ctrl plus wheel still zooms the PDF");
        view.zoomFitWidth();view.goToPage(0);QTest::qWait(100);
    } else check(false,"PDF page table available for wheel interaction");
    const int pixelsBefore=pageImage() ? pageImage()->property("sourceSize").toSize().width() : 0;
    view.zoomIn();
    check(root->property("zoomFactor").toDouble()>before, "zoom in");
    check(waitFor([&]{return pageImage() && pageImage()->property("sourceSize").toSize().width()>pixelsBefore;}),
        "zoom rerenders at higher resolution");
    view.zoomFitPage();
    check(root->property("zoomFactor").toDouble()<before, "fit page");
    view.goToPage(1);
    check(waitFor([&]{return view.currentPage()==1;}), "navigate page two");
    view.startSearch();
    auto *search=view.findChild<QLineEdit *>();
    // QSpinBox also owns a line edit; choose the PDF search field explicitly.
    for (auto *edit : view.findChildren<QLineEdit *>())
        if (edit->placeholderText().contains(QStringLiteral("PDF"))) search=edit;
    search->setText(QStringLiteral("Alpha"));
    check(waitFor([&]{return root->property("searchCount").toInt()==1;}), "search locates text");
    check(waitFor([&]{return view.currentPage()==0;}), "search jumps to matching page");
    QTest::keyClick(search, Qt::Key_Escape);
    check(root->property("searchText").toString().isEmpty(), "Escape clears search highlights");
    view.setTheme(QStringLiteral("dark"));
    check(view.palette().color(QPalette::Text).lightness()>180
          && view.palette().color(QPalette::Base).lightness()<80, "dark theme contrast");
    view.setTheme(QStringLiteral("light"));
    check(view.palette().color(QPalette::Text).lightness()<100
          && view.palette().color(QPalette::Base).lightness()>200, "light theme contrast");
    // A small PDF with real outline destinations, independent of any user file.
    const QByteArray page = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /Font << /F1 9 0 R >> >> /Contents ";
    const QByteArray text1 = "BT /F1 18 Tf 60 690 Td (Selectable PDF text, page one.) Tj 0 -40 Td (Search target Alpha.) Tj ET";
    const QByteArray text2 = "BT /F1 18 Tf 60 690 Td (Page two contains Beta.) Tj ET";
    auto stream = [](const QByteArray &s) { return "<< /Length " + QByteArray::number(s.size()) + " >>\nstream\n" + s + "\nendstream"; };
    QList<QByteArray> objects = {
        "<< /Type /Catalog /Pages 2 0 R /Outlines 7 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>",
        page + "4 0 R >>", stream(text1), page + "6 0 R >>", stream(text2),
        "<< /Type /Outlines /First 8 0 R /Last 10 0 R /Count 2 >>",
        "<< /Title (First page - selectable text) /Parent 7 0 R /Dest [3 0 R /XYZ 0 800 0] /Next 10 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Title (Second page - navigation) /Parent 7 0 R /Dest [5 0 R /XYZ 0 800 0] /Prev 8 0 R >>"
    };
    auto writeOutline = [&](const QString &filePath) {
    QByteArray bytes="%PDF-1.4\n", xref="0000000000 65535 f \n";
    for(int i=0;i<objects.size();++i) {
        xref += QByteArray::number(bytes.size()).rightJustified(10,'0')+" 00000 n \n";
        bytes += QByteArray::number(i+1)+" 0 obj\n"+objects[i]+"\nendobj\n";
    }
    const int xrefOffset=bytes.size();
    bytes += "xref\n0 11\n"+xref+"trailer\n<< /Size 11 /Root 1 0 R >>\nstartxref\n"+QByteArray::number(xrefOffset)+"\n%%EOF\n";
    QFile outlineFile(filePath); outlineFile.open(QIODevice::WriteOnly); outlineFile.write(bytes); outlineFile.close();
    };
    const auto outlinePath=temp.filePath(QStringLiteral("bookmarks.pdf"));
    writeOutline(outlinePath);
    check(view.load(outlinePath) && view.hasOutline(), "PDF bookmarks populate outline");
    QTest::qWait(300);
    auto *tree=view.findChild<QTreeView *>();
    auto *splitter=view.findChild<QSplitter *>();
    const int normalWidth=splitter->sizes().first();
    check(normalWidth>=tree->fontMetrics().horizontalAdvance(QStringLiteral("First page - selectable text"))+tree->indentation(),
        "outline fits title text and indentation");
    const auto second=tree->model()->index(1,0);
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualRect(second).center());
    check(waitFor([&]{return view.currentPage()==1;}), "clicking bookmark navigates");
    check(tree->palette().color(QPalette::Text).lightness()<100
          && tree->palette().color(QPalette::Base).lightness()>200, "outline text has light-theme contrast");
    view.setTheme(QStringLiteral("dark"));
    check(tree->palette().color(QPalette::Text).lightness()>180
          && tree->palette().color(QPalette::Base).lightness()<80, "outline text has dark-theme contrast");
    view.setTheme(QStringLiteral("light"));
    view.zoomFitWidth();
    QTest::qWait(200);
    view.goToPage(0);
    QTest::qWait(500);
    check(!root->property("horizontalOverflow").toBool(), "fit width has no horizontal overflow");
    QList<QQuickItem *> pageItems{root};
    bool topVisible = false;
    for (int i=0; i<pageItems.size(); ++i) {
        auto *item=pageItems[i];
        pageItems.append(item->childItems());
        if (QString::fromLatin1(item->metaObject()->className()).contains("PdfSelection")
            && item->property("page").toInt()==0) {
            const qreal top=item->mapToScene(QPointF(0,0)).y();
            topVisible = top >= -1 && top < 40;
        }
    }
    check(topVisible, "page navigation reveals the top of the page");
    const auto shot = view.grab().toImage();
    const auto panelPoint = tree->viewport()->mapTo(&view, QPoint(20, tree->viewport()->height()-20));
    check(shot.pixelColor(panelPoint).lightness()>200, "rendered outline background follows light theme");
    view.selectAll();
    if (app.arguments().size()>1) view.grab().save(app.arguments().at(1));
    objects[7].replace("First page - selectable text", "One");
    objects[9].replace("Second page - navigation", "Two");
    const auto shortPath=temp.filePath(QStringLiteral("short.pdf")); writeOutline(shortPath);
    view.load(shortPath); QTest::qWait(250);
    check(splitter->sizes().first()<normalWidth, "short outline gives space back to document");
    objects[7].replace("One", "A very long chapter title that should not consume the entire document viewport even on a narrow window");
    const auto longPath=temp.filePath(QStringLiteral("long.pdf")); writeOutline(longPath);
    view.load(longPath); QTest::qWait(250);
    check(splitter->sizes().first()<=view.width()*0.42+1, "long outline width is bounded");
    auto *handle=splitter->handle(1);
    QTest::mousePress(handle,Qt::LeftButton,Qt::NoModifier,handle->rect().center());
    QTest::mouseMove(handle,handle->rect().center()-QPoint(80,0),50);
    QTest::mouseRelease(handle,Qt::LeftButton,Qt::NoModifier,handle->rect().center());
    const int manualWidth=splitter->sizes().first();
    view.resize(1000,800); QTest::qWait(200);
    check(manualWidth<view.width()*0.35 && qAbs(splitter->sizes().first()-manualWidth)<=1,
        "manual outline width survives resizing");
    auto *spin=view.findChild<QSpinBox *>();
    spin->setFocus(); spin->selectAll(); QTest::keyClicks(spin,"2");
    check(view.currentPage()==0, "page input waits for confirmation");
    QTest::keyClick(spin,Qt::Key_Return);
    check(waitFor([&]{return view.currentPage()==1;}), "Enter commits page navigation");
    view.toggleOutline(); view.resize(460,600); QTest::qWait(250);
    auto *toolbar=view.findChild<QWidget *>(QStringLiteral("pdfToolbar"));
    bool controlsFit=view.width()==460;
    for(auto *b:toolbar->findChildren<QToolButton *>())
        controlsFit &= toolbar->rect().contains(QRect(b->mapTo(toolbar,QPoint()),b->size()));
    check(controlsFit,"compact toolbar keeps all controls inside window");
    if(app.arguments().size()>1) view.grab().save(QCoreApplication::applicationDirPath()+QStringLiteral("/pdf-narrow.png"));
    const int documentArg=app.arguments().indexOf(QStringLiteral("--document"));
    if(documentArg>0 && documentArg+1<app.arguments().size()) {
        PdfViewWidget documentView;
        documentView.setAttribute(Qt::WA_DontShowOnScreen);
        documentView.resize(1380,980); documentView.show();
        documentView.load(app.arguments().at(documentArg+1));
        auto *quick=documentView.findChild<QQuickWidget *>();
        auto *root=quick->rootObject();
        waitFor([&]{return root->property("pageReady").toBool();}); QTest::qWait(800);
        documentView.grab().save(QCoreApplication::applicationDirPath()+QStringLiteral("/pdf-document-review.png"));
        const QImage high=quick->grab().toImage();
        for(auto *timer:documentView.findChildren<QTimer *>()) timer->stop();
        QList<QQuickItem *> images{root};
        for(int i=0;i<images.size();++i) {
            auto *img=images[i]; images.append(img->childItems());
            if(QString::fromLatin1(img->metaObject()->className()).contains("PdfPageImage"))
                img->setProperty("sourceSize",QSize(qRound(img->width()*quick->devicePixelRatioF()),0));
        }
        QTest::qWait(800);
        const QImage baseline=quick->grab().toImage();
        const QRect sample(110,36,640,160);
        high.copy(sample).save(QCoreApplication::applicationDirPath()+QStringLiteral("/pdf-detail-high.png"));
        baseline.copy(sample).save(QCoreApplication::applicationDirPath()+QStringLiteral("/pdf-detail-baseline.png"));
    }
    out << passed << " PDF checks passed; " << failed << " failed.\n";
    result = failed ? 1 : 0;
    app.exit(result);
    });
    return app.exec();
}
