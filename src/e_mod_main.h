#ifndef E_MOD_MAIN_H
#define E_MOD_MAIN_H
#include <libintl.h>
#define D_(str) dgettext(PACKAGE, str)

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

#define TILING_MAX_COLUMNS 8

struct _Config_vdesk
{
   int           x, y;
   unsigned int  zone_num;
   int           nb_cols;
};

struct _Config
{
    int            tile_dialogs;
    int            float_too_big_windows;
    Eina_List     *vdesks;
};

struct _Tiling_Info
{
    /* The desk for which this _Tiling_Info is used. Needed because
     * (for example) on e restart all desks are shown on all zones but no
     * change events are triggered */
    const E_Desk    *desk;

    struct _Config_vdesk *conf;

    /* List of windows which were toggled floating */
    Eina_List *floating_windows;

    Eina_List *columns[TILING_MAX_COLUMNS];
    int              x[TILING_MAX_COLUMNS];
    int              w[TILING_MAX_COLUMNS];

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
