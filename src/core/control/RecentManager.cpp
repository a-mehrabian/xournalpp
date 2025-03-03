#include "RecentManager.h"

#include <array>

#include <filesystem.h>

#include "util/PathUtil.h"
#include "util/StringUtils.h"
#include "util/i18n.h"
#include "util/safe_casts.h"

#define MIME "application/x-xoj"
#define MIME_PDF "application/x-pdf"
#define GROUP "xournal++"

using std::string;

RecentManagerListener::~RecentManagerListener() = default;

RecentManager::RecentManager() {
    this->menu = gtk_menu_new();

    GtkRecentManager* recentManager = gtk_recent_manager_get_default();
    this->recentHandlerId = g_signal_connect(recentManager, "changed", G_CALLBACK(recentManagerChangedCallback), this);

    updateMenu();
}

RecentManager::~RecentManager() {
    if (this->recentHandlerId) {
        GtkRecentManager* recentManager = gtk_recent_manager_get_default();
        g_signal_handler_disconnect(recentManager, this->recentHandlerId);
        this->recentHandlerId = 0;
    }
    this->menu = nullptr;
}

void RecentManager::addListener(RecentManagerListener* l) { this->listener.push_back(l); }

void RecentManager::recentManagerChangedCallback(GtkRecentManager* /*manager*/, RecentManager* recentManager) {
    // regenerate the menu when the model changes
    recentManager->updateMenu();
}

void RecentManager::addRecentFileFilename(const fs::path& filepath) {
    GtkRecentManager* recentManager = gtk_recent_manager_get_default();

    std::string group_name = GROUP;
    std::array<gchar*, 2> groups = {group_name.data(), nullptr};
    std::string app_name = g_get_application_name();
    std::string app_exec = std::string(g_get_prgname()) + " %u";
    std::string mime_type = Util::hasPdfFileExt(filepath) ? std::string(MIME_PDF) : std::string(MIME);

    GtkRecentData recentData{};
    recentData.display_name = nullptr;
    recentData.description = nullptr;
    recentData.app_name = app_name.data();
    recentData.app_exec = app_exec.data();
    recentData.groups = groups.data();
    recentData.mime_type = mime_type.data();
    recentData.is_private = false;

    auto uri = Util::toUri(filepath);
    if (!uri) {
        return;
    }
    gtk_recent_manager_add_full(recentManager, (*uri).c_str(), &recentData);
}

void RecentManager::removeRecentFileFilename(const fs::path& filename) {
    auto uri = Util::toUri(filename);
    if (!uri) {
        return;
    }
    GtkRecentManager* recentManager = gtk_recent_manager_get_default();
    gtk_recent_manager_remove_item(recentManager, uri->c_str(), nullptr);
}

void RecentManager::openRecent(const fs::path& p) {
    if (p.empty()) {
        return;
    }

    for (RecentManagerListener* l: this->listener) { l->fileOpened(p); }
}

auto RecentManager::getMenu() -> GtkWidget* { return menu; }

void RecentManager::freeOldMenus() {
    for (GtkWidget* w: menuItemList) { gtk_widget_destroy(w); }

    this->menuItemList.clear();
}

using stime_t = std::make_signed<time_t>;

// Todo: replace with <=> in c++ 20
auto RecentManager::sortRecentsEntries(GtkRecentInfo* a, GtkRecentInfo* b) -> gint {
    auto tp_a = gtk_recent_info_get_modified(a);
    auto tp_b = gtk_recent_info_get_modified(b);
    return tp_a != tp_b ? (tp_a < tp_b ? 1 : -1) : 0;
}

auto RecentManager::getMostRecent() -> GtkRecentInfo* {
    GList* filteredItemsXoj = filterRecent(gtk_recent_manager_get_items(gtk_recent_manager_get_default()), true);
    if (filteredItemsXoj == nullptr) {
        return nullptr;
    }
    auto mostRecent = static_cast<GtkRecentInfo*>(filteredItemsXoj->data);

    gtk_recent_info_ref(mostRecent);
    for (GList* l = filteredItemsXoj; l != nullptr; l = l->next) {
        gtk_recent_info_unref(static_cast<GtkRecentInfo*>(l->data));
    }
    g_list_free(filteredItemsXoj);

    return mostRecent;
}

