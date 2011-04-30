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
     int x,
         y,
         w,
         h;
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

   E_Action            *act_toggletiling,
                       *act_togglefloat,
                       *act_switchtiling;
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

   .act_toggletiling = NULL,
   .act_togglefloat = NULL,
   .act_switchtiling = NULL,
};

static void
_e_mod_action_toggle_floating_cb(E_Object   *obj,
                                 const char *params);
static void toggle_floating(E_Border *bd);
static void _desk_show(const E_Desk *desk);

#define TILE_LOOP_DESKCHECK                               \
  if ((lbd->desk != bd->desk) || (lbd->zone != bd->zone)) \
    continue;

#define TILE_LOOP_CHECKS(lbd)                                              \
((_G.tinfo && eina_list_data_find(_G.tinfo->floating_windows, lbd) == lbd) \
 || (lbd->visible == 0)                                                    \
 || (!tiling_g.config->tile_dialogs                                        \
     && ((lbd->client.icccm.transient_for != 0)                            \
         || (lbd->client.netwm.type == ECORE_X_WINDOW_TYPE_DIALOG))))      \

/* Utils {{{ */

/* I wonder why noone has implemented the following one yet? */
static E_Desk *
get_current_desk(void)
{
    E_Manager *m = e_manager_current_get();
    E_Container *c = e_container_current_get(m);
    E_Zone *z = e_zone_current_get(c);
    return e_desk_current_get(z);
}

/* Generates a unique identifier for the given desk to be used in info_hash */
static char *
desk_hash_key(const E_Desk *desk)
{
    /* TODO: can't we use the pointer as a hash? */
    /* I think 64 chars should be enough for all localizations of "desk" */
    static char buffer[64];

    if (!desk)
        return NULL;
    snprintf(buffer, sizeof(buffer), "%d%s", desk->zone->num, desk->name);
    return buffer;
}

static Tiling_Info *
_initialize_tinfo(const E_Desk *desk)
{
    Eina_List *l;
    Tiling_Info *res;
    E_Border *lbd;

    res = E_NEW(Tiling_Info, 1);
    res->desk = desk;
    res->slaves_count = 0;
    res->big_perc = 0.5;
    res->need_rearrange = 0;
    res->nb_cols = 2;
    eina_hash_add(_G.info_hash, desk_hash_key(desk), res);

    /* TODO: should we do that?? */
    EINA_LIST_FOREACH(e_border_client_list(), l, lbd) {
        if (lbd->desk == desk) {
            if (res->master_list)
                res->slave_list = eina_list_append(res->slave_list, lbd);
            else
                res->master_list = eina_list_append(res->master_list, lbd);
        }
    }

    return res;
}

