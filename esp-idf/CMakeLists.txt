set(LXMF_SRCS
    "src/lxmf.cpp"
    # LCD slice — per straddle plan this lives in esp-idf/lcd/ and is
    # gated by the spangap/spangap-lcd activator at build time. Until
    # the CLI implements activator-driven source-list exclusion, the
    # file's body is wrapped in #if CONFIG_SPANGAP_LCD so it compiles
    # to nothing in non-LCD builds.
    "lcd/src/lxmf_lcd.cpp"
)

idf_component_register(
    SRCS ${LXMF_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        spangap-core
        spangap-lcd
        reticulous-core
        json
)
