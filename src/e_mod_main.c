#include "e.h"
#include "trivials.h"
#include "config.h"
#include "e_mod_main.h"
#include "e_mod_config.h"
#include "e_border.h"
#include "e_shelf.h"
#include <math.h>
#include <stdbool.h>

/* Use TILING_DEBUG-define to toggle displaying lots of debugmessages */
#define TILING_DEBUG

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
                       *act_switchtiling,
                       *act_moveleft,
                       *act_moveright,
                       *act_movetop,
                       *act_movebottom;
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
   .act_moveleft = NULL,
   .act_moveright = NULL,
   .act_movetop = NULL,
   .act_movebottom = NULL,
};

static void _e_mod_action_toggle_tiling_cb(E_Object   *obj,
                                           const char *params);
static void _e_mod_action_toggle_floating_cb(E_Object   *obj,
                                             const char *params);
static void _e_mod_action_switch_tiling_cb(E_Object   *obj,
                                           const char *params);
static void _e_mod_action_move_left(E_Object   *obj,
                                    const char *params);
static void _e_mod_action_move_right(E_Object   *obj,
                                     const char *params);
static void _e_mod_action_move_top(E_Object   *obj,
                                   const char *params);
static void _e_mod_action_move_bottom(E_Object   *obj,
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

#define ORIENT_BOTTOM(x)                \
((x == E_GADCON_ORIENT_CORNER_BL)       \
 || (x == E_GADCON_ORIENT_CORNER_BR)    \
 || (x == E_GADCON_ORIENT_BOTTOM))

#define ORIENT_TOP(x)                   \
((x == E_GADCON_ORIENT_CORNER_TL)       \
 || (x == E_GADCON_ORIENT_CORNER_TR)    \
 || (x == E_GADCON_ORIENT_TOP))

#define ORIENT_LEFT(x)                  \
((x == E_GADCON_ORIENT_CORNER_LB)       \
 || (x == E_GADCON_ORIENT_CORNER_LT)    \
 || (x == E_GADCON_ORIENT_LEFT))

#define ORIENT_RIGHT(x)                 \
((x == E_GADCON_ORIENT_CORNER_RB)       \
 || (x == E_GADCON_ORIENT_CORNER_RT)    \
 || (x == E_GADCON_ORIENT_RIGHT))


/* Utils {{{ */

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
    res->mainbd_width = -1;
    res->desk = desk;
    res->big_perc = tiling_g.config->big_perc;
    res->need_rearrange = 0;
    eina_hash_add(_G.info_hash, desk_hash_key(desk), res);

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

static E_Tiling_Type
layout_for_desk(E_Desk *desk)
{
    if (tiling_g.config->tiling_mode == E_TILING_INDIVIDUAL) {
        struct _Config_vdesk *vd;

        vd = get_vdesk(tiling_g.config->vdesks, desk->x, desk->y,
                       desk->zone->num);

        return vd ? vd->layout : E_TILING_NONE;
    }
    return tiling_g.config->tiling_mode;
}

#ifdef TILING_DEBUG
static void
print_borderlist()
{
    int wc = 0;

    if (!_G.tinfo)
        return;

    printf("\n\nTILING_DEBUG: Tiling-Borderlist for \"%s\":\n",
           desk_hash_key(_G.tinfo->desk));
    for (Eina_List *l = _G.tinfo->master_list; l; l = l->next, wc++) {
        E_Border *lbd = l->data;

        printf("  #M:%d = %p, %s, %s, %s, desk %s)\n",
               wc, lbd, lbd->client.icccm.name,
               lbd->client.icccm.title, lbd->client.netwm.name,
               desk_hash_key(lbd->desk));
        printf("  current = %p, next = %p, prev = %p\n", l, l->next, l->prev);
    }
    for (Eina_List *l = _G.tinfo->slave_list; l; l = l->next, wc++) {
        E_Border *lbd = l->data;

        printf("  #S:%d = %p, %s, %s, %s, desk %s)\n",
               wc, lbd, lbd->client.icccm.name,
               lbd->client.icccm.title, lbd->client.netwm.name,
               desk_hash_key(lbd->desk));
        printf("  current = %p, next = %p, prev = %p\n", l, l->next, l->prev);
    }
    printf("TILING_DEBUG: End of Borderlist\n\n");
}

#endif

/* Checks for windows which are bigger than width/height in their min_w/min_h-
 * attributes and therefore need to be set to floating
 *
 * returns True if the given border was affected */
static Eina_Bool
check_for_too_big_windows(int       width,
                          int       height,
                          E_Border *bd)
{
    Eina_List *l;
    E_Border *lbd;

    EINA_LIST_FOREACH(e_border_focus_stack_get(), l, lbd) {
        TILE_LOOP_DESKCHECK;

        if (TILE_LOOP_CHECKS(lbd))
            continue;

        if (lbd->client.icccm.min_w > width || lbd->client.icccm.min_h > height) {
            toggle_floating(lbd);
            /* If this was the window this call was about,
             * we don't need to change anything */
            if (bd && (lbd == bd))
                return EINA_TRUE;
        }
    }
    return EINA_FALSE;
}

static void
change_window_border(E_Border *bd,
                     char     *bordername)
{
   eina_stringshare_replace(&bd->bordername, bordername);
   bd->client.border.changed = 1;
   bd->changed = 1;
}

/* }}} */
/* Move {{{ */
/* Moves the nth list-entry to the left */
static Eina_Bool
border_move_to_left(E_Border *bd,
                    int       times)
{
    /* TODO
    Eina_List *n, *p;
    void *data;

    if (!bd || !_G.tinfo)
        return EINA_FALSE;
    if (!(n = eina_list_data_find_list(_G.tinfo->client_list, bd)))
        return EINA_FALSE;
    if (!(p = n->prev))
        return EINA_FALSE;

    data = n->data;
    for (int c = 0; c < (times - 1); c++)
        if (!(p = p->prev))
            return EINA_FALSE;

    _G.tinfo->client_list = eina_list_remove_list(_G.tinfo->client_list, n);
    _G.tinfo->client_list = eina_list_prepend_relative_list(
        _G.tinfo->client_list, data, p);
    */

    return EINA_TRUE;
}

/* Move to right is basically the same as move to left of (num+1) */
static Eina_Bool
border_move_to_right(E_Border *bd,
                     int       times)
{
    /* TODO
    Eina_List *n, *p;
    void *data;

    if (!bd || !_G.tinfo)
        return EINA_FALSE;
    if (!(n = eina_list_data_find_list(_G.tinfo->client_list, bd)))
        return EINA_FALSE;
    if (!(p = n->next))
        return EINA_FALSE;

    data = n->data;
    for (int c = 0; c < (times - 1); c++)
        if (!(p = p->next))
            return EINA_FALSE;

    _G.tinfo->client_list = eina_list_remove_list(_G.tinfo->client_list, n);
    _G.tinfo->client_list = eina_list_append_relative_list(
        _G.tinfo->client_list, data, p);
    */

    return EINA_TRUE;
}

/* }}} */
/* Toggle Floating {{{ */

static void
toggle_floating(E_Border *bd)
{
    if (!bd || !_G.tinfo)
        return;

    if (eina_list_data_find(_G.tinfo->floating_windows, bd) == bd) {
        _G.tinfo->floating_windows =
            eina_list_remove(_G.tinfo->floating_windows, bd);

        if (!tiling_g.config->dont_touch_borders
            && tiling_g.config->tiling_border
            && (!bd->bordername
                || strcmp(bd->bordername, tiling_g.config->tiling_border)))
        {
            change_window_border(bd, tiling_g.config->tiling_border);
        }

        e_border_idler_before();
    } else {
        int w = bd->w,
            h = bd->h;

        /* To give the user a bit of feedback we restore the original border */
        /* TODO: save the original border, don't just restore the default one */
        _G.tinfo->floating_windows =
            eina_list_prepend(_G.tinfo->floating_windows, bd);

        if (!tiling_g.config->dont_touch_borders
            && tiling_g.config->floating_border
            && (!bd->bordername
                || strcmp(bd->bordername, tiling_g.config->floating_border)))
        {
            change_window_border(bd, tiling_g.config->floating_border);
        }
        e_border_idler_before();
        e_border_resize(bd, w, h);
    }
}

/* }}} */

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
#ifdef TILING_DEBUG
    printf("TILING_DEBUG: desk show. %s\n", desk->name);
    print_borderlist();
    printf("TILING_DEBUG: desk show done\n");
#endif
}