static void
check_tinfo(const E_Desk *desk)
{
    if (!_G.tinfo || _G.tinfo->desk != desk) {
        _G.tinfo = eina_hash_find(_G.info_hash, desk_hash_key(desk));
        if (!_G.tinfo) {
            /* We need to add a new Tiling_Info, so we weren't on that desk before.
             * As e doesn't call the POST_EVAL-hook (or e_desk_show which then
             * indirectly calls the POST_EVAL) for each window on that desk but only
             * for the focused, we need to get all borders on that desk. */
            DBG("need new info for %s\n", desk->name);
            _G.tinfo = _initialize_tinfo(desk);
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
is_untilable_dialog(E_Border *bd)
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
   bd->client.border.changed = 1;
   bd->changed = 1;
}

/* }}} */
/* Reorganize windows {{{*/
static void
_reorganize_slaves(void)
{
    int zx, zy, zw, zh, x, w, h, ch, i = 0;

    if (!_G.tinfo->slaves_count)
        return;

    e_zone_useful_geometry_get(_G.tinfo->desk->zone, &zx, &zy, &zw, &zh);
    DBG("useful geometry: %dx%d+%d+%d", zw, zh, zx, zy);

    x = _G.tinfo->big_perc * zw + zx;
    ch = 0;
    w = zw - x + zx;
    h = zh / _G.tinfo->slaves_count;

    DBG("zw = %d, count = %d, h = %d",
        zw, _G.tinfo->slaves_count, h);
    for (Eina_List *l = _G.tinfo->slave_list; l; l = l->next, i++) {
        E_Border *bd = l->data;
        Border_Extra *extra;
        int d = (i * 2 * zh) % _G.tinfo->slaves_count
              - (2 * ch) % _G.tinfo->slaves_count;

        extra = eina_hash_find(_G.border_extras, &bd);
        if (!extra) {
            ERR("No extra for %p", bd);
            continue;
        }

        /* let's use a bresenham here */

        extra->x = x;
        extra->y = ch + zy;
        extra->w = w;
        extra->h = h + d;
        ch += extra->h;
        DBG("%p: d = %d, ch = %d, (%d, %d, %d, %d)", bd, d, ch,
            extra->x, extra->y, extra->w, extra->h);

        e_border_move_resize(bd, extra->x,
                                 extra->y,
                                 extra->w,
                                 extra->h);
    }
}

static void
_add_border(E_Border *bd)
{
    Border_Extra *extra;

    if (!bd || !bd->visible) {
        DBG("bd=%p", bd);
        return;
    }
    if (is_floating_window(bd)) {
        DBG("floating window");
        return;
    }
    if (is_untilable_dialog(bd)) {
        DBG("untilable_dialog");
        return;
    }

    if (!_G.tinfo->nb_cols) {
        DBG("no tiling");
        return;
    }

    extra = E_NEW(Border_Extra, 1);
    *extra = (Border_Extra) {
        .border = bd,
            .x = bd->x,
            .y = bd->y,
            .w = bd->w,
            .h = bd->h
    };

    eina_hash_direct_add(_G.border_extras, &extra->border, extra);

    /* New Border! */
    DBG("new border");

    if ((bd->bordername && strcmp(bd->bordername, "pixel"))
    ||  !bd->bordername)
    {
        change_window_border(bd, "pixel");
    }

    if (_G.tinfo->master_list) {
        DBG("put in slaves");
        if (_G.tinfo->slave_list) {
            if (!_G.tinfo->slave_list->next) {
                e_border_unmaximize(_G.tinfo->slave_list->data,
                                    E_MAXIMIZE_BOTH);
            }
            _G.tinfo->slave_list = eina_list_append(_G.tinfo->slave_list, bd);
            _G.tinfo->slaves_count++;
            _reorganize_slaves();
        } else {
            /* Resize Master */
            E_Border *master = _G.tinfo->master_list->data;
            Border_Extra *master_extra = eina_hash_find(_G.border_extras,
                                                        &master);
            int new_master_width = master->w * _G.tinfo->big_perc;

            assert(master_extra);

            extra->x = master->x + new_master_width;
            extra->y = master->y;
            extra->w = master->w - new_master_width;
            extra->h = master->h;
            e_border_move_resize(bd,
                                 extra->x,
                                 extra->y,
                                 extra->w,
                                 extra->h);
            e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_VERTICAL);

            master_extra->w = new_master_width;
            e_border_unmaximize(master, E_MAXIMIZE_HORIZONTAL);
            e_border_move_resize(master, master_extra->x,
                                 master_extra->y,
                                 new_master_width,
                                 master->h);
            _G.tinfo->slave_list = eina_list_append(_G.tinfo->slave_list, bd);
            _G.tinfo->slaves_count++;
        }
    } else {
        e_border_unmaximize(bd, E_MAXIMIZE_BOTH);
        e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);
        _G.tinfo->master_list = eina_list_append(_G.tinfo->master_list, bd);
    }
}

