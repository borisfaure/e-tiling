#include "e.h"
#include "config.h"
#include "e_mod_main.h"
#include "e_mod_config.h"
#include "e_border.h"
#include "e_shelf.h"

#include <math.h>
#include <stdbool.h>
#include <assert.h>

typedef enum {
    TILING_RESIZE,
    TILING_MOVE,
} tiling_change_t;

/* actual module specifics */

typedef struct Border_Extra {
     E_Border *border;
     struct {
         int x, y, w, h;
     } expected, orig;
     E_Popup *popup;
     Evas_Object *obj;
} Border_Extra;

struct tiling_g tiling_g = {
   .module = NULL,
   .config = NULL,
   .log_domain = -1,
};

static struct
{
    E_Config_DD         *config_edd,
                        *vdesk_edd;
    E_Border_Hook       *hook;
    int                  currently_switching_desktop;
    Ecore_Event_Handler *handler_hide,
                        *handler_desk_show,
                        *handler_desk_before_show,
                        *handler_mouse_move,
                        *handler_desk_set;
    E_Zone              *current_zone;

    Tiling_Info         *tinfo;
    /* This hash holds the Tiling_Info-pointers for each desktop */
    Eina_Hash           *info_hash;

    Eina_Hash           *border_extras;

    E_Action            *act_togglefloat,
                        *act_addcolumn,
                        *act_removecolumn,
                        *act_swap;

    E_Border            *swap_from;

    unsigned int        has_overlay    :1;
    unsigned int        is_swapping    :1;
} tiling_mod_main_g = {
#define _G tiling_mod_main_g
    .hook = NULL,
    .currently_switching_desktop = 0,
    .handler_hide = NULL,
    .handler_desk_show = NULL,
    .handler_desk_before_show = NULL,
    .handler_mouse_move = NULL,
    .handler_desk_set = NULL,
    .current_zone = NULL,
    .tinfo = NULL,
    .info_hash = NULL,
    .border_extras = NULL,

    .act_togglefloat = NULL,
    .act_addcolumn = NULL,
    .act_removecolumn= NULL,
    .act_swap = NULL,
};

static void
_add_border(E_Border *bd);

/* Utils {{{ */

#define EINA_LIST_IS_IN(_list, _el) \
    (eina_list_data_find(_list, _el) == _el)
#define EINA_LIST_APPEND(_list, _el) \
    _list = eina_list_append(_list, _el)
#define EINA_LIST_REMOVE(_list, _el) \
    _list = eina_list_remove(_list, _el)

/* I wonder why noone has implemented the following one yet? */
static E_Desk *
get_current_desk(void)
{
    E_Manager *m = e_manager_current_get();
    E_Container *c = e_container_current_get(m);
    E_Zone *z = e_zone_current_get(c);

    return e_desk_current_get(z);
}

static Tiling_Info *
_initialize_tinfo(const E_Desk *desk)
{
    Tiling_Info *tinfo;

    tinfo = E_NEW(Tiling_Info, 1);
    tinfo->desk = desk;
    eina_hash_direct_add(_G.info_hash, &tinfo->desk, tinfo);

    tinfo->conf = get_vdesk(tiling_g.config->vdesks, desk->x, desk->y,
                          desk->zone->num);

    return tinfo;
}

static void
check_tinfo(const E_Desk *desk)
{
    if (!_G.tinfo || _G.tinfo->desk != desk) {
        _G.tinfo = eina_hash_find(_G.info_hash, &desk);
        if (!_G.tinfo) {
            /* lazy init */
            _G.tinfo = _initialize_tinfo(desk);
        }
        if (!_G.tinfo->conf) {
            _G.tinfo->conf = get_vdesk(tiling_g.config->vdesks,
                                       desk->x, desk->y,
                                       desk->zone->num);
        }
    }
}

static int
is_floating_window(const E_Border *bd)
{
    check_tinfo(bd->desk);
    return (eina_list_data_find(_G.tinfo->floating_windows, bd) == bd);
}

static int
is_untilable_dialog(const E_Border *bd)
{
    return (!tiling_g.config->tile_dialogs
    && ((bd->client.icccm.transient_for != 0)
         || (bd->client.netwm.type == ECORE_X_WINDOW_TYPE_DIALOG)));
}

static void
change_window_border(E_Border   *bd,
                     const char *bordername)
{
   eina_stringshare_replace(&bd->bordername, bordername);
   bd->client.border.changed = true;
   bd->changed = true;
}

static int
get_column(const E_Border *bd)
{
    for (int i = 0; i < TILING_MAX_COLUMNS; i++) {
        if (EINA_LIST_IS_IN(_G.tinfo->columns[i], bd))
            return i;
    }
    return -1;
}

static int
get_column_count(void)
{
    for (int i = 0; i < TILING_MAX_COLUMNS; i++) {
        if (!_G.tinfo->columns[i])
            return i;
    }
    return TILING_MAX_COLUMNS;
}

