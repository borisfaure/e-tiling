#include "e.h"
#include "e_mod_main.h"
#include "e_mod_config.h"
#include "config.h"

/* Prototypes */
static void *_create_data(E_Config_Dialog *cfd);
static void  _free_data(E_Config_Dialog      *cfd,
                        E_Config_Dialog_Data *cfdata);
static Evas_Object *_basic_create_widgets(E_Config_Dialog      *cfd,
                                          Evas                 *evas,
                                          E_Config_Dialog_Data *cfdata);
static int _basic_apply_data(E_Config_Dialog      *cfd,
                             E_Config_Dialog_Data *cfdata);





/* HACK: Needed to get subobjs of the widget. Is there a better way? */
typedef struct _E_Widget_Smart_Data E_Widget_Smart_Data;

struct _E_Widget_Smart_Data
{
   Evas_Object *parent_obj;
   Evas_Coord   x, y, w, h;
   Evas_Coord   minw, minh;
   Eina_List   *subobjs;
};

static void
recursively_set_disabled(Evas_Object *obj,
                         int          disabled)
{
    E_Widget_Smart_Data *sd;

    if (!obj)
        return;

    sd = evas_object_smart_data_get(obj);
    if (!sd || (evas_object_type_get(obj)
                && strcmp(evas_object_type_get(obj), "e_widget")))
        return;

    for (Eina_List *l = sd->subobjs; l; l = l->next) {
        Evas_Object *o = l->data;
        if (!o) continue;
        recursively_set_disabled(o, disabled);
    }

    e_widget_disabled_set(obj, disabled);
}






/* Some defines to make coding with the e_widget_* easier for configuration panel */
#define RADIO(title, value, radiogroup) \
  e_widget_radio_add(evas, D_(title), value, radiogroup)
#define LIST_ADD(list, object) \
  e_widget_list_object_append(list, object, 1, 1, 0.5)

struct _Config_vdesk *
get_vdesk(Eina_List *vdesks,
          int x,
          int y,
          unsigned int zone_num)
{
    DBG("getting vdesk x %d / y %d / zone_num %d\n", x, y, zone_num);

    for (Eina_List *l = vdesks; l; l = l->next) {
        struct _Config_vdesk *vd = l->data;

        if (!vd)
            continue;

        if (vd->x == x && vd->y == y && vd->zone_num == zone_num)
            return vd;
    }

    return NULL;
}

E_Config_Dialog *
e_int_config_tiling_module(E_Container *con,
                           const char  *params)
{
    E_Config_Dialog *cfd;
    E_Config_Dialog_View *v;
    char buf[PATH_MAX];

    if (e_config_dialog_find("E", "windows/tiling"))
        return NULL;

    v = E_NEW(E_Config_Dialog_View, 1);

    v->create_cfdata = _create_data;
    v->free_cfdata = _free_data;
    v->basic.apply_cfdata = _basic_apply_data;
    v->basic.create_widgets = _basic_create_widgets;

    snprintf(buf, sizeof(buf), "%s/e-module-tiling.edj",
             e_module_dir_get(tiling_g.module));
    cfd = e_config_dialog_new(con,
                              D_("Tiling Configuration"),
                              "E", "windows/tiling",
                              buf, 0, v, NULL);
    return cfd;
}

/*
 * Fills the E_Config_Dialog-struct with the data currently in use
 *
 */
static void *
_create_data(E_Config_Dialog *cfd)
{
    E_Config_Dialog_Data *cfdata = E_NEW(E_Config_Dialog_Data, 1);

    /* Because we save a lot of lines here by using memcpy,
     * the structs have to be ordered the same */
    memcpy(cfdata, tiling_g.config, sizeof(Config));

    /* Handle things which can't be easily memcpy'd */
    cfdata->config.tiling_border = tiling_g.config->tiling_border
        ? strdup(tiling_g.config->tiling_border) : NULL;
    cfdata->config.floating_border = tiling_g.config->floating_border
        ? strdup(tiling_g.config->floating_border) : NULL;
    cfdata->config.vdesks = NULL;

    for (Eina_List *l = tiling_g.config->vdesks; l; l = l->next) {
        struct _Config_vdesk *vd = l->data;
        struct _Config_vdesk *newvd;

        if (!vd)
            continue;

        newvd = malloc(sizeof(struct _Config_vdesk));
        newvd->x = vd->x;
        newvd->y = vd->y;
        newvd->zone_num = vd->zone_num;
        newvd->layout = vd->layout;

        cfdata->config.vdesks = eina_list_append(cfdata->config.vdesks, newvd);
    }

    return cfdata;
}

static void
_free_data(E_Config_Dialog      *cfd,
           E_Config_Dialog_Data *cfdata)
{
    /*TODO: LEAK */
    free(cfdata);
}