static void
_remove_border(E_Border *bd)
{
    bool is_master = false,
         is_slave = false;

    check_tinfo(bd->desk);

    is_master = eina_list_data_find(_G.tinfo->master_list, bd) == bd;
    is_slave = eina_list_data_find(_G.tinfo->slave_list, bd) == bd;

    if (!is_master && !is_slave)
        return;

    if (is_master) {
        _G.tinfo->master_list = eina_list_remove(_G.tinfo->master_list, bd);
        if (_G.tinfo->slave_list) {
            if (_G.tinfo->slave_list->next) {
                E_Border *new_master = _G.tinfo->slave_list->data;
                Border_Extra *new_master_extra = eina_hash_find(_G.border_extras,
                                                                &new_master);
                _G.tinfo->slave_list = eina_list_remove(_G.tinfo->slave_list,
                                                        new_master);
                _G.tinfo->slaves_count--;

                new_master_extra->x = bd->x;
                new_master_extra->y = bd->y;
                new_master_extra->w = bd->w;
                new_master_extra->h = bd->h;
                e_border_move_resize(new_master, bd->x, bd->y,
                                                 bd->w, bd->h);
                e_border_maximize(new_master, E_MAXIMIZE_EXPAND | E_MAXIMIZE_VERTICAL);


                _G.tinfo->master_list = eina_list_append(_G.tinfo->master_list,
                                                         new_master);
                _reorganize_slaves();
            } else {
                E_Border *new_master = _G.tinfo->slave_list->data;
                Border_Extra *new_master_extra = eina_hash_find(_G.border_extras,
                                                                &new_master);
                _G.tinfo->slave_list = eina_list_remove(_G.tinfo->slave_list,
                                                        new_master);
                _G.tinfo->slaves_count--;

                e_border_unmaximize(new_master, E_MAXIMIZE_BOTH);
                e_border_maximize(new_master, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);
                new_master_extra->x = bd->x;
                new_master_extra->y = bd->y;
                new_master_extra->w = bd->w;
                new_master_extra->h = bd->h;

                _G.tinfo->master_list = eina_list_append(_G.tinfo->master_list,
                                                         new_master);
            }
        }
    } else {
        _G.tinfo->slaves_count--;
        if (!_G.tinfo->slave_list->next) {
            E_Border *master = _G.tinfo->master_list->data;

            e_border_unmaximize(master, E_MAXIMIZE_BOTH);
            e_border_maximize(master, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);
            _G.tinfo->slave_list = eina_list_remove(_G.tinfo->slave_list, bd);
        } else {
            _G.tinfo->slave_list = eina_list_remove(_G.tinfo->slave_list, bd);
            _reorganize_slaves();
        }
    }
    eina_hash_del(_G.border_extras, bd, NULL);
}

static void
_move_resize_column(Eina_List *list, int delta_x, int delta_w)
{
    for (Eina_List *l = list; l; l = l->next) {
        E_Border *bd = l->data;
        Border_Extra *extra;

        extra = eina_hash_find(_G.border_extras, &bd);
        if (!extra) {
            ERR("No extra for %p", bd);
            continue;
        }

        extra->x += delta_x;
        extra->w += delta_w;

        e_border_move_resize(bd, extra->x,
                                 extra->y,
                                 extra->w,
                                 extra->h);
    }
}

static void
_move_resize_border_in_column(E_Border *bd, Border_Extra *extra,
                              bool is_master, tiling_change_t change)
{
    if (change == TILING_RESIZE) {
        if (is_master) {
            int delta = bd->w - extra->w;

            _move_resize_column(_G.tinfo->slave_list, delta, -delta);
        } else {
            /* You're not allowed to resize */
            bd->w = extra->w;
        }
    } else {
        if (is_master) {
            /* You're not allowed to move */
            bd->x = extra->x;
        } else {
            int delta = bd->x - extra->x;

            _move_resize_column(_G.tinfo->slave_list, delta, -delta);
            _move_resize_column(_G.tinfo->master_list, 0, delta);
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
        /* TODO: save the original border, don't just restore the default one */
        _G.tinfo->floating_windows =
            eina_list_prepend(_G.tinfo->floating_windows, bd);

        _remove_border(bd);

        e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);

        change_window_border(bd, "default");
    }
}

