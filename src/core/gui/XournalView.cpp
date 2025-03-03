#include "XournalView.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <tuple>

#include <gdk/gdk.h>

#include "control/Control.h"
#include "control/PdfCache.h"
#include "control/settings/MetadataManager.h"
#include "gui/PdfFloatingToolbox.h"
#include "gui/inputdevices/HandRecognition.h"
#include "gui/widgets/XournalWidget.h"
#include "model/Document.h"
#include "model/Stroke.h"
#include "undo/DeleteUndoAction.h"
#include "util/Rectangle.h"
#include "util/Util.h"

#include "Layout.h"
#include "PageView.h"
#include "RepaintHandler.h"
#include "Shadow.h"
#include "XournalppCursor.h"
#include "filesystem.h"

using xoj::util::Rectangle;

std::pair<size_t, size_t> XournalView::preloadPageBounds(size_t page, size_t maxPage) {
    const size_t preloadBefore = this->control->getSettings()->getPreloadPagesBefore();
    const size_t preloadAfter = this->control->getSettings()->getPreloadPagesAfter();
    const size_t lower = page > preloadBefore ? page - preloadBefore : 0;
    const size_t upper = std::min(maxPage, page + preloadAfter);
    return {lower, upper};
}

XournalView::XournalView(GtkWidget* parent, Control* control, ScrollHandling* scrollHandling):
        scrollHandling(scrollHandling), control(control) {
    this->cache = new PdfCache(control->getSettings()->getPdfPageCacheSize());

    registerListener(control);

    InputContext* inputContext = new InputContext(this, scrollHandling);
    this->widget = gtk_xournal_new(this, inputContext);
    // we need to refer widget here, because we unref it somewhere twice!?
    g_object_ref(this->widget);

    gtk_container_add(GTK_CONTAINER(parent), this->widget);
    gtk_widget_show(this->widget);

    g_signal_connect(getWidget(), "realize", G_CALLBACK(onRealized), this);

    this->repaintHandler = new RepaintHandler(this);
    this->handRecognition = new HandRecognition(this->widget, inputContext, control->getSettings());

    control->getZoomControl()->addZoomListener(this);

    gtk_widget_set_can_default(this->widget, true);
    gtk_widget_grab_default(this->widget);

    gtk_widget_grab_focus(this->widget);

    this->cleanupTimeout = g_timeout_add_seconds(5, reinterpret_cast<GSourceFunc>(clearMemoryTimer), this);
}

XournalView::~XournalView() {
    g_source_remove(this->cleanupTimeout);

    for (auto&& page: viewPages) { delete page; }
    viewPages.clear();

    delete this->cache;
    this->cache = nullptr;
    delete this->repaintHandler;
    this->repaintHandler = nullptr;

    gtk_widget_destroy(this->widget);
    this->widget = nullptr;

    delete this->handRecognition;
    this->handRecognition = nullptr;
}

auto pageViewIncreasingClockTime(XojPageView* a, XojPageView* b) -> gint {
    return a->getLastVisibleTime() - b->getLastVisibleTime();  // >0 will put a after b
}

void XournalView::staticLayoutPages(GtkWidget* widget, GtkAllocation* allocation, void* data) {
    auto* xv = static_cast<XournalView*>(data);
    xv->layoutPages();
}


auto XournalView::clearMemoryTimer(XournalView* widget) -> gboolean {
    widget->cleanupBufferCache();
    return true;
}

auto XournalView::cleanupBufferCache() -> void {
    const auto& [pagesLower, pagesUpper] = this->preloadPageBounds(this->currentPage, this->viewPages.size());
    g_assert(pagesLower <= pagesUpper);

    for (size_t i = 0; i < this->viewPages.size(); i++) {
        auto&& page = this->viewPages[i];
        const size_t pageNum = i + 1;
        const bool isPreload = pagesLower <= pageNum && pageNum <= pagesUpper;
        if (!isPreload && page->getLastVisibleTime() > 0 && page->getBufferPixels() > 0) {
            page->deleteViewBuffer();
        }
    }
}