static void
maximize(E_Border *bd)
{
   int offset_top = 0,
       offset_left = 0;
   Eina_List *l;
   E_Shelf *sh;

   /* However, we still need to check if any of the shelves produces
    * an offset */
   EINA_LIST_FOREACH(e_shelf_list(), l, sh) {
        if (!sh || (sh->zone != bd->zone)
            || !shelf_show_on_desk(sh, bd->desk)
            || sh->cfg->overlap)
          continue;

        if (ORIENT_TOP(sh->gadcon->orient))
          offset_top += sh->h;
        else
          if (ORIENT_LEFT(sh->gadcon->orient))
            offset_left += sh->w;
   }
   DBG("maximizing the window");
   e_border_move(bd, bd->zone->x + offset_left,
                 bd->zone->y + offset_top);
   e_border_unmaximize(bd, E_MAXIMIZE_BOTH);
   e_border_maximize(bd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);
}

/* Action callbacks {{{*/

static void
_e_mod_action_toggle_tiling_cb(E_Object   *obj,
                               const char *params)
{
    tiling_g.config->tiling_enabled = !tiling_g.config->tiling_enabled;
    e_mod_tiling_rearrange();
}

static void
_e_mod_action_toggle_floating_cb(E_Object   *obj,
                                 const char *params)
{
    toggle_floating(e_border_focused_get());
}