auto RecentManager::filterRecent(GList* items, bool xoj) -> GList* {
    GList* filteredItems = nullptr;

    // filter
    for (GList* l = items; l != nullptr; l = l->next) {
        auto* info = static_cast<GtkRecentInfo*>(l->data);

        const gchar* uri = gtk_recent_info_get_uri(info);
        if (!uri)  // issue #1071
        {
            continue;
        }

        auto p = Util::fromUri(uri);

        // Skip remote files
        if (!p) {
            continue;
        }

        if (xoj && Util::hasXournalFileExt(*p)) {
            filteredItems = g_list_prepend(filteredItems, info);
        }
        if (!xoj && Util::hasPdfFileExt(*p)) {
            filteredItems = g_list_prepend(filteredItems, info);
        }
    }

    // sort
    filteredItems = g_list_sort(filteredItems, reinterpret_cast<GCompareFunc>(sortRecentsEntries));

    return filteredItems;
}

void RecentManager::recentsMenuActivateCallback(GtkAction* action, RecentManager* recentManager) {
    auto* info = static_cast<GtkRecentInfo*>(g_object_get_data(G_OBJECT(action), "gtk-recent-info"));
    g_return_if_fail(info != nullptr);

    auto p = Util::fromUri(gtk_recent_info_get_uri(info));
    if (p) {
        recentManager->openRecent(*p);
    }
}

void RecentManager::addRecentMenu(GtkRecentInfo* info, int i) {
    string display_name = gtk_recent_info_get_display_name(info);

    // escape underscore
    StringUtils::replaceAllChars(display_name, {replace_pair('_', "__")});

    string label =
            (i >= 10 ? FS(FORMAT_STR("{1}. {2}") % i % display_name) : FS(FORMAT_STR("_{1}. {2}") % i % display_name));

    /* gtk_recent_info_get_uri_display (info) is buggy and
     * works only for local files */
    GFile* gfile = g_file_new_for_uri(gtk_recent_info_get_uri(info));
    char* fileUri = g_file_get_parse_name(gfile);
    string ruri = fileUri;
    g_free(fileUri);

    g_object_unref(gfile);

    if (StringUtils::startsWith(ruri, "~/")) {
        ruri = string(g_get_home_dir()) + ruri.substr(1);
    }

    string tip = FS(C_F("{1} is a URI", "Open {1}") % ruri);


    GtkWidget* item = gtk_menu_item_new_with_mnemonic(label.c_str());

    gtk_widget_set_tooltip_text(item, tip.c_str());

    g_object_set_data_full(G_OBJECT(item), "gtk-recent-info", gtk_recent_info_ref(info),
                           reinterpret_cast<GDestroyNotify>(gtk_recent_info_unref));

    g_signal_connect(item, "activate", G_CALLBACK(recentsMenuActivateCallback), this);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_widget_set_visible(GTK_WIDGET(item), true);

    this->menuItemList.push_back(item);
}

void RecentManager::updateMenu() {
    GtkRecentManager* recentManager = gtk_recent_manager_get_default();
    GList* items = gtk_recent_manager_get_items(recentManager);
    GList* filteredItemsXoj = filterRecent(items, true);
    GList* filteredItemsPdf = filterRecent(items, false);

    freeOldMenus();

    int xojCount = 0;
    for (GList* l = filteredItemsXoj; l != nullptr; l = l->next) {
        auto* info = static_cast<GtkRecentInfo*>(l->data);

        if (xojCount >= maxRecent) {
            break;
        }
        xojCount++;

        addRecentMenu(info, xojCount);
    }
    g_list_free(filteredItemsXoj);

    GtkWidget* separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator);
    gtk_widget_set_visible(GTK_WIDGET(separator), true);

    this->menuItemList.push_back(separator);

    int pdfCount = 0;
    for (GList* l = filteredItemsPdf; l != nullptr; l = l->next) {
        auto* info = static_cast<GtkRecentInfo*>(l->data);

        if (pdfCount >= maxRecent) {
            break;
        }
        pdfCount++;

        addRecentMenu(info, pdfCount + xojCount);
    }
    g_list_free(filteredItemsPdf);

    g_list_foreach(items, reinterpret_cast<GFunc>(gtk_recent_info_unref), nullptr);
    g_list_free(items);
}