auto XournalView::getCurrentPage() const -> size_t { return currentPage; }

const int scrollKeySize = 30;

auto XournalView::onKeyPressEvent(GdkEventKey* event) -> bool {
    size_t p = getCurrentPage();
    if (p != npos && p < this->viewPages.size()) {
        XojPageView* v = this->viewPages[p];
        if (v->onKeyPressEvent(event)) {
            return true;
        }
    }

    // Esc leaves fullscreen mode
    if (event->keyval == GDK_KEY_Escape) {
        if (control->isFullscreen()) {
            control->setFullscreen(false);
            return true;
        }
    }

    // F5 starts presentation modus
    if (event->keyval == GDK_KEY_F5) {
        if (!control->isFullscreen()) {
            control->setViewPresentationMode(true);
            control->setFullscreen(true);
            return true;
        }
    }

    guint state = event->state & gtk_accelerator_get_default_mod_mask();

    Layout* layout = gtk_xournal_get_layout(this->widget);

    if (state & GDK_SHIFT_MASK) {
        GtkAllocation alloc = {0};
        gtk_widget_get_allocation(gtk_widget_get_parent(this->widget), &alloc);
        int windowHeight = alloc.height - scrollKeySize;

        if (event->keyval == GDK_KEY_Page_Down) {
            layout->scrollRelative(0, windowHeight);
            return false;
        }
        if (event->keyval == GDK_KEY_Page_Up || event->keyval == GDK_KEY_space) {
            layout->scrollRelative(0, -windowHeight);
            return true;
        }
    } else {
        if (event->keyval == GDK_KEY_Page_Down || event->keyval == GDK_KEY_KP_Page_Down) {
            control->getScrollHandler()->goToNextPage();
            return true;
        }
        if (event->keyval == GDK_KEY_Page_Up || event->keyval == GDK_KEY_KP_Page_Up) {
            control->getScrollHandler()->goToPreviousPage();
            return true;
        }
    }

    if (event->keyval == GDK_KEY_space) {
        GtkAllocation alloc = {0};
        gtk_widget_get_allocation(gtk_widget_get_parent(this->widget), &alloc);
        int windowHeight = alloc.height - scrollKeySize;

        layout->scrollRelative(0, windowHeight);
        return true;
    }

    // Numeric keypad always navigates by page
    if (event->keyval == GDK_KEY_KP_Up) {
        this->pageRelativeXY(0, -1);
        return true;
    }

    if (event->keyval == GDK_KEY_KP_Down) {
        this->pageRelativeXY(0, 1);
        return true;
    }

    if (event->keyval == GDK_KEY_KP_Left) {
        this->pageRelativeXY(-1, 0);
        return true;
    }

    if (event->keyval == GDK_KEY_KP_Right) {
        this->pageRelativeXY(1, 0);
        return true;
    }


    if (event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_k) {
        if (control->getSettings()->isPresentationMode()) {
            control->getScrollHandler()->goToPreviousPage();
            return true;
        }


        if (state & GDK_SHIFT_MASK) {
            this->pageRelativeXY(0, -1);
        } else {
            layout->scrollRelative(0, -scrollKeySize);
        }
        return true;
    }

    if (event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_j) {
        if (control->getSettings()->isPresentationMode()) {
            control->getScrollHandler()->goToNextPage();
            return true;
        }


        if (state & GDK_SHIFT_MASK) {
            this->pageRelativeXY(0, 1);
        } else {
            layout->scrollRelative(0, scrollKeySize);
        }
        return true;
    }

    if (event->keyval == GDK_KEY_Left || event->keyval == GDK_KEY_h) {
        if (state & GDK_SHIFT_MASK) {
            this->pageRelativeXY(-1, 0);
        } else {
            if (control->getSettings()->isPresentationMode()) {
                control->getScrollHandler()->goToPreviousPage();
            } else {
                layout->scrollRelative(-scrollKeySize, 0);
            }
        }
        return true;
    }

    if (event->keyval == GDK_KEY_Right || event->keyval == GDK_KEY_l) {
        if (state & GDK_SHIFT_MASK) {
            this->pageRelativeXY(1, 0);
        } else {
            if (control->getSettings()->isPresentationMode()) {
                control->getScrollHandler()->goToNextPage();
            } else {
                layout->scrollRelative(scrollKeySize, 0);
            }
        }
        return true;
    }

    if (event->keyval == GDK_KEY_End || event->keyval == GDK_KEY_KP_End) {
        control->getScrollHandler()->goToLastPage();
        return true;
    }

    if (event->keyval == GDK_KEY_Home || event->keyval == GDK_KEY_KP_Home) {
        control->getScrollHandler()->goToFirstPage();
        return true;
    }

    return false;
}

