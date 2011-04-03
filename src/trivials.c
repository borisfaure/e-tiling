#include "e.h"
#include "trivials.h"
#include <stdarg.h>

#define TILING_DEBUG


void
move_resize(E_Border *bd,
            int       x,
            int       y,
            int       w,
            int       h)
{
    e_border_move_resize(bd,
                         (bd->zone ? bd->zone->x : 0) + x,
                         (bd->zone ? bd->zone->y : 0) + y,
                         w, h);
}

void
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

/* I wonder why noone has implemented the following one yet? */
E_Desk *
get_current_desk()
{
    E_Manager *m = e_manager_current_get();
    E_Container *c = e_container_current_get(m);
    E_Zone *z = e_zone_current_get(c);
    return e_desk_current_get(z);
}

/* Returns 1 if the given shelf is visible on the given desk */
int
shelf_show_on_desk(E_Shelf *sh,
                   E_Desk  *desk)
{
    E_Config_Shelf *cf;

    if (!sh || !desk)
        return 0;

    cf = sh->cfg;
    if (!cf)
        return 0;

    if (!cf->desk_show_mode)
        return 1;

    for (Eina_List *l = cf->desk_list; l; l = l->next) {
        E_Config_Shelf_Desk *sd = l->data;

        if (sd && sd->x == desk->x && sd->y == desk->y)
            return 1;
    }
    return 0;
}