/* }}} */
/* Action callbacks {{{*/

static void
_e_mod_action_toggle_floating_cb(E_Object   *obj,
                                 const char *params)
{
    toggle_floating(e_border_focused_get());
}

/* }}} */
/* Hooks {{{*/

static void
_desk_before_show(const E_Desk *desk)
{
    if (_G.tinfo->desk == desk) {
        DBG("desk before show: %s \n", desk->name);
        if (!eina_hash_modify(_G.info_hash, desk_hash_key(desk), _G.tinfo))
            eina_hash_add(_G.info_hash, desk_hash_key(desk), _G.tinfo);
    }
    _G.tinfo = NULL;
}

static void
_desk_show(const E_Desk *desk)
{
    _G.tinfo = eina_hash_find(_G.info_hash, desk_hash_key(desk));
    if (!_G.tinfo) {
        /* We need to add a new Tiling_Info, so we weren't on that desk before.
         * As e doesn't call the POST_EVAL-hook (or e_desk_show which then
         * indirectly calls the POST_EVAL) for each window on that desk but only
         * for the focused, we need to get all borders on that desk. */
        DBG("need new info for %s\n", desk->name);
        _G.tinfo = _initialize_tinfo(desk);
    }
}

static void
_e_module_tiling_cb_hook(void *data,
                         void *border)
{
    E_Border *bd = border;
    bool is_master = false,
         is_slave = false;

    DBG("cb-Hook");
    if (!bd || !bd->visible) {
        DBG("bd=%p", bd);
        return;
    }
    if (is_floating_window(bd)) {
        DBG("floating window");
        return;
    }
    if (is_untilable_dialog(bd)) {
        DBG("untilable_dialog");
        return;
    }

    if (!_G.tinfo->nb_cols) {
        DBG("no tiling");
        return;
    }

    is_master = eina_list_data_find(_G.tinfo->master_list, bd) == bd;
    is_slave = eina_list_data_find(_G.tinfo->slave_list, bd) == bd;

    DBG("cb-Hook for %p / %s / %s, changes(size=%d, position=%d, border=%d)"
        " g:%dx%d+%d+%d bdname:%s (%c) %d",
        bd, bd->client.icccm.title, bd->client.netwm.name,
        bd->changes.size, bd->changes.pos, bd->changes.border,
        bd->x, bd->y, bd->w, bd->h, bd->bordername,
        is_master ? 'M' : (is_slave ? 'S': 'N'),
        bd->maximized);

    if (!bd->changes.size && !bd->changes.pos && !bd->changes.border
    && (is_master || is_slave)) {
        DBG("nothing to do");
        return;
    }

    if (!is_master && !is_slave) {
        _add_border(bd);
    } else {
        Border_Extra *extra;

        /* Move or Resize */
        DBG("move or resize");

        extra = eina_hash_find(_G.border_extras, &bd);

        if (!extra) {
            ERR("No extra for %p", bd);
            return;
        }

        if (is_master && !_G.tinfo->master_list->next && !_G.tinfo->slave_list) {
            DBG("forever alone :)");
            if (bd->maximized) {
                extra->x = bd->x;
                extra->y = bd->y;
                extra->w = bd->w;
                extra->h = bd->h;
            } else {
                /* TODO: what if a window doesn't want to be maximized? */
                e_border_unmaximize(bd, E_MAXIMIZE_BOTH);
                e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);
            }
        }
        /* TODO */
        ERR("TODO");
        if (bd->x == extra->x && bd->y == extra->y
        &&  bd->w == extra->w && bd->h == extra->h)
        {
            return;
        }

        if (bd->changes.border && bd->changes.size) {
            e_border_move_resize(bd, extra->x, extra->y,
                                     extra->w, extra->h);
            return;
        }

        DBG("old:%dx%d+%d+%d vs new:%dx%d+%d+%d. step:%dx%d. base:%dx%d",
            extra->x, extra->y, extra->w, extra->h,
            bd->x, bd->y, bd->w, bd->h,
            bd->client.icccm.step_w, bd->client.icccm.step_h,
            bd->client.icccm.base_w, bd->client.icccm.base_h);

        if (abs(extra->w - bd->w) >= bd->client.icccm.step_w) {
            _move_resize_border_in_column(bd, extra, is_master, TILING_RESIZE);
        }
        if (abs(extra->h - bd->h) >= bd->client.icccm.step_h) {
        }
        if (extra->x != bd->x) {
            _move_resize_border_in_column(bd, extra, is_master, TILING_MOVE);
        }
        if (extra->y != bd->y) {
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

    if (_G.currently_switching_desktop)
        return EINA_TRUE;

    DBG("hide-hook\n");

    check_tinfo(bd->desk);

    if (eina_list_data_find(_G.tinfo->floating_windows, bd) == bd) {
        _G.tinfo->floating_windows =
            eina_list_remove(_G.tinfo->floating_windows, bd);
    }

    _remove_border(bd);

    return EINA_TRUE;
}

static Eina_Bool
_e_module_tiling_desk_show(void *data,
                           int   type,
                           void *event)
{
    E_Event_Desk_Show *ev = event;

    _desk_show(ev->desk);
    _G.currently_switching_desktop = 0;

    return EINA_TRUE;
}

static Eina_Bool
_e_module_tiling_desk_before_show(void *data,
                                  int   type,
                                  void *event)
{
    E_Event_Desk_Before_Show *ev = event;

    _desk_before_show(ev->desk);
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
        ti->need_rearrange = 1;
        DBG("set need_rearrange=1\n");
        return EINA_TRUE;
    }

    /* TODO
    if (eina_list_data_find(ti->client_list, ev->border) == ev->border) {
        ti->client_list = eina_list_remove(ti->client_list, ev->border);
        if (ti->desk == get_current_desk()) {
            E_Border *first;

            if ((first = get_first_window(NULL, ti->desk)))
                rearrange_windows(first, EINA_FALSE);
        }
    }
    */

    if (eina_list_data_find(ti->floating_windows, ev->border) == ev->border)
        ti->floating_windows = eina_list_remove(ti->floating_windows, ev->border);

    return EINA_TRUE;
}