static int
get_window_count(void)
{
    int res = 0;

    for (int i = 0; i < TILING_MAX_COLUMNS; i++) {
        if (!_G.tinfo->columns[i])
            break;
        res += eina_list_count(_G.tinfo->columns[i]);
    }
    return res;
}
/* }}} */
/* Reorganize columns {{{*/

static void
_reorganize_column(int col)
{
    int zx, zy, zw, zh, x, w, h, ch, i = 0, count;

    if (col < 0 || col >= TILING_MAX_COLUMNS
        || !_G.tinfo->columns[col])
        return;

    e_zone_useful_geometry_get(_G.tinfo->desk->zone, &zx, &zy, &zw, &zh);

    count = eina_list_count(_G.tinfo->columns[col]);

    x = _G.tinfo->x[col];
    ch = 0;
    w = _G.tinfo->w[col];
    h = zh / count;

    for (Eina_List *l = _G.tinfo->columns[col]; l; l = l->next, i++) {
        E_Border *bd = l->data;
        Border_Extra *extra;
        int d = (i * 2 * zh) % count
              - (2 * ch) % count;

        extra = eina_hash_find(_G.border_extras, &bd);
        if (!extra) {
            ERR("No extra for %p", bd);
            continue;
        }

        if ((bd->maximized & E_MAXIMIZE_VERTICAL) && count != 1) {
            e_border_unmaximize(bd, E_MAXIMIZE_VERTICAL);
        }
        /* let's use a bresenham here */

        extra->expected.x = x;
        extra->expected.y = ch + zy;
        extra->expected.w = w;
        extra->expected.h = h + d;
        ch += extra->expected.h;

        e_border_move_resize(bd, extra->expected.x,
                                 extra->expected.y,
                                 extra->expected.w,
                                 extra->expected.h);
    }
}

static void
_move_resize_column(Eina_List *list, int delta_x, int delta_w)
{
    /* TODO: is alone */
    for (Eina_List *l = list; l; l = l->next) {
        E_Border *bd = l->data;
        Border_Extra *extra;

        extra = eina_hash_find(_G.border_extras, &bd);
        if (!extra) {
            ERR("No extra for %p", bd);
            continue;
        }

        extra->expected.x += delta_x;
        extra->expected.w += delta_w;

        e_border_move_resize(bd, extra->expected.x,
                                 extra->expected.y,
                                 extra->expected.w,
                                 extra->expected.h);
    }
}

static void
_set_column_geometry(int col, int x, int w)
{
    for (Eina_List *l = _G.tinfo->columns[col]; l; l = l->next) {
        E_Border *bd = l->data;
        Border_Extra *extra;

        extra = eina_hash_find(_G.border_extras, &bd);
        if (!extra) {
            ERR("No extra for %p", bd);
            continue;
        }

        extra->expected.x = x;
        extra->expected.w = w;

        if (bd->maximized & E_MAXIMIZE_VERTICAL) {
            e_border_unmaximize(bd, E_MAXIMIZE_HORIZONTAL);
        }

        e_border_move_resize(bd, extra->expected.x,
                                 extra->expected.y,
                                 extra->expected.w,
                                 extra->expected.h);
    }
    _G.tinfo->x[col] = x;
    _G.tinfo->w[col] = w;
}

static void _add_column(void)
{
    if (_G.tinfo->conf->nb_cols == TILING_MAX_COLUMNS)
        return;

    _G.tinfo->conf->nb_cols++;

    if (_G.tinfo->conf->nb_cols == 1) {
        for (Eina_List *l = e_border_focus_stack_get(); l; l = l->next) {
            _add_border(l->data);
        }
    }
    if (_G.tinfo->columns[_G.tinfo->conf->nb_cols - 2]
    &&  _G.tinfo->borders >= _G.tinfo->conf->nb_cols)
    {
        int nb_cols = _G.tinfo->conf->nb_cols - 1;
        int x, y, w, h;
        int width = 0;
        /* Add column */

        e_zone_useful_geometry_get(_G.tinfo->desk->zone, &x, &y, &w, &h);

        for (int i = 0; i < nb_cols; i++) {

            width = w / (nb_cols + 1 - i);

            _set_column_geometry(i, x, width);

            w -= width;
            x += width;
        }
        for (int i = nb_cols - 1; i >= 0; i--) {
            if (eina_list_count(_G.tinfo->columns[i]) == 1) {
                _G.tinfo->columns[i+1] = _G.tinfo->columns[i];
                _reorganize_column(i+1);
            } else {
                E_Border *bd = eina_list_last(_G.tinfo->columns[i])->data;

                EINA_LIST_REMOVE(_G.tinfo->columns[i], bd);
                _reorganize_column(i);

                _G.tinfo->columns[i+1] = NULL;
                EINA_LIST_APPEND(_G.tinfo->columns[i+1], bd);
                _reorganize_column(i+1);
                return;
            }
        }
    }
}

