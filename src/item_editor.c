#include <limits.h>

static bool auth_has_admin_access(Shop* shop) {
    if (!shop_is_admin(shop)) return false;

    sqlite3_stmt* statement = NULL;
    const char* sql = "SELECT 1 FROM users WHERE id = ?1 AND is_admin = 1;";
    if (sqlite3_prepare_v2(shop->admin.db, sql, -1, &statement, NULL) != SQLITE_OK) {
        return false;
    }
    bool allowed = sqlite3_bind_int(statement, 1, shop->account.user_id) == SQLITE_OK &&
                   sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return allowed;
}

void open_item_editor(Shop* shop, Item item) {
    if (!auth_has_admin_access(shop)) return;

    shop->item_editor = (Item_Editor){
        .active = true,
        .on_sale = item_is_on_sale(item),
        .item = item,
        .stock = item.stock,
    };
    shop->search_bar_active = false;
    shop->paused = true;
}

static bool item_editor_error(Item_Editor* editor, const char* message) {
    snprintf(editor->error, sizeof(editor->error), "%s", message);
    return false;
}

static void update_edited_item(Item_List* list, Item item) {
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].id == item.id) {
            if (list->category_filter && strcmp(list->category_filter, item.category) != 0) {
                memmove(&list->items[i], &list->items[i + 1],
                        (list->count - i - 1) * sizeof(*list->items));
                list->count--;
            } else {
                list->items[i] = item;
            }
            return;
        }
    }
}

static bool save_item_edit(Shop* shop) {
    Item_Editor* editor = &shop->item_editor;
    Item item = editor->item;
    if (!shop_is_admin(shop)) {
        return item_editor_error(editor, "An admin account is required to save changes.");
    }
    if (item.display[strspn(item.display, " \t\r\n")] == '\0') {
        return item_editor_error(editor, "Enter a product name.");
    }
    if (!isfinite(item.price) || item.price < 0.0 || item.price > FLT_MAX) {
        return item_editor_error(editor, "Enter a valid price of $0.00 or more.");
    }
    item.price = round(item.price * 100.0) / 100.0;
    if (editor->on_sale) {
        item.sale_price = round(item.sale_price * 100.0) / 100.0;
        if (!isfinite(item.sale_price) || item.sale_price < 0.01 || item.sale_price >= item.price) {
            return item_editor_error(editor, "Enter a sale price of at least $0.01 and below the regular price.");
        }
    } else {
        item.sale_price = 0.0;
    }
    if (editor->stock < 0 || editor->stock > INT_MAX) {
        return item_editor_error(editor, "Enter a stock quantity between 0 and 2,147,483,647.");
    }

    bool stock_changed = editor->stock != item.stock;
    sqlite3_stmt* statement = NULL;
    sqlite3* db = shop->admin.db;
    const char* sql =
        "UPDATE items SET display = ?1, description = ?2, price = ?3, sale_price = ?4, "
        "stock = CASE WHEN ?5 THEN ?6 ELSE stock END, category = ?10 "
        "WHERE id = ?7 AND (NOT ?5 OR stock = ?8) "
        "AND EXISTS (SELECT 1 FROM users WHERE id = ?9 AND is_admin = 1);";
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) {
        return item_editor_error(editor, "Could not save this item. Try again.");
    }
    bool saved =
        sqlite3_bind_text(statement, 1, item.display, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, item.description, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_double(statement, 3, item.price) == SQLITE_OK &&
        sqlite3_bind_double(statement, 4, item.sale_price) == SQLITE_OK &&
        sqlite3_bind_int(statement, 5, stock_changed) == SQLITE_OK &&
        sqlite3_bind_int64(statement, 6, editor->stock) == SQLITE_OK &&
        sqlite3_bind_int(statement, 7, item.id) == SQLITE_OK &&
        sqlite3_bind_int(statement, 8, item.stock) == SQLITE_OK &&
        sqlite3_bind_int(statement, 9, shop->account.user_id) == SQLITE_OK &&
        sqlite3_bind_text(statement, 10, item.category, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_DONE;
    int updated = saved ? sqlite3_changes(db) : 0;
    sqlite3_finalize(statement);
    if (!saved) {
        return item_editor_error(editor, "Could not save this item. Try again.");
    }
    if (updated != 1) {
        return item_editor_error(editor, "Item availability, stock, or admin access changed. Reopen the editor and try again.");
    }

    item.stock = item_available_stock(shop, item.id);
    update_edited_item(&shop->display, item);
    update_edited_item(&shop->featured, item);
    return true;
}

static void item_category_dropdown(Item* item) {
    static const struct {
        const char* value;
        const char* label;
    } categories[] = {
        {ITEM_CATEGORY_CHAIRS, "Chairs"},
        {ITEM_CATEGORY_FURNITURE, "Other Furnishings"},
        {ITEM_CATEGORY_LARGE, "Larger Items"},
    };
    const char* preview = item->category;
    for (size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); i++) {
        if (strcmp(item->category, categories[i].value) == 0) {
            preview = categories[i].label;
            break;
        }
    }

    ImGui_TextDisabled("Category");
    ImGui_SetNextItemWidth(-FLT_MIN);
    if (ImGui_BeginCombo("##item_category", preview, 0)) {
        for (size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); i++) {
            bool selected = strcmp(item->category, categories[i].value) == 0;
            if (ImGui_SelectableEx(categories[i].label, selected, 0, (ImVec2){0})) {
                snprintf(item->category, sizeof(item->category), "%s", categories[i].value);
            }
            if (selected) {
                ImGui_SetItemDefaultFocus();
            }
        }
        ImGui_EndCombo();
    }
}