auto XournalView::getRepaintHandler() -> RepaintHandler* { return this->repaintHandler; }

auto XournalView::onKeyReleaseEvent(GdkEventKey* event) -> bool {
    size_t p = getCurrentPage();
    if (p != npos && p < this->viewPages.size()) {
        XojPageView* v = this->viewPages[p];
        if (v->onKeyReleaseEvent(event)) {
            return true;
        }
    }

    return false;
}

void XournalView::onRealized(GtkWidget* widget, XournalView* view) {
    // Disable event compression
    if (gtk_widget_get_realized(view->getWidget())) {
        gdk_window_set_event_compression(gtk_widget_get_window(view->getWidget()), false);
    } else {
        g_warning("could not disable event compression");
    }
}

// send the focus back to the appropriate widget
void XournalView::requestFocus() { gtk_widget_grab_focus(this->widget); }

auto XournalView::searchTextOnPage(std::string text, size_t p, int* occures, double* top) -> bool {
    if (p == npos || p >= this->viewPages.size()) {
        return false;
    }
    XojPageView* v = this->viewPages[p];

    return v->searchTextOnPage(text, occures, top);
}

void XournalView::forceUpdatePagenumbers() {
    size_t p = this->currentPage;
    this->currentPage = npos;

    control->firePageSelected(p);
}

auto XournalView::getViewFor(size_t pageNr) -> XojPageView* {
    if (pageNr == npos || pageNr >= this->viewPages.size()) {
        return nullptr;
    }
    return this->viewPages[pageNr];
}

void XournalView::pageSelected(size_t page) {
    if (this->currentPage == page && this->lastSelectedPage == page) {
        return;
    }

    Document* doc = control->getDocument();
    doc->lock();
    auto const& file = doc->getEvMetadataFilename();
    doc->unlock();

    control->getMetadataManager()->storeMetadata(file, page, getZoom());

    control->getWindow()->getPdfToolbox()->userCancelSelection();

    if (this->lastSelectedPage != npos && this->lastSelectedPage < this->viewPages.size()) {
        this->viewPages[this->lastSelectedPage]->setSelected(false);
    }

    this->currentPage = page;

    size_t pdfPage = npos;

    if (page != npos && page < viewPages.size()) {
        XojPageView* vp = viewPages[page];
        vp->setSelected(true);
        lastSelectedPage = page;
        pdfPage = vp->getPage()->getPdfPageNr();
    }

    control->updatePageNumbers(currentPage, pdfPage);

    control->updateBackgroundSizeButton();

    if (control->getSettings()->isEagerPageCleanup()) {
        this->cleanupBufferCache();
    }

    // Load surrounding pages if they are not
    const auto& [pagesLower, pagesUpper] = preloadPageBounds(page, this->viewPages.size());
    g_assert(pagesLower <= pagesUpper);
    for (size_t i = pagesLower; i < pagesUpper; i++) {
        if (this->viewPages[i]->getBufferPixels() == 0) {
            this->viewPages[i]->rerenderPage();
        }
    }
}

auto XournalView::getControl() -> Control* { return control; }

void XournalView::scrollTo(size_t pageNo, double yDocument) {
    if (pageNo >= this->viewPages.size()) {
        return;
    }

    XojPageView* v = this->viewPages[pageNo];

    // Make sure it is visible
    Layout* layout = gtk_xournal_get_layout(this->widget);

    int x = v->getX();
    int y = v->getY() + std::lround(yDocument);
    int width = v->getDisplayWidth();
    int height = v->getDisplayHeight();

    layout->ensureRectIsVisible(x, y, width, height);

    // Select the page
    control->firePageSelected(pageNo);
}


