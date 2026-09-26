import QtQuick
import QtQuick.Controls
import QtQuick.Pdf

Rectangle {
    id: root
    color: "#e5e7eb"
    property alias source: pdf.source
    property int currentPage: viewer.currentPage
    property real zoomFactor: viewer.renderScale
    property string selectedText: viewer.selectedText
    property int searchCount: viewer.searchModel.count
    property int searchIndex: viewer.searchModel.currentResult
    property alias searchText: viewer.searchString
    property int fitMode: 1 // 0: custom, 1: width, 2: page
    property bool pageReady: viewer.currentPageRenderingStatus === Image.Ready
    property var pageTable: null
    property bool horizontalOverflow: pageTable ? pageTable.contentWidth > pageTable.width + 1 : false

    Component.onCompleted: {
        // Qt 6.8's default provider adds a scrollbar width to an already full
        // viewport width, causing horizontal overflow even at fit-to-width.
        for (var i = 0; i < viewer.children.length; ++i) {
            var child = viewer.children[i]
            if (child instanceof TableView) {
                pageTable = child
                child.animate = false
                child.columnWidthProvider = function(column) {
                    return Math.max(pageTable.width, pdf.maxPageWidth * viewer.renderScale + 16)
                }
                child.ScrollBar.horizontal.policy = Qt.binding(function() {
                    return root.horizontalOverflow ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                })
                break
            }
        }
    }

    function fit() {
        if (pdf.status !== PdfDocument.Ready || width < 1 || height < 1) return
        if (fitMode === 1) viewer.scaleToWidth(width - 32, height - 24)
        else if (fitMode === 2) viewer.scaleToPage(width - 32, height - 24)
    }
    function setZoom(value) { fitMode = 0; viewer.renderScale = value }
    function scrollWheel(distance) {
        if (!pageTable || pdf.status !== PdfDocument.Ready) return false
        pageTable.cancelFlick()
        var top = pageTable.originY
        var bottom = top + Math.max(0, pageTable.contentHeight - pageTable.height)
        pageTable.contentY = Math.max(top, Math.min(bottom, pageTable.contentY + distance))
        return true
    }
    function setFit(mode) { fitMode = mode; fit() }
    function goToLocation(page, x, y) { viewer.goToLocation(page, Qt.point(x, y), 0) }
    function goToPage(page) {
        viewer.goToLocation(page, Qt.point(0, 0), 0)
        if (pageTable && pageTable.rows > page) {
            pageTable.positionViewAtRow(page, TableView.AlignTop)
            var item = pageTable.itemAtCell(0, page)
            if (item) pageTable.contentY = item.y
            if (fitMode !== 0) pageTable.contentX = 0
        }
    }
    function selectAll() { viewer.selectAll() }
    function searchNext() { viewer.searchForward() }
    function searchPrev() { viewer.searchBack() }
    function revealSearchResult() {
        if (!viewer.searchString || viewer.searchModel.count < 1) return
        var link = viewer.searchModel.currentResultLink
        if (link.valid) viewer.goToLocation(link.page, link.location, 0)
    }
    onSearchCountChanged: if (searchCount > 0) Qt.callLater(revealSearchResult)
    onWidthChanged: fitTimer.restart()
    onHeightChanged: fitTimer.restart()
    Timer { id: fitTimer; interval: 50; onTriggered: root.fit() }
    Connections {
        target: viewer.searchModel
        // The result index can change before its asynchronously computed link.
        // Navigate when the actual destination is available as well.
        function onCurrentResultLinkChanged() {
            Qt.callLater(root.revealSearchResult)
        }
    }

    PdfDocument {
        id: pdf
        onStatusChanged: if (pdf.status === PdfDocument.Ready) fitTimer.restart()
    }
    PdfMultiPageView {
        id: viewer
        objectName: "pdfPages"
        anchors.fill: parent
        anchors.margins: 8
        document: pdf
    }
}