static void _remove_column(void)
{
    if (!_G.tinfo->conf->nb_cols)
        return;

    _G.tinfo->conf->nb_cols--;

    if (!_G.tinfo->conf->nb_cols) {
        for (int i = 0; i < TILING_MAX_COLUMNS; i++) {
            for (Eina_List *l = _G.tinfo->columns[i]; l; l = l->next) {
                E_Border *bd = l->data;
                Border_Extra *extra;

                extra = eina_hash_find(_G.border_extras, &bd);
                if (!extra) {
                    ERR("No extra for %p", bd);
                    continue;
                }
                e_border_move_resize(bd, extra->orig.x,
                                         extra->orig.y,
                                         extra->orig.w,
                                         extra->orig.h);
            }
            eina_list_free(_G.tinfo->columns[i]);
            _G.tinfo->columns[i] = NULL;
        }
        e_place_zone_region_smart_cleanup(_G.tinfo->desk->zone);
    } else {
        int col = _G.tinfo->conf->nb_cols;

        if (_G.tinfo->columns[col]) {
            _G.tinfo->columns[col-1] = eina_list_merge(
                _G.tinfo->columns[col-1], _G.tinfo->columns[col]);
            _reorganize_column(col-1);
        }
    }
}

void change_column_number(struct _Config_vdesk *newconf)
{
    E_Manager *m;
    E_Container *c;
    E_Zone *z;
    E_Desk *d;
    int old_nb_cols;

    m = e_manager_current_get();
    if (!m) return;
    c = e_container_current_get(m);
    if (!c) return;
    z = e_container_zone_number_get(c, newconf->zone_num);
    if (!z) return;
    d = e_desk_at_xy_get(z, newconf->x, newconf->y);
    if (!d) return;

    check_tinfo(d);
    old_nb_cols = _G.tinfo->conf->nb_cols;

    if (newconf->nb_cols == 0) {
        for (int i = 0; i < TILING_MAX_COLUMNS; i++) {
            for (Eina_List *l = _G.tinfo->columns[i]; l; l = l->next) {
                E_Border *bd = l->data;
                Border_Extra *extra;

                extra = eina_hash_find(_G.border_extras, &bd);
                if (!extra) {
                    ERR("No extra for %p", bd);
                    continue;
                }
                e_border_move_resize(bd, extra->orig.x,
                                         extra->orig.y,
                                         extra->orig.w,
                                         extra->orig.h);
            }
            eina_list_free(_G.tinfo->columns[i]);
            _G.tinfo->columns[i] = NULL;
        }
        e_place_zone_region_smart_cleanup(z);
    } else if (newconf->nb_cols > old_nb_cols) {
        for (int i = newconf->nb_cols; i > old_nb_cols; i--) {
            _add_column();
        }
    } else {
        for (int i = newconf->nb_cols; i < old_nb_cols; i++) {
            _remove_column();
        }
    }
}
/* }}} */
/* Reorganize windows {{{*/

static void
_add_border(E_Border *bd)
{
    Border_Extra *extra;
    int col;

    if (!bd) {
        return;
    }
    if (is_floating_window(bd)) {
        return;
    }
    if (is_untilable_dialog(bd)) {
        return;
    }

    if (!_G.tinfo->conf || !_G.tinfo->conf->nb_cols) {
        return;
    }

    extra = E_NEW(Border_Extra, 1);
    *extra = (Border_Extra) {
        .border = bd,
        .expected = {
            .x = bd->x,
            .y = bd->y,
            .w = bd->w,
            .h = bd->h,
        },
        .orig = {
            .x = bd->x,
            .y = bd->y,
            .w = bd->w,
            .h = bd->h,
        },
    };

    eina_hash_direct_add(_G.border_extras, &extra->border, extra);

    /* New Border! */

    if ((bd->bordername && strcmp(bd->bordername, "pixel"))
    ||  !bd->bordername)
    {
        change_window_border(bd, "pixel");
    }

    if (_G.tinfo->columns[0]) {
        if (_G.tinfo->columns[_G.tinfo->conf->nb_cols - 1]) {
            col = _G.tinfo->conf->nb_cols - 1;

            if (!_G.tinfo->columns[col]->next) {
                e_border_unmaximize(_G.tinfo->columns[col]->data,
                                    E_MAXIMIZE_BOTH);
            }
            EINA_LIST_APPEND(_G.tinfo->columns[col], bd);
            _reorganize_column(col);
        } else {
            /* Add column */
            int nb_cols = get_column_count();
            int x, y, w, h;
            int width = 0;

            e_zone_useful_geometry_get(bd->zone, &x, &y, &w, &h);

            for (int i = 0; i < nb_cols; i++) {

                width = w / (nb_cols + 1 - i);

                _set_column_geometry(i, x, width);

                w -= width;
                x += width;
            }

            _G.tinfo->x[nb_cols] = x;
            _G.tinfo->w[nb_cols] = width;
            extra->expected.x = x;
            extra->expected.y = y;
            extra->expected.w = width;
            extra->expected.h = h;
            e_border_move_resize(bd,
                                 extra->expected.x,
                                 extra->expected.y,
                                 extra->expected.w,
                                 extra->expected.h);
            e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_VERTICAL);

            EINA_LIST_APPEND(_G.tinfo->columns[nb_cols], bd);
            col = nb_cols;
        }
    } else {
        e_border_unmaximize(bd, E_MAXIMIZE_BOTH);
        e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);
        EINA_LIST_APPEND(_G.tinfo->columns[0], bd);
        e_zone_useful_geometry_get(bd->zone,
                                   &_G.tinfo->x[0], NULL,
                                   &_G.tinfo->w[0], NULL);
        col = 0;
    }
    _G.tinfo->borders++;

}

