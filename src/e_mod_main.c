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
static E_Border *get_first_window(const E_Border *exclude,
                                  const E_Desk   *desk);
static void toggle_floating(E_Border *bd);
static void rearrange_windows(E_Border *bd,
                              Eina_Bool remove_bd);
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

/* Returns the first window from focus-stack (or NULL), avoiding *exclude if specified */
static E_Border *
get_first_window(const E_Border *exclude,
                 const E_Desk   *desk)
{
    Eina_List *l;
    E_Border *lbd;

    EINA_LIST_FOREACH(e_border_focus_stack_get(), l, lbd) {
        if (exclude
            && ((lbd == exclude) || (lbd->desk != exclude->desk)))
            continue;
        if (!exclude && desk && (lbd->desk != desk))
            continue;
        if (TILE_LOOP_CHECKS(lbd))
            continue;
        return lbd;
    }

    return NULL;
}

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
        rearrange_windows(bd, EINA_FALSE);
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
/* Rearrange {{{ */

static void
rearrange_windows_grid(E_Border *bd,
                       int remove_bd,
                       int window_count)
{
    Eina_List *l;
    E_Border *lbd;
    E_Shelf *sh;
    int wc = 0;
    int wf,
        wh;
    int gridrows;
    int *shelf_collision_vert,
        *shelf_collision_horiz,
        *offset_top,
        *offset_left;
    int highest_collision_vert = 0,
        highest_collision_horiz = 0;
    int sub_space_x,
        sub_space_y;
    int windows_per_row;

    if (tiling_g.config->grid_distribute_equally) {
        int internal = 1;

        if (window_count > 1)
            while (((double)window_count / (double)internal) > (double)internal)
                internal++;
        gridrows = max(internal, 1);
    } else {
        gridrows = max(min(tiling_g.config->grid_rows, window_count), 1);
    }

    wf = (bd->zone->w / gridrows);
    windows_per_row = max((int)ceil((double)window_count / (double)gridrows), 1);

    shelf_collision_vert = alloca(sizeof(int) * gridrows);
    bzero(shelf_collision_vert, sizeof(int) * gridrows);

    shelf_collision_horiz = alloca(sizeof(int) * windows_per_row);
    bzero(shelf_collision_horiz, sizeof(int) * windows_per_row);

    offset_top = alloca(sizeof(int) * gridrows);
    bzero(offset_top, sizeof(int) * gridrows);

    offset_left = alloca(sizeof(int) * windows_per_row);
    bzero(offset_left, sizeof(int) * windows_per_row);

    sub_space_x = (tiling_g.config->space_between ? tiling_g.config->between_x : 0);
    sub_space_y = (tiling_g.config->space_between ? tiling_g.config->between_y : 0);


    /* Loop through all the shelves on this screen (=zone) to get their space */
    /* NOTE:
     * shelf detection in gridmode has a tolerance area of the height/width (depends
     * on the orientation) of your shelves if you have more than one.
     *
     * If you have found a good way to deal with this problem in a simple manner,
     * please send a patch :-).
     */
    EINA_LIST_FOREACH(e_shelf_list(), l, sh) {
        if (!sh || (sh->zone != bd->zone)
        || !shelf_show_on_desk(sh, bd->desk) || sh->cfg->overlap)
            continue;

        E_Gadcon_Orient orient = sh->gadcon->orient;
        /* Every row between sh_min and sh_max needs to be flagged */
        if (ORIENT_BOTTOM(orient) || ORIENT_TOP(orient)) {
            int sh_min = sh->x,
                sh_max = sh->x + sh->w;

            for (int c = 0; c < gridrows; c++)
            {
                int row_min = ((c % gridrows) * wf),
                    row_max = row_min + wf;

                if (!shelf_collision_vert[c] &&
                    (between(row_min, sh_min, sh_max) ||
                     between(sh_min, row_min, row_max)))
                {
                    shelf_collision_vert[c] = sh->h;
                    if (sh->h > highest_collision_vert)
                        highest_collision_vert = sh->h;
                    if (ORIENT_TOP(orient))
                        offset_top[c] = sh->h;
                }
            }
        }
        else
        if (ORIENT_LEFT(orient) || ORIENT_RIGHT(orient)) {
            int sh_min = sh->y,
                sh_max = sh->y + sh->h;

            for (int c = 0; c < windows_per_row; c++) {
                int row_min = c * (bd->zone->h / windows_per_row),
                    row_max = row_min + (bd->zone->h / windows_per_row);

                if (!shelf_collision_horiz[c] &&
                    (between(row_min, sh_min, sh_max) ||
                     between(sh_min, row_min, row_max)))
                {
                    shelf_collision_horiz[c] = sh->w;
                    if (sh->w > highest_collision_horiz)
                        highest_collision_horiz = sh->w;
                    if (ORIENT_LEFT(orient))
                        offset_left[c] = sh->w;
                }
            }
        }
    }

    for (int c = 0; c < gridrows; c++)
        shelf_collision_vert[c] = bd->zone->h
            - (sub_space_y * (windows_per_row - 1)) - shelf_collision_vert[c];
    for (int c = 0; c < windows_per_row; c++)
        shelf_collision_horiz[c] = bd->zone->w
            - (sub_space_x * (gridrows - 1)) - shelf_collision_horiz[c];

    /* Check for too big windows. We're pessimistic for the height-value and use the
     * one with the lowest available space (due to shelves) */
    if (tiling_g.config->float_too_big_windows
    && check_for_too_big_windows(((bd->zone->w - highest_collision_horiz) /
                                   max((window_count < (gridrows + 1) ?
                                        window_count : gridrows), 1)),
                                  ((bd->zone->h - highest_collision_vert) /
                                     windows_per_row),
                                  bd))
        return;

    /* TODO:
    EINA_LIST_FOREACH(_G.tinfo->client_list, l, lbd) {
        int row_horiz,
            row_vert;
        int hf;

        TILE_LOOP_DESKCHECK;
        if (TILE_LOOP_CHECKS(lbd))
            continue;

        if (remove_bd && lbd == bd)
            continue;

        row_horiz = (wc % gridrows);
        row_vert = (wc / gridrows);
        wf = (shelf_collision_horiz[row_vert] / gridrows);
        hf = (shelf_collision_vert[row_horiz] / windows_per_row);
        move_resize(lbd,
                    (row_horiz * wf) + offset_left[row_vert]
                                     + (sub_space_x * row_horiz),
                    (row_vert  * hf) + offset_top[row_horiz]
                                     + (sub_space_y * row_vert),
                    wf, hf);
        wc++;
    }
    */
}
static void
rearrange_windows_bigmain(E_Border *bd,
                          int remove_bd,
                          int window_count)
{
    E_Border *lbd;
    Eina_List *l;
    E_Shelf *sh;
    int wc = 0,
        hf;
    int bigw = bd->zone->w;
    int bigh = bd->zone->h;
    int offset_top = 0, offset_left = 0;
    int smallh = bigh,
        smallw;
    int sub_space_x = (tiling_g.config->space_between ?
                       tiling_g.config->between_x : 0);
    int sub_space_y = (tiling_g.config->space_between ?
                       tiling_g.config->between_y : 0);

    /* Loop through all the shelves on this screen (=zone) to get their space */
    EINA_LIST_FOREACH(e_shelf_list(), l, sh) {
        E_Gadcon_Orient orient;

        if (!sh || (sh->zone != bd->zone)
        || !shelf_show_on_desk(sh, bd->desk)
        || sh->cfg->overlap)
            continue;

        /* Decide what to do based on the orientation of the shelf */
        orient = sh->gadcon->orient;
        if (ORIENT_BOTTOM(orient) || ORIENT_TOP(orient)) {
            if (sh->x <= (bigw * _G.tinfo->big_perc))
                bigh -= sh->h;
            if ((sh->x + sh->w) >= (bigw * _G.tinfo->big_perc))
                smallh -= sh->h;
            if (ORIENT_TOP(orient))
                offset_top = sh->h;
        }
        if (ORIENT_LEFT(orient) || ORIENT_RIGHT(orient))
            bigw -= sh->w;
        if (ORIENT_LEFT(orient))
            offset_left = sh->w;
    }

    smallw = bigw;
    bigw *= _G.tinfo->big_perc;
    smallw -= (bigw + sub_space_x);
    smallh -= (sub_space_y * (window_count - 2));
    hf = (smallh / (window_count - 1));
    if (tiling_g.config->float_too_big_windows
    && check_for_too_big_windows(bigw, hf, bd))
        return;

    /* Handle Small windows */
    /* TODO:
    EINA_LIST_FOREACH(_G.tinfo->client_list, l, lbd) {
        TILE_LOOP_DESKCHECK;
        if (TILE_LOOP_CHECKS(lbd))
            continue;

        if (lbd == _G.tinfo->mainbd)
            continue;
        move_resize(lbd, sub_space_x + offset_left + bigw,
                    (wc * hf) + offset_top + (wc * sub_space_y), smallw, hf);
        wc++;
    }
    */

    if (_G.tinfo->mainbd) {
        _G.tinfo->mainbd_width = bigw;
        move_resize(_G.tinfo->mainbd, offset_left, offset_top, bigw, bigh);
    }
}