void XournalView::pageRelativeXY(int offCol, int offRow) {
    size_t currPage = getCurrentPage();

    XojPageView* view = getViewFor(currPage);
    int row = view->getMappedRow();
    int col = view->getMappedCol();

    Layout* layout = gtk_xournal_get_layout(this->widget);
    auto optionalPageIndex = layout->getPageIndexAtGridMap(row + offRow, col + offCol);
    if (optionalPageIndex) {
        this->scrollTo(*optionalPageIndex, 0);
    }
}


void XournalView::endTextAllPages(XojPageView* except) {
    for (auto v: this->viewPages) {
        if (except != v) {
            v->endText();
        }
    }
}

void XournalView::layerChanged(size_t page) {
    if (page != npos && page < this->viewPages.size()) {
        this->viewPages[page]->rerenderPage();
    }
}

void XournalView::getPasteTarget(double& x, double& y) {
    size_t pageNo = getCurrentPage();
    if (pageNo == npos) {
        return;
    }

    Rectangle<double>* rect = getVisibleRect(pageNo);

    if (rect) {
        x = rect->x + rect->width / 2;
        y = rect->y + rect->height / 2;
        delete rect;
    }
}

/**
 * Return the rectangle which is visible on screen, in document cooordinates
 *
 * Or nullptr if the page is not visible
 */
auto XournalView::getVisibleRect(size_t page) -> Rectangle<double>* {
    if (page == npos || page >= this->viewPages.size()) {
        return nullptr;
    }
    XojPageView* p = this->viewPages[page];

    return getVisibleRect(p);
}

auto XournalView::getVisibleRect(XojPageView* redrawable) -> Rectangle<double>* {
    return gtk_xournal_get_visible_area(this->widget, redrawable);
}

/**
 * @return Helper class for Touch specific fixes
 */
auto XournalView::getHandRecognition() -> HandRecognition* { return handRecognition; }

/**
 * @return Scrollbars
 */
auto XournalView::getScrollHandling() -> ScrollHandling* { return scrollHandling; }

auto XournalView::getWidget() -> GtkWidget* { return widget; }

void XournalView::ensureRectIsVisible(int x, int y, int width, int height) {
    Layout* layout = gtk_xournal_get_layout(this->widget);
    layout->ensureRectIsVisible(x, y, width, height);
}

void XournalView::zoomChanged() {

    size_t currentPage = this->getCurrentPage();
    XojPageView* view = getViewFor(currentPage);

    ZoomControl* zoom = control->getZoomControl();
    this->cache->setAnyZoomChangeCausesRecache(false);  // We're zooming -- no need for repeated re-renders.
    this->cache->setRefreshThreshold(control->getSettings()->getPDFPageRerenderThreshold());

    if (!view) {
        return;
    }

    layoutPages();

    if (zoom->isZoomPresentationMode() || zoom->isZoomFitMode()) {
        scrollTo(currentPage);
    } else {
        auto pos = zoom->getScrollPositionAfterZoom();
        if (pos.x != -1 && pos.y != -1) {
            Layout* layout = gtk_xournal_get_layout(this->widget);
            layout->scrollAbs(pos.x, pos.y);
        }
    }

    Document* doc = control->getDocument();
    doc->lock();
    auto const& file = doc->getEvMetadataFilename();
    doc->unlock();

    control->getMetadataManager()->storeMetadata(file, getCurrentPage(), zoom->getZoomReal());

    // Updates the Eraser's cursor icon in order to make it as big as the erasing area
    control->getCursor()->updateCursor();

    // if we changed the zoom of the page, we should hide the pdf floating toolbox
    // and if user clicked the selection again, the floating toolbox shows again
    control->getWindow()->getPdfToolbox()->hide();

    this->control->getScheduler()->blockRerenderZoom();
}