static void
_remove_border(E_Border *bd)
{
    int col;
    int nb_cols;

    check_tinfo(bd->desk);

    nb_cols = get_column_count();

    col = get_column(bd);
    if (col < 0)
        return;

    _G.tinfo->borders--;
    EINA_LIST_REMOVE(_G.tinfo->columns[col], bd);
    eina_hash_del(_G.border_extras, bd, NULL);

    if (_G.tinfo->columns[col]) {
        _reorganize_column(col);
    } else {
        if (nb_cols > _G.tinfo->borders) {
            int x, y, w, h;
            int width = 0;
            /* Remove column */

            nb_cols--;

            e_zone_useful_geometry_get(bd->zone, &x, &y, &w, &h);

            for (int i = col; i < nb_cols; i++) {
                _G.tinfo->columns[i] = _G.tinfo->columns[i+1];
            }
            _G.tinfo->columns[nb_cols] = NULL;
            for (int i = 0; i < nb_cols; i++) {

                width = w / (nb_cols - i);

                _set_column_geometry(i, x, width);

                w -= width;
                x += width;
            }
        } else {
            for (int i = col+1; i < nb_cols; i++) {
                if (eina_list_count(_G.tinfo->columns[i]) > 1) {
                    for (int j = col; j < i - 1; j++) {
                        _G.tinfo->columns[j] = _G.tinfo->columns[j+1];
                        _reorganize_column(j);
                    }
                    bd = _G.tinfo->columns[i]->data;
                    EINA_LIST_REMOVE(_G.tinfo->columns[i], bd);
                    _reorganize_column(i);

                    _G.tinfo->columns[i-1] = NULL;
                    EINA_LIST_APPEND(_G.tinfo->columns[i-1], bd);
                    _reorganize_column(i-1);
                    return;
                }
            }
            for (int i = col-1; i >= 0; i--) {
                if (eina_list_count(_G.tinfo->columns[i]) == 1) {
                    _G.tinfo->columns[i+1] = _G.tinfo->columns[i];
                    _reorganize_column(i+1);
                } else {
                    bd = eina_list_last(_G.tinfo->columns[i])->data;
                    EINA_LIST_REMOVE(_G.tinfo->columns[i], bd);
                    _reorganize_column(i);

                    _G.tinfo->columns[i+1] = NULL;
                    EINA_LIST_APPEND(_G.tinfo->columns[i+1], bd);
                    _reorganize_column(i+1);
                    return;
                }
            }
        }
    }
}

static void
_move_resize_border_column(E_Border *bd, Border_Extra *extra,
                           int col, tiling_change_t change)
{
    if (change == TILING_RESIZE) {
        if (col == TILING_MAX_COLUMNS || !_G.tinfo->columns[col + 1]) {
            /* You're not allowed to resize */
            bd->w = extra->expected.w;
        } else {
            int delta = bd->w - extra->expected.w;

            if (delta + 1 > _G.tinfo->w[col + 1])
                delta = _G.tinfo->w[col + 1] - 1;

            _move_resize_column(_G.tinfo->columns[col], 0, delta);
            _move_resize_column(_G.tinfo->columns[col+1], delta, -delta);
            extra->expected.w = bd->w;
        }
    } else {
        if (col == 0) {
            /* You're not allowed to move */
            bd->x = extra->expected.x;
        } else {
            int delta = bd->x - extra->expected.x;

            if (delta + 1 > _G.tinfo->w[col - 1])
                delta = _G.tinfo->w[col - 1] - 1;

            _move_resize_column(_G.tinfo->columns[col], delta, -delta);
            _move_resize_column(_G.tinfo->columns[col-1], 0, delta);
            extra->expected.x = bd->x;
        }
    }
}

