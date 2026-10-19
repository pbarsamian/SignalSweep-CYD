# SPDX-License-Identifier: GPL-3.0-or-later
# The frozen contract, registered as an ESP-IDF component for the C5 build.
# (The CYD build compiles the same .c files through PlatformIO's build_src_filter;
# PlatformIO ignores this file.)
idf_component_register(
    SRCS "ss_capabilities.c" "ss_frame.c" "ss_cbor.c" "ss_token.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES mbedtls)