static void
_fill_zone_config(E_Zone               *zone,
                  E_Config_Dialog_Data *cfdata)
{
    E_Radio_Group *rg;
    Evas *evas = cfdata->evas;
    Evas_Object *o;
    int mw, mh;

    /* Clear old entries first */
    evas_object_del(cfdata->o_desklist);
    cfdata->o_desklist = e_widget_list_add(evas, 1, 0);
    o = e_widget_scrollframe_object_get(cfdata->o_deskscroll);
    e_scrollframe_child_set(o, cfdata->o_desklist);
    e_widget_sub_object_add(cfdata->o_deskscroll, cfdata->o_desklist);

    for (int i = 0; i < zone->desk_y_count * zone->desk_x_count; i++) {
        E_Desk *desk = zone->desks[i];
        struct _Config_vdesk *vd;
        Evas_Object *radiolist;

        if (!desk)
            continue;

        vd = get_vdesk(cfdata->config.vdesks, desk->x, desk->y, zone->num);
        if (!vd) {
            vd = malloc(sizeof(struct _Config_vdesk));
            vd->x = desk->x;
            vd->y = desk->y;
            vd->zone_num = zone->num;
            vd->layout = E_TILING_NONE;

            cfdata->config.vdesks = eina_list_append(cfdata->config.vdesks, vd);
        }

        rg = e_widget_radio_group_new((int*)&vd->layout);
        radiolist = e_widget_list_add(evas, 0, 1);

        LIST_ADD(radiolist, e_widget_label_add(evas, desk->name));
        LIST_ADD(radiolist, RADIO("None", E_TILING_NONE, rg));
        LIST_ADD(radiolist, RADIO("Bigmain", E_TILING_BIGMAIN, rg));
        LIST_ADD(radiolist, RADIO("Grid", E_TILING_GRID, rg));
        LIST_ADD(cfdata->o_desklist, radiolist);
    }

    /* Get the correct sizes of desklist and scrollframe */
    e_widget_size_min_get(cfdata->o_desklist, &mw, &mh);
    evas_object_resize(cfdata->o_desklist, mw, mh);
    /*FIXME: what are those values?? */
    if (mh > 150)
        mh = 150;
    mw += 32;
    mh += 32;
    e_widget_size_min_set(cfdata->o_deskscroll, mw, mh);
}

static void
_cb_zone_change(void        *data,
                Evas_Object *obj)
{
    int n;
    E_Config_Dialog_Data *cfdata = data;
    E_Zone *zone;

    if (!cfdata)
        return;

    n = e_widget_ilist_selected_get(cfdata->o_zonelist);
    zone = e_widget_ilist_nth_data_get(cfdata->o_zonelist, n);
    if (!zone)
        return;
    _fill_zone_config(zone, cfdata);
}

static void
_cb_tiling_border_change(void        *data,
                         Evas_Object *obj)
{
    E_Config_Dialog_Data *cfdata = data;

    if (!cfdata)
        return;

    if (cfdata->config.tiling_border)
        free(cfdata->config.tiling_border);
    cfdata->config.tiling_border = strdup(e_widget_ilist_selected_label_get(obj));
}

static void
_cb_floating_border_change(void        *data,
                           Evas_Object *obj)
{
    E_Config_Dialog_Data *cfdata = data;

    if (!cfdata)
        return;

    if (cfdata->config.floating_border)
        free(cfdata->config.floating_border);
    cfdata->config.floating_border = strdup(e_widget_ilist_selected_label_get(obj));
}

static void
_cb_leave_space_change(void        *data,
                       Evas_Object *obj)
{
    E_Config_Dialog_Data *cfdata = data;

    if (!cfdata)
        return;

    recursively_set_disabled(cfdata->o_space_between,
                             !cfdata->config.space_between);
}

