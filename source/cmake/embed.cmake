cmake_minimum_required(VERSION 3.21)

# Inlines the vendored assets into ui.html, then turns the result into a C
# header with a NUL-terminated byte array.
# Usage: cmake -DINPUT=ui.html -DFONTS=fonts.css -DGSAP=gsap.min.js -DOUTPUT=ui_html.h -P embed.cmake
# A byte array is used instead of a string literal because MSVC caps string
# literals at 64 KB.

file(READ "${INPUT}" html)
file(READ "${FONTS}" fonts)
file(READ "${GSAP}" gsap)

string(REPLACE "/*@FONTS@*/" "${fonts}" html "${html}")
string(REPLACE "/*@GSAP@*/" "${gsap}" html "${html}")

string(HEX "${html}" hex)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,){24})" "\\1\n    " bytes "${bytes}")

file(WRITE "${OUTPUT}"
"/* Generated from ui.html + vendor/ by cmake/embed.cmake. Do not edit. */\n"
"#ifndef UI_HTML_H\n"
"#define UI_HTML_H\n\n"
"static const unsigned char ui_html[] = {\n"
"    ${bytes}0x00\n"
"};\n\n"
"#endif\n")