static void
_move_resize_border_in_column(E_Border *bd, Border_Extra *extra,
                              int col, tiling_change_t change)
{
    Eina_List *l;

    l = eina_list_data_find_list(_G.tinfo->columns[col], bd);
    if (!l)
        return;

    if (change == TILING_RESIZE) {
        if (!l->next) {
            /* You're not allowed to resize */
            bd->h = extra->expected.h;
        } else {
            int delta = bd->h - extra->expected.h;
            E_Border *nextbd = l->next->data;
            Border_Extra *nextextra;
            int min_height = MAX(nextbd->client.icccm.base_h, 1);

            nextextra = eina_hash_find(_G.border_extras, &nextbd);
            if (!nextextra) {
                ERR("No extra for %p", nextbd);
                return;
            }

            if (nextextra->expected.h - delta < min_height)
                delta = nextextra->expected.h - min_height;

            nextextra->expected.y += delta;
            nextextra->expected.h -= delta;
            e_border_move_resize(nextbd,
                                 nextextra->expected.x,
                                 nextextra->expected.y,
                                 nextextra->expected.w,
                                 nextextra->expected.h);

            extra->expected.h += delta;
            bd->h = extra->expected.h;
        }
    } else {
        if (!l->prev) {
            /* You're not allowed to move */
            bd->y = extra->expected.y;
        } else {
            int delta = bd->y - extra->expected.y;
            E_Border *prevbd = l->prev->data;
            Border_Extra *prevextra;
            int min_height = MAX(prevbd->client.icccm.base_h, 1);

            prevextra = eina_hash_find(_G.border_extras, &prevbd);
            if (!prevextra) {
                ERR("No extra for %p", prevbd);
                return;
            }

            if (prevextra->expected.h - delta < min_height)
                delta = prevextra->expected.h - min_height;

            prevextra->expected.h += delta;
            e_border_move_resize(prevbd,
                                 prevextra->expected.x,
                                 prevextra->expected.y,
                                 prevextra->expected.w,
                                 prevextra->expected.h);

            extra->expected.y += delta;
            extra->expected.h -= delta;
            bd->y = extra->expected.y;
            bd->h = extra->expected.h;
        }
    }
}

/* }}} */
/* Toggle Floating {{{ */

static void
toggle_floating(E_Border *bd)
{
    if (!bd || !_G.tinfo)
        return;

    check_tinfo(bd->desk);

    if (eina_list_data_find(_G.tinfo->floating_windows, bd) == bd) {
        _G.tinfo->floating_windows =
            eina_list_remove(_G.tinfo->floating_windows, bd);

        _add_border(bd);
    } else {
        /* To give the user a bit of feedback we restore the original border */
        /* TODO: save the original border, don't just restore the default one*/
        _G.tinfo->floating_windows =
            eina_list_prepend(_G.tinfo->floating_windows, bd);

        _remove_border(bd);

        e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);

        change_window_border(bd, "default");
    }
}

/* }}} */
/* Overlays {{{*/

static Eina_Bool
destroy_overlay(const Eina_Hash *hash,
                const void      *key,
                void            *data,
                void            *fdata)
{
    Border_Extra *extra = data;

    if (extra->obj) {
        evas_object_del(extra->obj);
        extra->obj = NULL;
    }
    if (extra->popup) {
        e_object_del(E_OBJECT(extra->popup));
        extra->popup = NULL;
    }

    return EINA_TRUE;
}


static void
destroy_overlays(void)
{
    if (!_G.has_overlay)
        return;

    eina_hash_foreach(_G.border_extras, destroy_overlay, NULL);
    _G.has_overlay = false;
}

/* }}} */
/* Action callbacks {{{*/

static void
_e_mod_action_toggle_floating_cb(E_Object   *obj,
                                 const char *params)
{
    destroy_overlays();

    toggle_floating(e_border_focused_get());
}

static void
_e_mod_action_add_column_cb(E_Object   *obj,
                            const char *params)
{
    E_Desk *desk = get_current_desk();

    destroy_overlays();

    check_tinfo(desk);

    _add_column();
}

static void
_e_mod_action_remove_column_cb(E_Object   *obj,
                               const char *params)
{
    E_Desk *desk = get_current_desk();

    destroy_overlays();

    check_tinfo(desk);

    _remove_column();
}