/* The main tiling happens here. Layouts are calculated and windows are
 * moved/resized */
static void
rearrange_windows(E_Border *bd,
                  Eina_Bool remove_bd)
{
    Eina_List *l;
    E_Border *lbd;
    E_Shelf *sh;
    int window_count;
    E_Tiling_Type layout;

    if (!bd || !_G.tinfo || !tiling_g.config->tiling_enabled)
        return;
    if (_G.tinfo->desk && (bd->desk != _G.tinfo->desk)) {
        E_Desk *desk = get_current_desk();
        /* We need to verify this because when having multiple zones (xinerama)
         * it's possible that tinfo is initialized for zone 1 even though
         * it should be zone 0 */
        if (desk != _G.tinfo->desk)
            _desk_show(desk);
        else
            return;
    }

#ifdef TILING_DEBUG
    printf("TILING_DEBUG: rearrange_windows()\n");
    print_borderlist();
#endif

    /* Take care of our own tinfo->client_list */
    /* TODO
    if (eina_list_data_find(_G.tinfo->client_list, bd) != bd) {
        if (!remove_bd)
            _G.tinfo->client_list = eina_list_append(_G.tinfo->client_list, bd);
    } else {
        if (remove_bd)
            _G.tinfo->client_list = eina_list_remove(_G.tinfo->client_list, bd);
    }
    */

    /* Check if the window is set floating */
    if (eina_list_data_find(_G.tinfo->floating_windows, bd) == bd) {
        if (remove_bd)
            _G.tinfo->floating_windows = eina_list_remove(_G.tinfo->floating_windows, bd);
        return;
    }

    /* Check if the window is a dialog and whether we should handle it */
    if (!tiling_g.config->tile_dialogs
    &&  ((bd->client.icccm.transient_for != 0)
         || (bd->client.netwm.type == ECORE_X_WINDOW_TYPE_DIALOG)))
        return;

    window_count = (remove_bd ? 0 : 1);
    layout = layout_for_desk(bd->desk);
    if (layout == E_TILING_NONE) {
        return;
    }
    if (layout == E_TILING_BIGMAIN) {
        if (remove_bd && (bd == _G.tinfo->mainbd))
            /* If the main border is getting removed, we need to find another
             * one */
            _G.tinfo->mainbd = get_first_window(bd, NULL);

        if (!remove_bd && !_G.tinfo->mainbd)
            _G.tinfo->mainbd = bd;
    }

    /* Loop through all windows to count them */
    EINA_LIST_FOREACH(e_border_focus_stack_get(), l, lbd) {
        TILE_LOOP_DESKCHECK;
        if (TILE_LOOP_CHECKS(lbd))
            continue;

        if (!tiling_g.config->dont_touch_borders
        && tiling_g.config->tiling_border && !remove_bd
        && ((lbd->bordername && strcmp(lbd->bordername,
                                       tiling_g.config->tiling_border))
            || !lbd->bordername))
        {
            change_window_border(lbd, tiling_g.config->tiling_border);
        }
        if (lbd == bd)
            continue;
        if (lbd->visible == 0)
            continue;

        window_count++;
    }

    /* If there are no other windows, it's easy: just maximize */
    if (window_count == 1) {
        lbd = (remove_bd ? get_first_window(bd, NULL) : bd);
        if (lbd) {
            int offset_top = 0,
                offset_left = 0;
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
            DBG("maximizing the window\n");
            e_border_move(lbd, lbd->zone->x + offset_left,
                               lbd->zone->y + offset_top);
            e_border_unmaximize(lbd, E_MAXIMIZE_BOTH);
            e_border_maximize(lbd, E_MAXIMIZE_EXPAND | E_MAXIMIZE_BOTH);
            _G.tinfo->single_win = lbd;
        }
        return;
    }

    if (_G.tinfo->single_win) {
        /* If we previously maximized a window, we need to unmaximize it or it
         * takes up all the space in bigmain mode */
        DBG("unmaximizing\n");
        e_border_unmaximize(_G.tinfo->single_win, E_MAXIMIZE_BOTH);
        _G.tinfo->single_win = NULL;
    }

    switch (layout) {
      case E_TILING_GRID:
        rearrange_windows_grid(bd, remove_bd, window_count);
        break;

      case E_TILING_BIGMAIN:
        rearrange_windows_bigmain(bd, remove_bd, window_count);
        break;
    }
    DBG("rearrange done\n\n");
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
    } else {
        if (_G.tinfo->need_rearrange) {
            E_Border *first;

            DBG("need_rearrange\n");
            if ((first = get_first_window(NULL, desk)))
                rearrange_windows(first, EINA_FALSE);
            _G.tinfo->need_rearrange = 0;
        }
    }
