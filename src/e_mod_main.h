#ifndef E_MOD_MAIN_H
#define E_MOD_MAIN_H
#include <libintl.h>
#define D_(str) dgettext(PACKAGE, str)

typedef enum _E_Tiling_Conf
{
    E_TILING_INDIVIDUAL,
    E_TILING_GLOBAL,
} E_Tiling_Type;

typedef struct _Config      Config;
typedef struct _Tiling_Info Tiling_Info;

struct tiling_g
{
   E_Module *module;
   Config   *config;
   int       log_domain;
};
extern struct tiling_g tiling_g;

#define ERR(...) EINA_LOG_DOM_ERR(tiling_g.log_domain, __VA_ARGS__)
#define DBG(...) EINA_LOG_DOM_DBG(tiling_g.log_domain, __VA_ARGS__)


struct _Config_vdesk
{
   int           x, y;
   unsigned int  zone_num;
   int           nb_cols;
};

struct _Config
{
    E_Tiling_Type  tiling_mode;
    int            tile_dialogs;
    int            float_too_big_windows;
    int            nb_cols;
    Eina_List     *vdesks;
};

struct _Tiling_Info
{
    /* The desk for which this _Tiling_Info is used. Needed because
     * (for example) on e restart all desks are shown on all zones but no
     * change events are triggered */
    const E_Desk    *desk;

    /* List of windows which were toggled floating */
    Eina_List *floating_windows;

    /* List of windows in our own sorting: slave */
    Eina_List *slave_list;
    /* List of windows in our own sorting: master */
    Eina_List *master_list;

    int        slaves_count;

    /* big_perc (percentage of the screen which the mainbd-border will get)
     * has to be stored individually for each desk, the one in Tiling_Config
     * is only the default */
    double     big_perc;

    int nb_cols;

    /* When sending a border to another desktop, it has to be updated as soon
     * as the user switches to it. This is stored in the following flag. */
    int        need_rearrange;
};

EAPI extern E_Module_Api e_modapi;

EAPI void *e_modapi_init(E_Module *m);
EAPI int   e_modapi_shutdown(E_Module *m);
EAPI int   e_modapi_save(E_Module *m);

EAPI void e_mod_tiling_rearrange(void);


#endif