static void _e_mod_action_swap_cb(E_Object   *obj,
                                  const char *params)
{
    E_Desk *desk;
    E_Border *focused_bd;
    int nb_win;
    char keys[] = "asdfghkl;'qwertyuiop[]\\zxcvbnm,./`1234567890-=";
    char *c = keys;

    destroy_overlays();

    desk = get_current_desk();
    if (!desk)
        return;

    focused_bd = e_border_focused_get();
    if (!focused_bd || focused_bd->desk != desk)
        return;

    check_tinfo(desk);

    if (!_G.tinfo->conf || !_G.tinfo->conf->nb_cols) {
        return;
    }
    DBG("swap");

    nb_win = get_window_count();
    if (nb_win < 2) {
        return;
    }
    _G.has_overlay = true;

    for (int i = 0; i < TILING_MAX_COLUMNS; i++) {
        Eina_List *l;
        E_Border *bd;

        if (!_G.tinfo->columns[i])
            break;
        EINA_LIST_FOREACH(_G.tinfo->columns[i], l, bd) {
            if (bd != focused_bd && *c) {
                Border_Extra *extra;
                Evas_Coord ew, eh;
                char buf[40];

                extra = eina_hash_find(_G.border_extras, &bd);
                if (!extra) {
                    ERR("No extra for %p", bd);
                    continue;
                }

                extra->popup = e_popup_new(bd->zone, 0, 0, 1, 1);
                if (!extra->popup)
                    continue;

                e_popup_layer_set(extra->popup, 255);
                extra->obj = edje_object_add(extra->popup->evas);
                e_theme_edje_object_set(extra->obj, "base/theme/borders",
                                        "e/widgets/border/default/resize");

                snprintf(buf, sizeof(buf), "%c", *c);
                c++;
                edje_object_part_text_set(extra->obj, "e.text.label", buf);
                edje_object_size_min_calc(extra->obj, &ew, &eh);
                evas_object_move(extra->obj, 0, 0);
                evas_object_resize(extra->obj, ew, eh);
                evas_object_show(extra->obj);
                e_popup_edje_bg_object_set(extra->popup, extra->obj);

                evas_object_show(extra->obj);
                e_popup_show(extra->popup);

                e_popup_move_resize(extra->popup,
                                    (bd->x - extra->popup->zone->x) +
                                    ((bd->w - ew) / 2),
                                    (bd->y - extra->popup->zone->y) +
                                    ((bd->h - eh) / 2),
                                    ew, eh);

                e_popup_show(extra->popup);
            }
        }
    }
    /* TODO:
     * add overlay,
     * add overlay timeout
     * …*/
}
/* }}} */
/* Hooks {{{*/

static void
_e_module_tiling_cb_hook(void *data,
                         void *border)
{
    E_Border *bd = border;
    int col = -1;

    destroy_overlays();

    if (!bd) {
        return;
    }
    if (is_floating_window(bd)) {
        return;
    }
    if (is_untilable_dialog(bd)) {
        return;
    }

    if (!_G.tinfo->conf || !_G.tinfo->conf->nb_cols) {
        return;
    }

    col = get_column(bd);

    DBG("cb-Hook for %p / %s / %s, changes(size=%d, position=%d, border=%d)"
        " g:%dx%d+%d+%d bdname:%s (%d) %d",
        bd, bd->client.icccm.title, bd->client.netwm.name,
        bd->changes.size, bd->changes.pos, bd->changes.border,
        bd->w, bd->h, bd->x, bd->y, bd->bordername,
        col, bd->maximized);

    if (!bd->changes.size && !bd->changes.pos && !bd->changes.border
    && col >= 0) {
        return;
    }

    if (col < 0) {
        _add_border(bd);
    } else {
        Border_Extra *extra;

        /* Move or Resize */

        extra = eina_hash_find(_G.border_extras, &bd);
        if (!extra) {
            ERR("No extra for %p", bd);
            return;
        }

        if (col == 0 && !_G.tinfo->columns[1] && !_G.tinfo->columns[0]->next) {
            if (bd->maximized) {
                extra->expected.x = bd->x;
                extra->expected.y = bd->y;
                extra->expected.w = bd->w;
                extra->expected.h = bd->h;
            } else {
                /* TODO: what if a window doesn't want to be maximized? */
                e_border_unmaximize(bd, E_MAXIMIZE_BOTH);
                e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);
            }
        }
        if (bd->x == extra->expected.x && bd->y == extra->expected.y
        &&  bd->w == extra->expected.w && bd->h == extra->expected.h)
        {
            return;
        }

        if (bd->changes.border && bd->changes.size) {
            e_border_move_resize(bd, extra->expected.x, extra->expected.y,
                                     extra->expected.w, extra->expected.h);
            return;
        }

        DBG("old:%dx%d+%d+%d vs new:%dx%d+%d+%d. step:%dx%d. base:%dx%d",
            extra->expected.w, extra->expected.h,
            extra->expected.x, extra->expected.y,
            bd->w, bd->h, bd->x, bd->y,
            bd->client.icccm.step_w, bd->client.icccm.step_h,
            bd->client.icccm.base_w, bd->client.icccm.base_h);

        if (abs(extra->expected.w - bd->w) >= bd->client.icccm.step_w) {
            _move_resize_border_column(bd, extra, col, TILING_RESIZE);
        }
        if (abs(extra->expected.h - bd->h) >= bd->client.icccm.step_h) {
            _move_resize_border_in_column(bd, extra, col, TILING_RESIZE);
        }
        if (extra->expected.x != bd->x) {
            _move_resize_border_column(bd, extra, col, TILING_MOVE);
        }
        if (extra->expected.y != bd->y) {
            _move_resize_border_in_column(bd, extra, col, TILING_MOVE);
        }
    }
}

