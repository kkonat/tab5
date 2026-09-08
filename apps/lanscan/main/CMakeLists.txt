idf_component_register(SRCS "lanscan_app.c"
                            "lan_engine.c"
                            "lan_model.c"
                            "lan_util.c"
                            "lan_tcp.c"
                            "lan_ports.c"
                            "lan_icmp.c"
                            "lan_dns.c"
                            "lan_dnsq.c"
                            "lan_nbt.c"
                            "lan_snmp.c"
                            "lan_mdns.c"
                            "lan_ssdp.c"
                            "lan_oui.c"
                            "lan_classify.c"
                            "lan_ui.c"
                       INCLUDE_DIRS "."
                       REQUIRES ngl neos_api)

#
# -Werror=double-promotion, for the reason the clock app gives: apps link
# -nostdlib with no libgcc, and the loader's table carries a handful of the
# double helpers out of the dozens that exist, so one stray promotion is an
# app that will not load rather than one that runs slowly.
#
# It costs nothing here. Nothing this program measures is a real number: an
# RTT is milliseconds, a TTL is a hop count, a port is a port, and the one
# place a float would be tempting - a percentage for the progress line - is
# two integers and a division.
#
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=double-promotion)
