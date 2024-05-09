#include "djui.h"
#include "djui_panel.h"
#include "djui_panel_menu.h"
#include "djui_panel_changelog.h"
#include "pc/lua/utils/smlua_misc_utils.h"

static char sInfo[512];

void djui_panel_info_create(struct DjuiBase *caller) {
    struct DjuiThreePanel *panel = djui_panel_menu_create(DLANG(INFORMATION, INFORMATION_TITLE));
    struct DjuiBase *body = djui_three_panel_get_body(panel);
    {
        snprintf(sInfo, 512, "\
sm64coopdx is an online multiplayer project for the Super Mario 64 PC port, started by the Coop Deluxe Team.\n\
Its purpose is to actively maintain and improve, but also continue sm64ex-coop, created by djoslin0.\n\
More features, customizability, and power to the Lua API allow modders and players to enjoy Super Mario 64 more than ever!");

        struct DjuiText* text = djui_text_create(body, sInfo);
        djui_base_set_location(&text->base, 0, 0);
        djui_base_set_size(&text->base, (DJUI_DEFAULT_PANEL_WIDTH * (configDjuiThemeCenter ? DJUI_THEME_CENTERED_WIDTH : 1)) - 64, 220);
        djui_base_set_color(&text->base, 220, 220, 220, 255);
        djui_text_set_drop_shadow(text, 64, 64, 64, 100);
        djui_text_set_alignment(text, DJUI_HALIGN_CENTER, DJUI_VALIGN_CENTER);

        struct DjuiRect* rect1 = djui_rect_container_create(body, 64);
        {
            djui_button_left_create(&rect1->base, DLANG(MENU, BACK), DJUI_BUTTON_STYLE_BACK, djui_panel_menu_back);
            djui_button_right_create(&rect1->base, DLANG(INFORMATION, CHANGELOG), DJUI_BUTTON_STYLE_NORMAL, djui_panel_changelog_create);
        }
    }

    djui_panel_add(caller, panel, NULL);
}