static void
toggle_layout(E_Tiling_Type *layout)
{
    switch(*layout) {
      case E_TILING_NONE:
      case E_TILING_INDIVIDUAL:
        *layout = E_TILING_GRID;
        break;
      case E_TILING_GRID:
        *layout = E_TILING_BIGMAIN;
        break;
      case E_TILING_BIGMAIN:
        *layout = E_TILING_NONE;
        break;
    }
}

static void
_e_mod_action_switch_tiling_cb(E_Object   *obj,
                               const char *params)
{
    if (tiling_g.config->tiling_mode != E_TILING_INDIVIDUAL) {
        toggle_layout(&tiling_g.config->tiling_mode);
    } else {
        E_Desk *desk = get_current_desk();
        struct _Config_vdesk *vd;

        if (!desk)
            return;

        vd = get_vdesk(tiling_g.config->vdesks, desk->x, desk->y,
                       desk->zone->num);
        if (!vd) {
            /* There was no config entry. Probably the vdesk-configuration
             * changed but the user didn't open the tiling config yet. */
            vd = malloc(sizeof(struct _Config_vdesk));

            vd->x = desk->x;
            vd->y = desk->y;
            vd->layout = tiling_g.config->tiling_mode;
            tiling_g.config->vdesks =
                eina_list_append(tiling_g.config->vdesks, vd);
        }
        toggle_layout(&vd->layout);
    }

    e_mod_tiling_rearrange();

    e_config_save_queue();
}

static void
_e_mod_action_move_left(E_Object   *obj,
                        const char *params)
{
   E_Border *bd = e_border_focused_get();

   if (!bd)
       return;

   /* TODO */

   switch (layout_for_desk(bd->desk)) {
     case E_TILING_NONE:
     case E_TILING_INDIVIDUAL:
       break;
     case E_TILING_GRID:
       break;
     case E_TILING_BIGMAIN:
       break;
   }
}

static void
_e_mod_action_move_right(E_Object   *obj,
                         const char *params)
{
   E_Border *bd = e_border_focused_get();

   if (!bd)
       return;

   /* TODO */
}


static void
_e_mod_action_move_top(E_Object   *obj,
                       const char *params)
{
    E_Border *bd = e_border_focused_get();

    if (!bd)
        return;

    /* TODO */

    switch (layout_for_desk(bd->desk)) {
     case E_TILING_NONE:
     case E_TILING_INDIVIDUAL:
       break;
      case E_TILING_GRID:
        break;
      case E_TILING_BIGMAIN:
        break;

    }
}