#ifdef TILING_DEBUG
    printf("TILING_DEBUG: desk show. %s\n", desk->name);
    print_borderlist();
    printf("TILING_DEBUG: desk show done\n");
#endif
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

   switch (layout_for_desk(bd->desk)) {
     case E_TILING_NONE:
     case E_TILING_INDIVIDUAL:
       break;
     case E_TILING_GRID:
       if (border_move_to_left(bd, 1))
           rearrange_windows(bd, EINA_FALSE);
       break;
     case E_TILING_BIGMAIN:
       _G.tinfo->mainbd = bd;
       rearrange_windows(bd, EINA_FALSE);
       break;

   }
}

static void
_e_mod_action_move_right(E_Object   *obj,
                         const char *params)
{
   E_Border *bd = e_border_focused_get();

   if (border_move_to_right(bd, 1))
       rearrange_windows(bd, EINA_FALSE);
}

static void
_e_mod_action_move_top(E_Object   *obj,
                       const char *params)
{
    E_Border *bd = e_border_focused_get();

    if (!bd)
        return;

    switch (layout_for_desk(bd->desk)) {
     case E_TILING_NONE:
     case E_TILING_INDIVIDUAL:
       break;
      case E_TILING_GRID:
        if (border_move_to_left(bd, tiling_g.config->grid_rows))
            rearrange_windows(bd, EINA_FALSE);
        break;
      case E_TILING_BIGMAIN:
        if (border_move_to_left(bd, 1))
            rearrange_windows(bd, EINA_FALSE);
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

   switch (layout_for_desk(bd->desk)) {
     case E_TILING_NONE:
     case E_TILING_INDIVIDUAL:
       break;
     case E_TILING_GRID:
      if (border_move_to_right(bd, tiling_g.config->grid_rows))
         rearrange_windows(bd, EINA_FALSE);
      break;
     case E_TILING_BIGMAIN:
      if (border_move_to_right(bd, 1))
         rearrange_windows(bd, EINA_FALSE);
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
        " g:%dx%d+%d+%d bdname:%s (%c)",
        bd, bd->client.icccm.title, bd->client.netwm.name,
        bd->changes.size, bd->changes.pos, bd->changes.border,
        bd->x, bd->y, bd->w, bd->h, bd->bordername,
        is_master ? 'M' : (is_slave ? 'S': 'N'));

    if (!bd->changes.size && !bd->changes.pos && !bd->changes.border
    && (is_master || is_slave)) {
        DBG("nothing to do");
        return;
    }

    if (!is_master && !is_slave) {
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
            /* Maximize */
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
            _G.tinfo->master_list = eina_list_append(_G.tinfo->master_list, bd);
        }
    } else {
        /* Move or Resize */
        DBG("move or resize");
        /* TODO */
    }

    return;

    /* If the border changes size, we maybe want to adjust the layout */
    if (bd->changes.size) {
        if (layout_for_desk(bd->desk) == E_TILING_BIGMAIN) {
            double x;

            /* Only the mainbd-window is resizable */
            if (bd != _G.tinfo->mainbd || _G.tinfo->mainbd_width == -1)
                goto next;
            /* Don't take the size of a maximized window */
            if (bd->maximized)
                goto next;
            /* If the difference is too small, do nothing */
            if (between(_G.tinfo->mainbd_width, (bd->w - 2), (bd->w + 2)))
                goto next;
#ifdef TILING_DEBUG
            printf("TILING_DEBUG: trying to change the tinfo->mainbd width"
                   "to %d (it should be: %d), big_perc atm is %f\n",
                   bd->w, _G.tinfo->mainbd_width, _G.tinfo->big_perc);
#endif
            /* x is the factor which is caused by shelves */
            x = _G.tinfo->mainbd_width / _G.tinfo->big_perc
                / bd->desk->zone->w;
#ifdef TILING_DEBUG
            printf("TILING_DEBUG: x = %f -> big_perc = %f\n",
                   x, bd->w / x / bd->desk->zone->w);
#endif
            _G.tinfo->big_perc = bd->w / x / bd->desk->zone->w;
        }
    }
next:
    rearrange_windows(bd, EINA_FALSE);
}

static Eina_Bool
_e_module_tiling_hide_hook(void *data,
                           int   type,
                           void *event)
{
    static Tiling_Info *_tinfo = NULL;
    E_Event_Border_Hide *ev = event;

    DBG("hide-hook\n");

    /*
    rearrange_windows(ev->border, EINA_TRUE);
    */

    if (_G.currently_switching_desktop)
        return EINA_TRUE;

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

    if (ti->mainbd == ev->border)
        ti->mainbd = get_first_window(NULL, ti->desk);

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
/* Exported functions {{{*/

EAPI void
e_mod_tiling_rearrange()
{
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
                    E_Border *first;

                    if ((first = get_first_window(NULL, desk)))
                        rearrange_windows(first, EINA_FALSE);
                }
            }
        }
    }
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

    return 1;
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

    /* Callback for new windows or changes */
    _G.hook = e_border_hook_add(E_BORDER_HOOK_EVAL_POST_FETCH,
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