void XournalView::pageSizeChanged(size_t page) {
    layoutPages();
    if (page != npos && page < this->viewPages.size()) {
        this->viewPages[page]->rerenderPage();
    }
}

void XournalView::pageChanged(size_t page) {
    if (page != npos && page < this->viewPages.size()) {
        this->viewPages[page]->rerenderPage();
    }
}

void XournalView::pageDeleted(size_t page) {
    size_t currentPage = control->getCurrentPageNo();

    delete this->viewPages[page];
    viewPages.erase(begin(viewPages) + page);

    layoutPages();
    control->getScrollHandler()->scrollToPage(currentPage);
}

auto XournalView::getTextEditor() -> TextEditor* {
    for (auto&& page: viewPages) {
        if (page->getTextEditor()) {
            return page->getTextEditor();
        }
    }

    return nullptr;
}

auto XournalView::getCache() -> PdfCache* { return this->cache; }

void XournalView::pageInserted(size_t page) {
    Document* doc = control->getDocument();
    doc->lock();
    auto* pageView = new XojPageView(this, doc->getPage(page));
    doc->unlock();

    viewPages.insert(begin(viewPages) + page, pageView);

    layoutPages();
    // check which pages are visible and select the most visible page
    Layout* layout = gtk_xournal_get_layout(this->widget);
    layout->updateVisibility();
}

auto XournalView::getZoom() -> double { return control->getZoomControl()->getZoom(); }

auto XournalView::getDpiScaleFactor() -> int { return gtk_widget_get_scale_factor(widget); }

void XournalView::clearSelection() {
    EditSelection* sel = GTK_XOURNAL(widget)->selection;
    GTK_XOURNAL(widget)->selection = nullptr;
    delete sel;

    control->setClipboardHandlerSelection(getSelection());

    getCursor()->setMouseSelectionType(CURSOR_SELECTION_NONE);
    control->getToolHandler()->setSelectionEditTools(false, false, false);
}

void XournalView::deleteSelection(EditSelection* sel) {
    if (sel == nullptr) {
        sel = getSelection();
    }

    if (sel) {
        XojPageView* view = sel->getView();
        auto undo = std::make_unique<DeleteUndoAction>(sel->getSourcePage(), false);
        sel->fillUndoItem(undo.get());
        control->getUndoRedoHandler()->addUndoAction(std::move(undo));

        clearSelection();

        view->rerenderPage();
        repaintSelection(true);
    }
}

void XournalView::setSelection(EditSelection* selection) {
    clearSelection();
    GTK_XOURNAL(this->widget)->selection = selection;

    control->setClipboardHandlerSelection(getSelection());

    bool canChangeSize = false;
    bool canChangeColor = false;
    bool canChangeFill = false;

    for (Element* e: *selection->getElements()) {
        if (e->getType() == ELEMENT_TEXT) {
            canChangeColor = true;
        } else if (e->getType() == ELEMENT_STROKE) {
            auto* s = dynamic_cast<Stroke*>(e);
            if (s->getToolType() != STROKE_TOOL_ERASER) {
                canChangeColor = true;
                canChangeFill = true;
            }
            canChangeSize = true;
        }

        if (canChangeColor && canChangeSize && canChangeFill) {
            break;
        }
    }

    control->getToolHandler()->setSelectionEditTools(canChangeColor, canChangeSize, canChangeFill);

    repaintSelection();
}

void XournalView::repaintSelection(bool evenWithoutSelection) {
    if (evenWithoutSelection) {
        gtk_widget_queue_draw(this->widget);
        return;
    }

    EditSelection* selection = getSelection();
    if (selection == nullptr) {
        return;
    }

    // repaint always the whole widget
    gtk_widget_queue_draw(this->widget);
}

void XournalView::setSetsquareView(std::unique_ptr<SetsquareView> setsquareView) {
    GTK_XOURNAL(this->widget)->setsquareView = std::move(setsquareView);
}

void XournalView::resetSetsquareView() {
    GTK_XOURNAL(this->widget)->setsquareView.reset();
    this->control->fireActionSelected(GROUP_SETSQUARE, ACTION_NONE);
}