static void
_e_mod_action_move_bottom(E_Object   *obj,
                          const char *params)
{
   E_Border *bd = e_border_focused_get();

   if (!bd)
      return;

   /* TODO */

   switch (layout_for_desk(bd->desk)) {
     case E_TILING_NONE:
     case E_TILING_INDIVIDUAL:
       break;
     case E_TILING_GRID:
      break;
     case E_TILING_BIGMAIN:
      break;
   }
}
/* }}} */
/* Hooks {{{*/

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

    if (!tiling_g.config->tiling_enabled
    || layout_for_desk(bd->desk) == E_TILING_NONE) {
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
        Border_Extra *extra = E_NEW(Border_Extra, 1);

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

        if (!tiling_g.config->dont_touch_borders
        && tiling_g.config->tiling_border
        && ((bd->bordername && strcmp(bd->bordername,
                                       tiling_g.config->tiling_border))
            || !bd->bordername))
        {
            change_window_border(bd, tiling_g.config->tiling_border);
        }

        if (_G.tinfo->master_list) {
           /*TODO: Put in slaves */
           DBG("put in slaves");
        } else {
            maximize(bd);
            _G.tinfo->master_list = eina_list_append(_G.tinfo->master_list, bd);
        }
    } else {
        Border_Extra *extra;

        /* Move or Resize */
        DBG("move or resize");
        /* TODO */

        extra = eina_hash_find(_G.border_extras, &bd);

        if (!extra) {
             ERR("No extra for %p", bd);
        }

        if (is_master && !_G.tinfo->master_list->next) {
            DBG("forever alone :)");
            if (bd->maximized) {
                 extra->x = bd->x;
                 extra->y = bd->y;
                 extra->w = bd->w;
                 extra->h = bd->h;
            } else {
                 /* TODO: what if a window doesn't want to be maximized? */
                 maximize(bd);
            }
        }
    }
}

