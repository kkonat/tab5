idf_component_register(
    SRCS "nupogodi_app.c"
         "npg_asset.c"
         "npg_draw.c"
         "npg_panel.c"
         "npg_sound.c"
         "sm5a.c"
    INCLUDE_DIRS "."
    REQUIRES ngl neos_api)