static Evas_Object *
_basic_create_widgets(E_Config_Dialog      *cfd,
                      Evas                 *evas,
                      E_Config_Dialog_Data *cfdata)
{
    Evas_Object *o, *ob, *of, *osf, *ossf, *ot;
    E_Radio_Group *rg;
    E_Container *con = e_container_current_get(e_manager_current_get());
    E_Zone *zone;
    int sel, c;

    o = e_widget_list_add(evas, 0, 0);
    ot = e_widget_table_add(evas, 0);

    /* General settings */
    of = e_widget_framelist_add(evas, D_("General"), 0);
    e_widget_framelist_object_append(of,
      e_widget_check_add(evas, D_("Don't change window borders"),
                         &cfdata->config.dont_touch_borders));
    e_widget_framelist_object_append(of,
      e_widget_check_add(evas, D_("Tile dialog windows aswell"),
                         &cfdata->config.tile_dialogs));
    e_widget_framelist_object_append(of,
      e_widget_check_add(evas, D_("Set too big windows floating automatically"),
                         &cfdata->config.float_too_big_windows));

    osf = e_widget_list_add(evas, 0, 0);
    ob = e_widget_check_add(evas, D_("Leave space between windows:"),
                            &cfdata->config.space_between);
    e_widget_on_change_hook_set(ob, _cb_leave_space_change, cfdata);
    e_widget_framelist_object_append(of, ob);

    ossf = e_widget_list_add(evas, 0, 1);
    LIST_ADD(ossf, e_widget_label_add(evas, D_("Horizontal:")));
    LIST_ADD(ossf, e_widget_slider_add(evas, 1, 0, D_("%1.0f px"), 0.0, 50.0,
                                       1.0, 0, NULL, &cfdata->config.between_x, 200));
    LIST_ADD(osf, ossf);
    ossf = e_widget_list_add(evas, 0, 1);
    LIST_ADD(ossf, e_widget_label_add(evas, D_("Vertical:")));
    LIST_ADD(ossf, e_widget_slider_add(evas, 1, 0, D_("%1.0f px"), 0.0, 50.0,
                                       1.0, 0, NULL, &cfdata->config.between_y, 200));
    LIST_ADD(osf, ossf);
    cfdata->o_space_between = osf;
    recursively_set_disabled(osf, !cfdata->config.space_between);
    e_widget_framelist_object_append(of, osf);
    e_widget_table_object_append(ot, of, 0, 0, 1, 2, 1, 1, 1, 1);


    /* Virtual desktop settings */
    of = e_widget_framelist_add(evas, D_("Virtual Desktops"), 0);
    rg = e_widget_radio_group_new((int *)&cfdata->config.tiling_mode);
    e_widget_framelist_object_append(of, RADIO("Don't tile by default", E_TILING_NONE, rg));
    e_widget_framelist_object_append(of, RADIO("Bigmain: Main window left, small windows right", E_TILING_BIGMAIN, rg));
    e_widget_framelist_object_append(of, RADIO("Grid: Distribute windows equally", E_TILING_GRID, rg));
    e_widget_framelist_object_append(of, RADIO("Individual modes:", E_TILING_INDIVIDUAL, rg));

    osf = e_widget_list_add(evas, 0, 1);

    /* Zone list */
    ob = e_widget_ilist_add(evas, 0, 0, NULL);
    e_widget_ilist_multi_select_set(ob, 0);
    e_widget_size_min_set(ob, 100, 100);
    cfdata->o_zonelist = ob;
    e_widget_on_change_hook_set(ob, _cb_zone_change, cfdata);
    for (Eina_List *l = con->zones; l; l = l->next) {
        if (!(zone = l->data))
            continue;
        e_widget_ilist_append(ob, NULL, zone->name, NULL, zone, NULL);
    }
    e_widget_ilist_selected_set(ob, 0);
    e_widget_ilist_go(ob);
    e_widget_ilist_thaw(ob);
    LIST_ADD(osf, ob);


    /* List of individual tiling modes */
    /* Order is important here: Firstly create the list, then add it to the
     * scrollframe before any objects get added to the list */
    cfdata->o_desklist = e_widget_list_add(evas, 1, 0);
    cfdata->o_deskscroll = e_widget_scrollframe_simple_add(evas, cfdata->o_desklist);
    cfdata->evas = evas;

    _fill_zone_config(con->zones->data, cfdata);

    LIST_ADD(osf, cfdata->o_deskscroll);

    e_widget_framelist_object_append(of, osf);
    e_widget_table_object_append(ot, of, 0, 2, 1, 1, 1, 1, 1, 1);


    /* Grid mode settings */
    of = e_widget_framelist_add(evas, D_("Grid mode settings"), 0);
    rg = e_widget_radio_group_new(&cfdata->config.grid_distribute_equally);
    e_widget_framelist_object_append(of, RADIO("Distribute space equally", 1, rg));
    e_widget_framelist_object_append(of, RADIO("Use this number of rows:", 0, rg));
    e_widget_framelist_object_append(of, e_widget_slider_add(evas, 1, 0, D_("%1.0f"), 1.0, 12.0, 1.0, 0, NULL, &cfdata->config.grid_rows, 100));
    e_widget_table_object_append(ot, of, 1, 0, 1, 1, 1, 1, 1, 1);


    /* Bigmain settings */
    of = e_widget_framelist_add(evas, D_("Bigmain settings"), 0);
    e_widget_framelist_object_append(of, e_widget_label_add(evas, D_("Big win takes percent of screen:")));
    e_widget_framelist_object_append(of, e_widget_slider_add(evas, 1, 0, D_("%.2f"), 0.1, 1.0, 0.01, 0, &cfdata->config.big_perc, NULL, 100));
    e_widget_table_object_append(ot, of, 1, 1, 1, 1, 1, 1, 1, 1);


    /* Tiling mode border style */
    osf = e_widget_list_add(evas, 0, 0);
    of = e_widget_framelist_add(evas, D_("Tiling border"), 0);
    ob = e_widget_ilist_add(evas, 0, 0, NULL);
    e_widget_ilist_multi_select_set(ob, 0);
    e_widget_size_min_set(ob, 100, 75);
    e_widget_on_change_hook_set(ob, _cb_tiling_border_change, cfdata);
    sel = -1;
    c = 0;
    for (Eina_List *l = e_theme_border_list(); l; l = l->next, c++) {
        e_widget_ilist_append(ob, NULL, l->data, NULL, NULL, NULL);
        if (cfdata->config.tiling_border
        && !strcmp(l->data, cfdata->config.tiling_border))
            sel = c;
    }
    if (sel != -1)
        e_widget_ilist_selected_set(ob, sel);
    e_widget_ilist_go(ob);
    e_widget_ilist_thaw(ob);
    e_widget_framelist_object_append(of, ob);
    LIST_ADD(osf, of);

    of = e_widget_framelist_add(evas, D_("Floating border"), 0);
    ob = e_widget_ilist_add(evas, 0, 0, NULL);
    e_widget_ilist_multi_select_set(ob, 0);
    e_widget_size_min_set(ob, 100, 75);
    e_widget_on_change_hook_set(ob, _cb_tiling_border_change, cfdata);
    e_widget_on_change_hook_set(ob, _cb_floating_border_change, cfdata);
    sel = -1;
    c = 0;
    for (Eina_List *l = e_theme_border_list(); l; l = l->next, c++) {
        e_widget_ilist_append(ob, NULL, l->data, NULL, NULL, NULL);
        if (cfdata->config.floating_border
        && !strcmp(l->data, cfdata->config.floating_border))
            sel = c;
    }
    if (sel != -1)
        e_widget_ilist_selected_set(ob, sel);
    e_widget_ilist_go(ob);
    e_widget_ilist_thaw(ob);
    e_widget_framelist_object_append(of, ob);
    LIST_ADD(osf, of);

    e_widget_table_object_append(ot, osf, 1, 2, 1, 1, 1, 1, 1, 1);
    LIST_ADD(o, ot);

    return o;
}