auto XournalView::getSetsquareView() -> SetsquareView* {
    g_return_val_if_fail(this->widget != nullptr, nullptr);
    g_return_val_if_fail(GTK_IS_XOURNAL(this->widget), nullptr);

    return GTK_XOURNAL(this->widget)->setsquareView.get();
}

void XournalView::repaintSetsquare(bool evenWithoutSetsquare) {
    if (getSetsquareView() || evenWithoutSetsquare) {
        // repaint always the whole widget
        gtk_widget_queue_draw(this->widget);
    }
    return;
}

void XournalView::layoutPages() {
    Layout* layout = gtk_xournal_get_layout(this->widget);
    layout->recalculate();

    // Todo (fabian): the following lines are conceptually wrong, the Layout::layoutPages function is meant to be called
    //                by an expose event, but removing it, will break "add page".
    auto rectangle = layout->getVisibleRect();
    layout->layoutPages(std::max<int>(layout->getMinimalWidth(), std::lround(rectangle.width)),
                        std::max<int>(layout->getMinimalHeight(), std::lround(rectangle.height)));
}

auto XournalView::getDisplayHeight() const -> int {
    GtkAllocation allocation = {0};
    gtk_widget_get_allocation(this->widget, &allocation);
    return allocation.height;
}

auto XournalView::getDisplayWidth() const -> int {
    GtkAllocation allocation = {0};
    gtk_widget_get_allocation(this->widget, &allocation);
    return allocation.width;
}

auto XournalView::isPageVisible(size_t page, int* visibleHeight) -> bool {
    Rectangle<double>* rect = getVisibleRect(page);
    if (rect) {
        if (visibleHeight) {
            *visibleHeight = std::lround(rect->height);
        }

        delete rect;
        return true;
    }
    if (visibleHeight) {
        *visibleHeight = 0;
    }

    return false;
}

void XournalView::documentChanged(DocumentChangeType type) {
    if (type != DOCUMENT_CHANGE_CLEARED && type != DOCUMENT_CHANGE_COMPLETE) {
        return;
    }

    XournalScheduler* scheduler = this->control->getScheduler();
    scheduler->lock();
    scheduler->removeAllJobs();

    clearSelection();

    for (auto&& page: viewPages) { delete page; }
    viewPages.clear();

    this->cache->clearCache();

    Document* doc = control->getDocument();
    doc->lock();

    size_t pagecount = doc->getPageCount();
    viewPages.reserve(pagecount);
    for (size_t i = 0; i < pagecount; i++) { viewPages.push_back(new XojPageView(this, doc->getPage(i))); }

    doc->unlock();

    layoutPages();
    scrollTo(0, 0);

    scheduler->unlock();
}

auto XournalView::cut() -> bool {
    size_t p = getCurrentPage();
    if (p == npos || p >= viewPages.size()) {
        return false;
    }

    XojPageView* page = viewPages[p];
    return page->cut();
}

auto XournalView::copy() -> bool {
    size_t p = getCurrentPage();
    if (p == npos || p >= viewPages.size()) {
        return false;
    }

    XojPageView* page = viewPages[p];
    return page->copy();
}

auto XournalView::paste() -> bool {
    size_t p = getCurrentPage();
    if (p == npos || p >= viewPages.size()) {
        return false;
    }

    XojPageView* page = viewPages[p];
    return page->paste();
}

auto XournalView::actionDelete() -> bool {
    size_t p = getCurrentPage();
    if (p == npos || p >= viewPages.size()) {
        return false;
    }

    XojPageView* page = viewPages[p];
    return page->actionDelete();
}

auto XournalView::getDocument() -> Document* { return control->getDocument(); }

auto XournalView::getViewPages() const -> std::vector<XojPageView*> const& { return viewPages; }

auto XournalView::getCursor() -> XournalppCursor* { return control->getCursor(); }

auto XournalView::getSelection() -> EditSelection* {
    g_return_val_if_fail(this->widget != nullptr, nullptr);
    g_return_val_if_fail(GTK_IS_XOURNAL(this->widget), nullptr);

    return GTK_XOURNAL(this->widget)->selection;
}