static Eina_Bool
_e_module_tiling_hide_hook(void *data,
                           int   type,
                           void *event)
{
    E_Event_Border_Hide *ev = event;
    E_Border *bd = ev->border;

    destroy_overlays();

    if (_G.currently_switching_desktop)
        return EINA_TRUE;

    check_tinfo(bd->desk);

    if (EINA_LIST_IS_IN(_G.tinfo->floating_windows, bd)) {
        EINA_LIST_REMOVE(_G.tinfo->floating_windows, bd);
    }

    _remove_border(bd);

    return EINA_TRUE;
}

static Eina_Bool
_e_module_tiling_desk_show(void *data,
                           int   type,
                           void *event)
{
    _G.currently_switching_desktop = 0;

    destroy_overlays();

    return EINA_TRUE;
}

static Eina_Bool
_e_module_tiling_desk_before_show(void *data,
                                  int   type,
                                  void *event)
{
    destroy_overlays();

    _G.currently_switching_desktop = 1;

    return EINA_TRUE;
}

static Eina_Bool
_clear_bd_from_info_hash(const Eina_Hash *hash,
                         const void      *key,
                         void            *data,
                         void            *fdata)
{
    Tiling_Info *ti = data;
    E_Event_Border_Desk_Set *ev = fdata;

    if (!ev || !ti)
        return EINA_TRUE;

    if (ti->desk == ev->desk) {
        return EINA_TRUE;
    }

    if (EINA_LIST_IS_IN(ti->floating_windows, ev->border)) {
        ti->floating_windows = eina_list_remove(ti->floating_windows,
                                                ev->border);
    }

    return EINA_TRUE;
}

static Eina_Bool
_e_module_tiling_desk_set(void *data,
                          int   type,
                          void *event)
{
    /* TODO: remove this stuff?? */
    /* We use this event to ensure that border desk changes are done correctly
     * because a user can move the window to another desk (and events are
     * fired) involving zone changes or not (depends on the mouse position) */
    E_Event_Border_Desk_Set *ev = event;
    Tiling_Info *tinfo;

    destroy_overlays();

    tinfo = eina_hash_find(_G.info_hash, &ev->desk);

    if (!tinfo) {
        tinfo = _initialize_tinfo(ev->desk);
    }

    eina_hash_foreach(_G.info_hash, _clear_bd_from_info_hash, ev);

    return EINA_TRUE;
}

/* }}} */
/* Module setup {{{*/

static void
_clear_info_hash(void *data)
{
    Tiling_Info *ti = data;

    eina_list_free(ti->floating_windows);
    for (int i = 0; i < TILING_MAX_COLUMNS; i++) {
        eina_list_free(ti->columns[i]);
        ti->columns[i] = NULL;
    }
    E_FREE(ti);
}

static void
_clear_border_extras(void *data)
{
    Border_Extra *be = data;

    E_FREE(be);
}

EAPI E_Module_Api e_modapi =
{
    E_MODULE_API_VERSION,
    "E-Tiling"
};

