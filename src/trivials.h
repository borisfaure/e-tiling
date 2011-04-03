#ifndef TRIVIALS_H
#define TRIVIALS_H
#include "e.h"
#include <stdarg.h>

void
move_resize(E_Border *bd,
            int       x,
            int       y,
            int       w,
            int       h);

void
recursively_set_disabled(Evas_Object *obj,
                         int          disabled);

int
between(int value,
        int minimum,
        int maximum);

E_Desk *
get_current_desk(void);

int
shelf_show_on_desk(E_Shelf *sh,
                   E_Desk   *desk);

/* HACK: Needed to get subobjs of the widget. Is there a better way? */
typedef struct _E_Widget_Smart_Data E_Widget_Smart_Data;

struct _E_Widget_Smart_Data
{
   Evas_Object *parent_obj;
   Evas_Coord   x, y, w, h;
   Evas_Coord   minw, minh;
   Eina_List   *subobjs;
};

#endif