static void item_editor_panel(Shop* shop) {
    Item_Editor* editor = &shop->item_editor;
    Item* item = &editor->item;
    ImVec2 viewport = ImGui_GetIO()->DisplaySize;
    ImVec2 panel_size = {fminf(520.0f, viewport.x - 24.0f),
                         fminf(560.0f, viewport.y - 24.0f)};
    ImGui_SetNextWindowPosEx((ImVec2){viewport.x * 0.5f, viewport.y * 0.5f},
                            ImGuiCond_Always, (ImVec2){0.5f, 0.5f});
    ImGui_SetNextWindowSize(panel_size, ImGuiCond_Always);

    ImGui_PushStyleVarImVec2(ImGuiStyleVar_WindowPadding, (ImVec2){32.0f, 26.0f});
    ImGui_PushStyleVarImVec2(ImGuiStyleVar_ItemSpacing, (ImVec2){10.0f, 10.0f});
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui_Begin("Edit Item##item_editor", NULL, flags)) {
        ImGui_TextDisabled("Name");
        ImGui_SetNextItemWidth(-FLT_MIN);
        ImGui_InputText("##item_name", item->display, sizeof(item->display), 0);

        item_category_dropdown(item);

        ImGui_TextDisabled("Description");
        ImGui_InputTextMultilineEx("##item_description", item->description, sizeof(item->description),
                                   (ImVec2){-FLT_MIN, 80.0f}, ImGuiInputTextFlags_WordWrap, NULL, NULL);

        ImGui_TextDisabled("Price ($)");
        ImGui_SetNextItemWidth(-FLT_MIN);
        ImGui_InputDoubleEx("##item_price", &item->price, 0.0, 0.0, "%.2f", 0);

        ImGui_TextDisabled("Stock");
        ImGui_SetNextItemWidth(-FLT_MIN);
        ImGui_InputScalar("##item_stock", ImGuiDataType_S64, &editor->stock);

        ImGui_Checkbox("On sale", &editor->on_sale);
        ImGui_TextDisabled("Sale price ($)");
        ImGui_BeginDisabled(!editor->on_sale);
        ImGui_SetNextItemWidth(-FLT_MIN);
        ImGui_InputDoubleEx("##item_sale_price", &item->sale_price, 0.0, 0.0, "%.2f", 0);
        ImGui_EndDisabled();

        if (editor->error[0]) {
            ImGui_TextWrapped("%s", editor->error);
        }
        ImGui_Separator();
        ImVec2 save_size = {136.0f, 34.0f};
        ImVec2 cancel_size = {88.0f, 34.0f};
        float row_width = save_size.x + ImGui_GetStyle()->ItemSpacing.x + cancel_size.x;
        float left_padding = (ImGui_GetContentRegionAvail().x - row_width) * 0.5f;
        ImGui_SetCursorPosX(ImGui_GetCursorPosX() + fmaxf(0.0f, left_padding));

        if (ImGui_ButtonEx("Save Changes", save_size) && save_item_edit(shop)) {
            editor->active = false;
        }
        ImGui_SameLine();
        if (ImGui_ButtonEx("Cancel", cancel_size)) {
            editor->active = false;
        }
    }
    ImGui_End();
    ImGui_PopStyleVarEx(2);
}
