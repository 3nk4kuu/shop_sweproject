// Get the latest stock from the database.
int item_available_stock(Shop* shop, int item_id) {
    sqlite3_stmt* stmt = NULL;
    int stock = 0;
    if (sqlite3_prepare_v2(shop->admin.db,
            "SELECT stock FROM items WHERE id = ?1;", -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int(stmt, 1, item_id);
    if (sqlite3_step(stmt) == SQLITE_ROW)
        stock = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return stock > 0 ? stock : 0;
}

enum {
    CART_LEFT = 50,
    CART_UNIT_PRICE_RIGHT = 538,
    CART_QUANTITY_X = 680,
    CART_PRICE_WIDTH = 175,
    CART_ROWS_Y = 154,
    CART_ROW_HEIGHT = 52,
};

static Rectangle cart_minus_button_bounds(int y) {
    return (Rectangle){CART_QUANTITY_X - 88, y, 40, 30};
}

static Rectangle cart_plus_button_bounds(int y) {
    return (Rectangle){CART_QUANTITY_X + 48, y, 40, 30};
}

static int cart_visible_row_count(Shop* shop) {
    return (shop->render_target.texture.height - CART_ROWS_Y - 148) / CART_ROW_HEIGHT;
}

static void cart_remove_zero_quantity_items(Shop* shop) {
    Cart* cart = &shop->cart;
    int remaining = 0;
    for (int i = 0; i < cart->count; i++) {
        if (cart->items[i].quantity > 0) {
            cart->items[remaining++] = cart->items[i];
        }
    }
    cart->count = remaining;
    cart->first_visible_row = MIN(cart->first_visible_row,
                                  MAX(0, remaining - cart_visible_row_count(shop)));
}

static float cart_subtotal(Shop* shop) {
    float subtotal = 0.0f;
    for (int i = 0; i < shop->cart.count; i++) {
        Cart_Item cart_item = shop->cart.items[i];
        subtotal += item_final_price(cart_item.item) * cart_item.quantity;
    }
    return subtotal;
}

static void draw_cart_amount(float amount, int right, int y, int size, Color color, bool crossed_out) {
    char text[64];
    format_money(text, sizeof(text), amount);
    const char* label = crossed_out ? TextFormat("Was %s", text) : text;
    int width = MeasureText(label, size);
    if (width > CART_PRICE_WIDTH) {
        size = size * CART_PRICE_WIDTH / width;
    }
    DrawText(label, right - MeasureText(label, size), y, size, color);
    if (crossed_out) {
        int price_x = right - MeasureText(text, size);
        DrawLine(price_x, y + size / 2, right, y + size / 2, color);
    }
}

void draw_cart(Shop* shop) {
    DrawText("SHOPPING CART", 50, 50, 40, BLACK);
    draw_button(shop, back_button_bounds(shop), "< BACK", SHOP_GREEN);

    int right = shop->render_target.texture.width - CART_LEFT;
    int header_y = 114;
    const char* unit_price = "Unit Price";
    const char* quantity = "Quantity";
    const char* line_total = "Line Total";
    DrawText("Item", CART_LEFT, header_y, 20, SHOP_INK);
    DrawText(unit_price, CART_UNIT_PRICE_RIGHT - MeasureText(unit_price, 20), header_y, 20, SHOP_INK);
    DrawText(quantity, CART_QUANTITY_X - MeasureText(quantity, 20) / 2, header_y, 20, SHOP_INK);
    DrawText(line_total, right - MeasureText(line_total, 20), header_y, 20, SHOP_INK);
    DrawLine(CART_LEFT, 142, right, 142, SHOP_SHADOW);

    if (shop->cart.count == 0) {
        DrawText("Your cart is empty.", CART_LEFT, CART_ROWS_Y + 5, 20, SHOP_TEXT_MUTED);
        return;
    }

    int row_capacity = cart_visible_row_count(shop);
    int first = shop->cart.first_visible_row;
    int end = MIN(first + row_capacity, shop->cart.count);
    int y = CART_ROWS_Y;
    for (int i = first; i < end; i++) {
        Cart_Item cart_item = shop->cart.items[i];
        Item item = cart_item.item;
        float price = item_final_price(item);
        Font font = GetFontDefault();
        float name_width = MeasureTextEx(font, item.display, 20, 2).x;
        float name_scale = name_width > 290 ? 290 / name_width : 1;
        Vector2 name_position = {CART_LEFT, y + (30 - 20 * name_scale) / 2};
        DrawTextEx(font, item.display, name_position, 20 * name_scale, 2 * name_scale, BLACK);
        bool on_sale = item_is_on_sale(item);
        draw_cart_amount(price, CART_UNIT_PRICE_RIGHT, y + 5, 20, on_sale ? SHOP_GREEN : BLACK, false);
        if (on_sale) {
            draw_cart_amount(item.price, CART_UNIT_PRICE_RIGHT, y + 29, 15, DARKGRAY, true);
        }

        Rectangle quantity_box = {CART_QUANTITY_X - 40, y, 80, 30};
        DrawRectangleRec(quantity_box, SHOP_WHITE);
        DrawRectangleLinesEx(quantity_box, 1, SHOP_SHADOW);
        char count[16];
        snprintf(count, sizeof(count), "%d", cart_item.quantity);
        int count_size = MeasureText(count, 20) > 74 ? 14 : 20;
        DrawText(count, CART_QUANTITY_X - MeasureText(count, count_size) / 2,
                 y + (30 - count_size) / 2, count_size, BLACK);
        draw_button(shop, cart_minus_button_bounds(y), "-", SHOP_RED);
        draw_button(shop, cart_plus_button_bounds(y), "+", SHOP_GREEN);
        draw_cart_amount(price * cart_item.quantity, right, y + 5, 20, BLACK, false);
        y += CART_ROW_HEIGHT;
    }

    if (shop->cart.count > row_capacity) {
        DrawText("Scroll for more items", CART_LEFT, y + 12, 16, SHOP_TEXT_MUTED);
    }
    int subtotal_y = y + 12;
    DrawText("Subtotal", CART_QUANTITY_X - MeasureText("Subtotal", 22) / 2, subtotal_y, 22, BLACK);
    draw_cart_amount(cart_subtotal(shop), right, subtotal_y, 22, BLACK, false);
}

void update_cart(Shop* shop) {
    if (button_event(shop, back_button_bounds(shop))) {
        screen_swap(shop, DISPLAY_SCREEN);
        return;
    }

    int row_capacity = cart_visible_row_count(shop);
    float wheel = GetMouseWheelMove();
    int last_first_row = MAX(0, shop->cart.count - row_capacity);
    if (wheel > 0) shop->cart.first_visible_row--;
    if (wheel < 0) shop->cart.first_visible_row++;
    shop->cart.first_visible_row = MAX(0, MIN(shop->cart.first_visible_row, last_first_row));

    int first = shop->cart.first_visible_row;
    int end = MIN(first + row_capacity, shop->cart.count);
    int y = CART_ROWS_Y;
    for (int i = first; i < end; i++, y += CART_ROW_HEIGHT) {
        Cart_Item* cart_item = &shop->cart.items[i];
        if (button_event(shop, cart_plus_button_bounds(y))) {
            if (cart_item->quantity < item_available_stock(shop, cart_item->item.id)) {
                cart_item->quantity++;
            }
            return;
        }

        if (button_event(shop, cart_minus_button_bounds(y))) {
            if (cart_item->quantity > 0) {
                cart_item->quantity--;
            }
            return;
        }
    }
}

void add_to_cart(Shop* shop, Item item) {
    // Check if item already exists in cart
    for (int i = 0; i < shop->cart.count; i++) {
        if (shop->cart.items[i].item.id == item.id) {

            // Don't go above available stock
            if (shop->cart.items[i].quantity < item_available_stock(shop, item.id)) {
                shop->cart.items[i].quantity++;
            }

            return;
        }
    }

    // Don't add out-of-stock items
    if (item_available_stock(shop, item.id) <= 0) {
        return;
    }

    // Grow array if necessary
    if (shop->cart.count >= shop->cart.capacity) {
        shop->cart.capacity =
            shop->cart.capacity == 0
                ? 4
                : shop->cart.capacity * 2;

        shop->cart.items = realloc(
            shop->cart.items,
            shop->cart.capacity * sizeof(Cart_Item)
        );
    }

    // Add new item
    shop->cart.items[shop->cart.count] = (Cart_Item) {
        .item = item,
        .quantity = 1
    };

    shop->cart.count++;
}