static Eina_Bool
_e_module_tiling_desk_set(void *data,
                          int   type,
                          void *event)
{
    /* We use this event to ensure that border desk changes are done correctly
     * because a user can move the window to another desk (and events are
     * fired) involving zone changes or not (depends on the mouse position) */
    E_Event_Border_Desk_Set *ev = event;
    Tiling_Info *_tinfo = eina_hash_find(_G.info_hash, desk_hash_key(ev->desk));

    if (!_tinfo) {
        DBG("create new info for %s\n", ev->desk->name);
        _tinfo = _initialize_tinfo(ev->desk);
    }

    eina_hash_foreach(_G.info_hash, _clear_bd_from_info_hash, ev);
    DBG("desk set\n");

    return EINA_TRUE;
}

/* }}} */
/* Module setup {{{*/

static Eina_Bool
_clear_info_hash(const Eina_Hash *hash,
                 const void      *key,
                 void            *data,
                 void            *fdata)
{
    Tiling_Info *ti = data;

    eina_list_free(ti->floating_windows);
    eina_list_free(ti->master_list);
    eina_list_free(ti->slave_list);
    E_FREE(ti);

    return EINA_TRUE;
}

static Eina_Bool
_clear_border_extras(const Eina_Hash *hash,
                     const void      *key,
                     void            *data,
                     void            *fdata)
{
    Border_Extra *be = data;

    E_FREE(be);

    return EINA_TRUE;
}

