#ifdef E_TYPEDEFS
#else
#ifndef E_MOD_CONFIG_H
#define E_MOD_CONFIG_H
#include "e_mod_main.h"

struct _E_Config_vdesk
{
   int x, y;
   int zone_num;
   int layout;
};

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
#endif
#endif
