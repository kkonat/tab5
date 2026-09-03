idf_component_register(
    SRCS "src/ngl_draw.c" "src/ngl_text.c" "src/ngl_icon.c" "src/ngl_screen.c"
         "src/ngl_font_data.c" "src/ngl_icon_data.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_timer esp_driver_ppa
    PRIV_REQUIRES esp_mm
)

# ngl_surface_t is opaque to everyone else - see the note in ngl.h. PRIVATE so
# that nothing which merely REQUIRES ngl can accidentally reach the layout.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NGL_INTERNAL)