EAPI E_Module_Api e_modapi =
{
    E_MODULE_API_VERSION,
    "Tiling"
};

EAPI void *
e_modapi_init(E_Module *m)
{
    char buf[PATH_MAX];
    E_Desk *desk;

    tiling_g.module = m;

    if (tiling_g.log_domain < 0) {
        tiling_g.log_domain = eina_log_domain_register("tiling", NULL);
        if (tiling_g.log_domain < 0) {
            EINA_LOG_CRIT("could not register log domain 'tiling'");
        }
    }


    snprintf(buf, sizeof(buf), "%s/locale", e_module_dir_get(m));
    bindtextdomain(PACKAGE, buf);
    bind_textdomain_codeset(PACKAGE, "UTF-8");

    _G.info_hash = eina_hash_string_small_new(NULL);

    _G.border_extras = eina_hash_pointer_new(NULL);

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
        e_action_predef_name_set(D_("Tiling"), D_(_title), _name,            \
                                 NULL, NULL, 0);                             \
     }                                                                       \
}

    /* Module's actions */
    ACTION_ADD(_G.act_togglefloat, _e_mod_action_toggle_floating_cb,
               "Toggle floating", "toggle_floating");
#undef ACTION_ADD

    /* Configuration entries */
    snprintf(buf, sizeof(buf), "%s/e-module-tiling.edj", e_module_dir_get(m));
    e_configure_registry_category_add("windows", 50, D_("Windows"), NULL,
                                      "preferences-system-windows");
    e_configure_registry_item_add("windows/tiling", 150, D_("Tiling"), NULL,
                                  buf, e_int_config_tiling_module);

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

    tiling_g.config = e_config_domain_load("module.tiling", _G.config_edd);
    if (!tiling_g.config) {
        tiling_g.config = E_NEW(Config, 1);
        tiling_g.config->float_too_big_windows = 1;
    }

    E_CONFIG_LIMIT(tiling_g.config->tile_dialogs, 0, 1);
    E_CONFIG_LIMIT(tiling_g.config->float_too_big_windows, 0, 1);

    desk = get_current_desk();
    _G.current_zone = desk->zone;
    _G.tinfo = _initialize_tinfo(desk);

    DBG("initialized");
    return m;
}

EAPI int
e_modapi_shutdown(E_Module *m)
{

    if (tiling_g.log_domain >= 0) {
        DBG("shutdown!");
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
     e_action_predef_name_del(D_("Tiling"), D_(title)); \
     e_action_del(value);                               \
     act = NULL;                                        \
}
    ACTION_DEL(_G.act_toggletiling, "Toggle tiling", "toggle_tiling");
    ACTION_DEL(_G.act_togglefloat, "Toggle floating", "toggle_floating");
    ACTION_DEL(_G.act_switchtiling, "Switch tiling mode", "switch_tiling");
#undef ACTION_DEL

    e_configure_registry_item_del("windows/tiling");
    e_configure_registry_category_del("windows");

    E_FREE(tiling_g.config);
    E_CONFIG_DD_FREE(_G.config_edd);
    E_CONFIG_DD_FREE(_G.vdesk_edd);

    tiling_g.module = NULL;

    eina_hash_foreach(_G.info_hash, _clear_info_hash, NULL);
    eina_hash_free(_G.info_hash);
    _G.info_hash = NULL;

    eina_hash_foreach(_G.info_hash, _clear_border_extras, NULL);
    eina_hash_free(_G.border_extras);
    _G.border_extras = NULL;

    _G.tinfo = NULL;

    return 1;
}

EAPI int
e_modapi_save(E_Module *m)
{
    e_config_domain_save("module.tiling", _G.config_edd, tiling_g.config);

    return EINA_TRUE;
}
/* }}} */