static Eina_Bool
_e_module_tiling_hide_hook(void *data,
                           int   type,
                           void *event)
{
    static Tiling_Info *_tinfo = NULL;
    E_Event_Border_Hide *ev = event;

    DBG("hide-hook\n");

    if (_G.currently_switching_desktop)
        return EINA_TRUE;

    eina_hash_del(_G.border_extras, &ev->border, NULL);

    /* Ensure that the border is deleted from all available desks */
    for (Eina_List *l = e_manager_list(); l; l = l->next) {
        for (Eina_List *ll = ((E_Manager *)(l->data))->containers;
             ll; ll = ll->next)
        {
            for (Eina_List *lll = ((E_Container *)(ll->data))->zones;
                 lll; lll = lll->next)
            {
                E_Zone *zone = lll->data;

                for (int i = 0; i < (zone->desk_x_count * zone->desk_y_count);
                     i++)
                {
                    E_Desk *desk = zone->desks[i];

                    if (!(_tinfo = eina_hash_find(_G.info_hash,
                                                  desk_hash_key(desk))))
                        continue;

                    if (eina_list_data_find(_tinfo->master_list, ev->border)
                        == ev->border)
                    {
                        _tinfo->master_list =
                            eina_list_remove(_tinfo->master_list, ev->border);
                        continue;
                    }
                    if (eina_list_data_find(_tinfo->slave_list, ev->border)
                        == ev->border)
                    {
                        _tinfo->slave_list =
                            eina_list_remove(_tinfo->slave_list, ev->border);
                        continue;
                    }
                    /*TODO: reorganize desk */
                }
            }
        }
    }

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

static Eina_Bool
_e_module_tiling_mouse_move(void *data,
                            int   type,
                            void *event)
{
    Ecore_Event_Mouse_Move *ev = event;

    if (!_G.current_zone
    || !E_INSIDE(ev->root.x, ev->root.y,
                 _G.current_zone->x, _G.current_zone->y,
                 _G.current_zone->w, _G.current_zone->h))
    {
        E_Desk *desk = get_current_desk();

        _desk_before_show(_G.tinfo->desk);
        _G.current_zone = desk->zone;
        _desk_show(desk);
    }

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
    /*
    _G.handler_desk_before_show =
        ecore_event_handler_add(E_EVENT_DESK_BEFORE_SHOW,
                                _e_module_tiling_desk_before_show, NULL);
    */
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
    ACTION_ADD(_G.act_toggletiling, _e_mod_action_toggle_tiling_cb,
               "Toggle tiling", "toggle_tiling");
    ACTION_ADD(_G.act_togglefloat, _e_mod_action_toggle_floating_cb,
               "Toggle floating", "toggle_floating");
    ACTION_ADD(_G.act_switchtiling, _e_mod_action_switch_tiling_cb,
                "Switch tiling mode", "switch_tiling");
    ACTION_ADD(_G.act_moveleft, _e_mod_action_move_left,
               "Move window to the left", "tiling_move_left");
    ACTION_ADD(_G.act_moveright, _e_mod_action_move_right,
               "Move window to the right", "tiling_move_right");
    ACTION_ADD(_G.act_movebottom, _e_mod_action_move_top,
               "Move window to the bottom", "tiling_move_bottom");
    ACTION_ADD(_G.act_movetop, _e_mod_action_move_bottom,
               "Move window to the top", "tiling_move_top");
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
    E_CONFIG_VAL(_G.config_edd, Config, tiling_enabled, INT);
    E_CONFIG_VAL(_G.config_edd, Config, tiling_mode, INT);
    E_CONFIG_VAL(_G.config_edd, Config, dont_touch_borders, INT);
    E_CONFIG_VAL(_G.config_edd, Config, tile_dialogs, INT);
    E_CONFIG_VAL(_G.config_edd, Config, float_too_big_windows, INT);
    E_CONFIG_VAL(_G.config_edd, Config, grid_rows, INT);
    E_CONFIG_VAL(_G.config_edd, Config, grid_distribute_equally, INT);
    E_CONFIG_VAL(_G.config_edd, Config, space_between, INT);
    E_CONFIG_VAL(_G.config_edd, Config, between_x, INT);
    E_CONFIG_VAL(_G.config_edd, Config, between_y, INT);
    E_CONFIG_VAL(_G.config_edd, Config, floating_border, STR);
    E_CONFIG_VAL(_G.config_edd, Config, tiling_border, STR);
    E_CONFIG_VAL(_G.config_edd, Config, big_perc, DOUBLE);

    E_CONFIG_LIST(_G.config_edd, Config, vdesks, _G.vdesk_edd);
    E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, x, INT);
    E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, y, INT);
    E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, zone_num, INT);
    E_CONFIG_VAL(_G.vdesk_edd, struct _Config_vdesk, layout, INT);

    tiling_g.config = e_config_domain_load("module.tiling", _G.config_edd);
    if (!tiling_g.config) {
        tiling_g.config = E_NEW(Config, 1);
        tiling_g.config->tiling_mode = E_TILING_BIGMAIN;
        tiling_g.config->float_too_big_windows = 1;
        tiling_g.config->big_perc = 0.5;
        tiling_g.config->grid_rows = 2;
    } else {
        /* Because e doesn't strdup these when loading from configuration,
         * we have to */
        if (tiling_g.config->floating_border)
            tiling_g.config->floating_border =
                strdup(tiling_g.config->floating_border);
        if (tiling_g.config->tiling_border)
            tiling_g.config->tiling_border =
                strdup(tiling_g.config->tiling_border);
    }
    if (!tiling_g.config->tiling_border)
        tiling_g.config->tiling_border = strdup("pixel");
    if (!tiling_g.config->floating_border)
        tiling_g.config->floating_border = strdup("default");

    E_CONFIG_LIMIT(tiling_g.config->tiling_enabled, 0, 1);
    E_CONFIG_LIMIT(tiling_g.config->dont_touch_borders, 0, 1);
    E_CONFIG_LIMIT(tiling_g.config->tiling_mode, E_TILING_NONE,
                                                 E_TILING_GRID);
    E_CONFIG_LIMIT(tiling_g.config->tile_dialogs, 0, 1);
    E_CONFIG_LIMIT(tiling_g.config->float_too_big_windows, 0, 1);
    E_CONFIG_LIMIT(tiling_g.config->grid_rows, 1, 12);
    E_CONFIG_LIMIT(tiling_g.config->grid_distribute_equally, 0, 1);
    E_CONFIG_LIMIT(tiling_g.config->big_perc, 0.1, 1);
    E_CONFIG_LIMIT(tiling_g.config->space_between, 0, 1);

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
    ACTION_DEL(_G.act_moveleft, "Move window to the left", "tiling_move_left");
    ACTION_DEL(_G.act_moveright, "Move window to the right", "tiling_move_right");
    ACTION_DEL(_G.act_movebottom, "Move window to the bottom", "tiling_move_bottom");
    ACTION_DEL(_G.act_movetop, "Move window to the top", "tiling_move_top");
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

    return 1;
}
/* }}} */