static int
_basic_apply_data(E_Config_Dialog      *cfd,
                  E_Config_Dialog_Data *cfdata)
{
    int need_rearrangement;

    need_rearrangement = memcmp(cfdata, tiling_g.config,
            sizeof(Config) - (sizeof(char *) * 2) - sizeof(Eina_List *));

    if (!need_rearrangement) {
        if (cfdata->config.tiling_border && tiling_g.config->tiling_border)
            need_rearrangement = strcmp(cfdata->config.tiling_border,
                                        tiling_g.config->tiling_border);
        else
        if (cfdata->config.tiling_border || tiling_g.config->tiling_border)
            need_rearrangement = 1;
    }

    if (!need_rearrangement) {
        if (cfdata->config.floating_border && tiling_g.config->floating_border)
            need_rearrangement = strcmp(cfdata->config.floating_border,
                                        tiling_g.config->floating_border);
        else
        if (cfdata->config.floating_border || tiling_g.config->floating_border)
            need_rearrangement = 1;
    }

    if (!need_rearrangement) {
        /* Check if the layout for one of the vdesks has changed */
        for (Eina_List *l = tiling_g.config->vdesks; l; l = l->next) {
            struct _Config_vdesk *vd = l->data,
                                 *newvd;

            if (!vd || !(newvd = get_vdesk(cfdata->config.vdesks,
                                           vd->x, vd->y, vd->zone_num)))
                continue;

            if (newvd->layout != vd->layout) {
                E_Zone *zone = e_zone_current_get(e_container_current_get(
                        e_manager_current_get()));
                E_Desk *desk = e_desk_current_get(zone);

                if (desk->x == vd->x && desk->y == vd->y
                && zone->num == vd->zone_num) {
                    need_rearrangement = 1;
                    break;
                }
            }
        }
    }

    if (tiling_g.config->floating_border)
        free(tiling_g.config->floating_border);
    if (tiling_g.config->tiling_border)
        free(tiling_g.config->tiling_border);
    memcpy(tiling_g.config, cfdata, sizeof(Config));

    cfdata->config.floating_border = NULL;
    cfdata->config.tiling_border = NULL;
    cfdata->config.vdesks = NULL;

    e_config_save_queue();

    return EINA_TRUE;
}