EAPI void *
e_modapi_init(E_Module *m)
{
    char buf[PATH_MAX];
    E_Desk *desk;

    tiling_g.module = m;

    if (tiling_g.log_domain < 0) {
        tiling_g.log_domain = eina_log_domain_register("e-tiling", NULL);
        if (tiling_g.log_domain < 0) {
            EINA_LOG_CRIT("could not register log domain 'e-tiling'");
        }
    }


    snprintf(buf, sizeof(buf), "%s/locale", e_module_dir_get(m));
    bindtextdomain(PACKAGE, buf);
    bind_textdomain_codeset(PACKAGE, "UTF-8");

    _G.info_hash = eina_hash_pointer_new(_clear_info_hash);

    _G.border_extras = eina_hash_pointer_new(_clear_border_extras);

    /* Callback for new windows or changes */
    _G.hook = e_border_hook_add(E_BORDER_HOOK_EVAL_POST_BORDER_ASSIGN,
                                _e_module_tiling_cb_hook, NULL);
    /* Callback for hiding windows */
    _G.handler_hide = ecore_event_handler_add(E_EVENT_BORDER_HIDE,
                                             _e_module_tiling_hide_hook, NULL);
    /* Callback when virtual desktop changes */
    _G.handler_desk_show = ecore_event_handler_add(E_EVENT_DESK_SHOW,
                                             _e_module_tiling_desk_show, NULL);
    /* Callback before virtual desktop changes */
    _G.handler_desk_before_show =
        ecore_event_handler_add(E_EVENT_DESK_BEFORE_SHOW,
                                _e_module_tiling_desk_before_show, NULL);
    /* Callback when the mouse moves */
    /*
    _G.handler_mouse_move = ecore_event_handler_add(ECORE_EVENT_MOUSE_MOVE,
                                            _e_module_tiling_mouse_move, NULL);
    */
    /* Callback when a border is set to another desk */
    _G.handler_desk_set = ecore_event_handler_add(E_EVENT_BORDER_DESK_SET,
                                              _e_module_tiling_desk_set, NULL);

#define ACTION_ADD(_act, _cb, _title, _value)                                \
{                                                                            \
   E_Action *_action = _act;                                                 \
   const char *_name = _value;                                               \
   if ((_action = e_action_add(_name)))                                      \
     {                                                                       \
        _action->func.go = _cb;                                              \
        e_action_predef_name_set(D_("E-Tiling"), D_(_title), _name,          \
                                 NULL, NULL, 0);                             \
     }                                                                       \
}

    /* Module's actions */
    ACTION_ADD(_G.act_togglefloat, _e_mod_action_toggle_floating_cb,
               "Toggle floating", "toggle_floating");
    ACTION_ADD(_G.act_addcolumn,   _e_mod_action_add_column_cb,
               "Add a Column", "add_column");
    ACTION_ADD(_G.act_removecolumn,   _e_mod_action_remove_column_cb,
               "Remove a Column", "remove_column");
    ACTION_ADD(_G.act_swap,   _e_mod_action_swap_cb,
               "Swap a window with an other", "swap");
#undef ACTION_ADD

    /* Configuration entries */
    snprintf(buf, sizeof(buf), "%s/e-module-e-tiling.edj",
             e_module_dir_get(m));
    e_configure_registry_category_add("windows", 50, D_("Windows"), NULL,
                                      "preferences-system-windows");
    e_configure_registry_item_add("windows/e-tiling", 150, D_("E-Tiling"),
                                  NULL, buf, e_int_config_tiling_module);

    /* Configuration itself */
    _G.config_edd = E_CONFIG_DD_NEW("Tiling_Config", Config);
    _G.vdesk_edd = E_CONFIG_DD_NEW("Tiling_Config_VDesk",
                                   struct _Config_vdesk);
    E_CONFIG_VAL(_G.config_edd, Config, tile_dialogs, INT);
    E_CONFIG_VAL(_G.config_edd, Config, float_too_big_windows, INT);

    E_CONFIG_LIST(_G.config_edd, Config, vdesks, _G.vdesk_edd);
    E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, x, INT);
    E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, y, INT);
    E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, zone_num, INT);
    E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, nb_cols, INT);

    tiling_g.config = e_config_domain_load("module.e-tiling", _G.config_edd);
    if (!tiling_g.config) {
        tiling_g.config = E_NEW(Config, 1);
        tiling_g.config->float_too_big_windows = 1;
        tiling_g.config->tile_dialogs = 1;
    }

    E_CONFIG_LIMIT(tiling_g.config->tile_dialogs, 0, 1);
    E_CONFIG_LIMIT(tiling_g.config->float_too_big_windows, 0, 1);

    desk = get_current_desk();
    _G.current_zone = desk->zone;
    _G.tinfo = _initialize_tinfo(desk);

    return m;
}

EAPI int
e_modapi_shutdown(E_Module *m)
{

    if (tiling_g.log_domain >= 0) {
        eina_log_domain_unregister(tiling_g.log_domain);
        tiling_g.log_domain = -1;
    }

    if (_G.hook) {
        e_border_hook_del(_G.hook);
        _G.hook = NULL;
    }

#define FREE_HANDLER(x)          \
if (x) {                         \
     ecore_event_handler_del(x); \
     x = NULL;                   \
}
    FREE_HANDLER(_G.handler_hide);
    FREE_HANDLER(_G.handler_desk_show);
    FREE_HANDLER(_G.handler_desk_before_show);
    FREE_HANDLER(_G.handler_mouse_move);
    FREE_HANDLER(_G.handler_desk_set);
#undef FREE_HANDLER


#define ACTION_DEL(act, title, value)                   \
if (act) {                                              \
     e_action_predef_name_del(D_("E-Tiling"), D_(title)); \
     e_action_del(value);                               \
     act = NULL;                                        \
}
    ACTION_DEL(_G.act_togglefloat, "Toggle floating", "toggle_floating");
    ACTION_DEL(_G.act_addcolumn, "Add a Column", "add_column");
    ACTION_DEL(_G.act_removecolumn, "Remove a Column", "remove_column");
    ACTION_DEL(_G.act_swap, "Swap a window with an other", "swap");
#undef ACTION_DEL

    e_configure_registry_item_del("windows/e-tiling");
    e_configure_registry_category_del("windows");

    destroy_overlays();

    E_FREE(tiling_g.config);
    E_CONFIG_DD_FREE(_G.config_edd);
    E_CONFIG_DD_FREE(_G.vdesk_edd);

    tiling_g.module = NULL;

    eina_hash_free(_G.info_hash);
    _G.info_hash = NULL;

    eina_hash_free(_G.border_extras);
    _G.border_extras = NULL;

    _G.tinfo = NULL;

    return 1;
}

EAPI int
e_modapi_save(E_Module *m)
{
    e_config_domain_save("module.e-tiling", _G.config_edd, tiling_g.config);
    /* TODO */
    DBG("SAVE");

    return EINA_TRUE;
}
/* }}} */
