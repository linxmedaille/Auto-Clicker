#!/bin/sh
# Regenerates vendor/fonts.css (base64-embedded @font-face rules) from vendor/fonts/*.woff2.
# The app has no network at runtime, so fonts travel inside the exe.
cd "$(dirname "$0")/../vendor" || exit 1
face() { # family style weight file
  printf '@font-face{font-family:"%s";font-style:%s;font-weight:%s;font-display:block;src:url(data:font/woff2;base64,%s) format("woff2");}\n' \
    "$1" "$2" "$3" "$(base64 -w0 "fonts/$4")"
}
{
  face "Instrument Serif" normal 400 instrument-serif-400-normal.woff2
  face "Instrument Serif" italic 400 instrument-serif-400-italic.woff2
  face "JetBrains Mono" normal 500 jetbrains-mono-500-normal.woff2
} > fonts.css
