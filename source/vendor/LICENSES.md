# Third-party assets embedded in autoclicker.exe

| Asset | Source | License |
| --- | --- | --- |
| GSAP 3.13.0 (`gsap.min.js`) | https://cdnjs.cloudflare.com/ajax/libs/gsap/3.13.0/gsap.min.js | GSAP Standard "No Charge" License, https://gsap.com/standard-license |
| Instrument Serif 400 regular + italic (latin subset) | Fontsource, `@fontsource/instrument-serif@5` | SIL Open Font License 1.1 (Instrument / Rodrigo Fuenzalida, Jordan Egstad) |
| JetBrains Mono 500 (latin subset) | Fontsource, `@fontsource/jetbrains-mono@5` | SIL Open Font License 1.1 (JetBrains) |
| Solar icons: `play-bold`, `stop-bold`, `target-bold` | Iconify API, https://icon-sets.iconify.design/solar/ | CC BY 4.0, Solar Icons by 480 Design |

`fonts.css` is generated from `fonts/*.woff2` by `tools/make-fonts-css.sh`.
The icon SVG paths are inlined in `ui.html`.
