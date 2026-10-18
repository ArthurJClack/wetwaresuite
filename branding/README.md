# branding

the wetwaresuite logo ("flow"): lowercase monoline letters standing on one rail that joins them all. one solid color, never a gradient.

| file | what it's for |
| --- | --- |
| `logo-on-dark.svg` | wordmark in mint, for dark backgrounds |
| `logo-on-light.svg` | wordmark in ink, for light backgrounds |
| `icon.svg` | app icon (the w on its rail), the source for every png |
| `png/icon-<size>.png` | 16 to 1024 px renders of `icon.svg` |
| `wetwaresuite.ico` | windows icon, built into every `.exe` through `wetwaresuite.rc.in` |
| `wetwaresuite.icns` | macOS icon, for `.app` bundles |
| `wetwaresuite.desktop` | linux start-menu entry, installed with `wetwaresuite-gui` |

the window icon is `png/icon-128.png`, embedded into the gui at build time (`gui/CMakeLists.txt`).

## colors

| name | hex | use |
| --- | --- | --- |
| mint | `#19e3b1` | the logo on dark, accents |
| ink | `#0e1116` | the logo on light, dark backgrounds |
| paper | `#f6f7f9` | light backgrounds |

## fonts

the logo letters are drawn by hand (plain svg paths), so they need no font. for docs, the website and anything else around the suite, use these (all SIL Open Font License, fine alongside GPL-3.0):

- headings: Space Grotesk
- body text: DM Sans
- code: JetBrains Mono

## changing the icon

edit `icon.svg`, then re-render the pngs and rebuild `wetwaresuite.ico` and `wetwaresuite.icns` from them (any svg renderer works; ImageMagick `convert png/icon-{16,32,48,64,128,256}.png wetwaresuite.ico` makes the ico).
