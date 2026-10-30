# Gzip a web page into a C header:
#   cmake -DIN=index.html -DOUT=web_page.h -P embed.cmake
# The header defines `web_page`, gzip data to be served with
# Content-Encoding: gzip. After PicoMultiLoader's embed.cmake (the owner's,
# GPL-3.0; THIRD-PARTY.md).
set(gz "${OUT}.gz")
file(ARCHIVE_CREATE OUTPUT "${gz}" PATHS "${IN}" FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL 9)
file(READ "${gz}" hex HEX)
file(REMOVE "${gz}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "/* Generated from ${IN}: do not edit. */\n#ifndef WEB_PAGE_H\n#define WEB_PAGE_H\n\n#include <stdint.h>\n\nstatic const uint8_t web_page[] = {\n${bytes}\n};\n\n#endif\n")
