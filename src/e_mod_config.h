#ifdef E_TYPEDEFS
#else
#ifndef E_MOD_CONFIG_H
#define E_MOD_CONFIG_H
#include "e_mod_main.h"

struct _E_Config_Dialog_Data
{
   struct _Config config;
   Evas_Object *o_zonelist;
   Evas_Object *o_desklist;
   Evas_Object *o_deskscroll;
   Evas_Object *o_space_between;
   Evas        *evas;
};

E_Config_Dialog *e_int_config_tiling_module(E_Container *con,
                                            const char  *params);

struct _Config_vdesk *
get_vdesk(Eina_List *vdesks,
          int x,
          int y,
          int zone_num);


#endif
#endif
