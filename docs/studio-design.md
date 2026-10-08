# Krita Studio: a first-principles redesign

This document is the reasoning behind the Studio layout of this fork. Every
surface of Krita was inventoried from the source (menus, tools, dockers, blend
modes, filters, brush engines, settings), each item was questioned, and the
decisions below follow from a small set of principles instead of from what
other painting apps do.

Status markers: **[done]** is implemented on the `modern-style` branch,
**[next]** is designed and scheduled, **[later]** is designed but further out.

---

## 1. What was wrong

Numbers from the source, not impressions:

| Surface | Count | Problem |
|---|---|---|
| Menu bar | 11 menus, **266 commands** | Organized by object type (Image, Layer, Select, Filter). Several entries appear twice (Scale Image, Resize Canvas, Scale Layers, Selection Scale). Finding anything means knowing Krita's taxonomy. |
| Toolbox | **37 tools**, always visible | 10 drawing tools, 8 selection tools, 5 vector-only tools shown while painting raster, 2 navigation tools that are better as gestures. |
| Dockers | **32 C++ + 8 Python** | 6 different color selectors and mixers, two histories, a separate brush history next to the brush presets. |
| Blend modes | **138 in 13 categories** | Grouped by math (HSY, HSI, HSL, HSV repeat the same 8 operations = 33 entries; 10 binary logic ops; 6 modulo ops). The useful ~25 are scattered among them. |
| Filters | **52 in 9 categories** | Categories like "Map" and "Other" say nothing; 6 entries are variants of Emboss; destructive filters, filter layers and filter masks are three separate places for the same thing. |
| Brush sliders | linear or cubic | Size is perceived as ratios; a linear slider spends most of its length on sizes nobody distinguishes. |
| Colors | tinted grays, mid-gray canvas surround | UI chroma and a bright surround bias the colors and values the artist is judging. |

## 2. Principles

1. **The canvas is the product.** Chrome is a cost. By default it should take
   about 10 % of the screen, and everything else should appear when and where
   it is needed.
2. **Nothing is searched for, it is found.** One "Find anything" (Ctrl+K)
   reaches every command, tool, effect, brush, blend mode, layer, panel,
   workspace and settings page, ranked by match and by your own use.
3. **Organize by intent, not by implementation.** "Make this darker", not
   "HSY category"; "Blur", not "Filter → Blur → Gaussian Blur…".
4. **Show the 20 % that is used 80 % of the time; keep the rest one step
   away, never gone.** Escape hatches exist for every hidden thing.
5. **Context over chrome.** Options appear for the tool or object they apply
   to (the toolbar changes with the tool; selection actions live next to the
   selection; layer settings live in the layer row).
6. **Preview before commit.** Hovering a blend mode shows it on the layer;
   Escape returns. No dialog should be needed to see what something does.
7. **Perceptual controls.** Sliders map to how the quantity is perceived:
   brush size is logarithmic; opacity stays linear because it is blended in
   gamma-encoded space where alpha steps already look even.
8. **A quiet, achromatic UI.** Neutral surfaces with zero chroma, hierarchy by
   lightness steps that are even in OKLab, one accent hue, minimal text.
9. **Stable positions, adaptive suggestions.** Primary controls never move
   (muscle memory); only explicitly "suggested" areas (Find, Recent) adapt.
10. **Customization at zero cost.** Customizing uses the same direct
    manipulation as using (drag a rail button, right-click to add a panel);
    every customization has a reset.
11. **Keyboard is first class, pointer is complete.** Every action has a
    pointer path; frequent ones have a key; Find teaches the keys.
12. **Remember intent.** Each tool slot remembers its variant; Paint and Blend
    remember their brushes; the brush in use decides which slot is lit.

## 3. Visual language [done]

All colors are defined in OKLCH and converted to sRGB
(`krita/data/themes/KritaStudioDark.colors`, `KritaStudioLight.colors`).

| Token | Dark | OKLCH | Use |
|---|---|---|---|
| chrome | `#191919` | L 0.215 | rails, top bar: the frame of the window |
| surface | `#222222` | L 0.250 | panels |
| raised | `#2b2b2b` | L 0.290 | fields, buttons |
| hover | `#343434` | L 0.325 | hover |
| canvas surround | `#383838` | L 0.340 | area around the image |
| text | `#ebebeb` | L 0.940 | 13.3 : 1 on surface |
| text secondary | `#ababab` | L 0.740 | 6.9 : 1 |
| text hint | `#838383` | L 0.610 | 4.2 : 1 |
| selection | `#3a69ad` | L 0.52 C 0.12 h 258 | fills; white text on it is 5.3 : 1 |
| accent | `#74a6ef` | L 0.72 C 0.12 h 258 | focus rings, links, active marks |

Decisions and why:

- **Zero chroma neutrals.** Any tint in the UI shifts the perceived hue of the
  painting next to it (simultaneous contrast). Krita's old themes had warm
  grays.
- **Even OKLab steps of ~0.035–0.04.** OKLab L is perceptually uniform, so
  surface levels read as evenly spaced; sRGB steps would not.
- **Not near-black.** The first palette (L 0.165–0.235) was rejected: black
  chrome next to a white canvas maximizes glare and makes the image look
  washed out. The ramp was lifted by 0.05.
- **Canvas surround follows the theme** (`KisConfig::canvasBorderColor`), one
  step lighter than the panels in dark themes: neutral, low glare. The old
  default was mid-gray 128, a bright frame next to dark chrome.
- **One hue.** Blue (h 258) is far from skin tones and reads as "interactive".
  Selection uses a muted version so white text stays readable; focus uses the
  bright one.
- **Surfaces, not outlines.** Buttons and fields are filled one step above the
  panel and have no border until hovered or focused (`KisModernStyle`). Group
  boxes lost their frames: spacing and the title group the content.
- **Selection is a tint**, not a solid block, in lists and menus.
- **One icon language for the rails**: 24 px grid, 1.6 px round strokes,
  dark and light variants (`krita/pics/svg/*_studio_*.svg`). Krita's own icons
  mix styles and weights.

## 4. Layout [done, refined in next phases]

```
┌ ☰ │ contextual tool options …………………………… │ Find anything   Ctrl+K ┐
├────┬──────────────────────────────────────────────┬───────────┬────┤
│Pnt │                                              │  pinned   │Lay │
│Bld │                                              │  panel    │Brs │
│Ers │                 CANVAS                       │  (opt.)   │Col │
│ ·  │                                              │           │Opt │
│Sel │                                    ┌flyout┐  │           │Pal │
│Mov │                                    │      │  │           │His │
│Fil │                                    └──────┘  │           │    │
│Shp │                                              │           │ ⋮  │
│Txt │                                              │           │    │
│Gde │                                              │           │    │
│size│                                              │           │    │
│opac│                                    zoom·rot  │           │    │
└────┴──────────────────────────────────────────────┴───────────┴────┘
```

- **No menu bar** while a document is open. The ☰ menu holds only document
  and app commands; the complete classic menus remain under ☰ → All Menus.
- **Top bar = the active tool's options**, nothing else (see 6).
- **Left rail = tools + brush size and opacity.** Right rail = panels.
- **Panels open as flyouts over the canvas edge** and close on any click
  elsewhere or Escape; pin to dock them.
- **No tab row, no status bar, no scroll bars** [done]. Documents are
  frameless maximized windows; the document button next to Find names the
  current one and switches between them. Zoom, rotation (only while rotated),
  mirror, a value check and progress sit in a pill at the canvas corner
  (`KisStudioCanvasHud`); color space, profile, size and memory belong to the
  document properties, not to every minute of painting.
- **Value check**: a soft proof into gray, so lightness is computed by the
  color management, without touching the image or its proofing settings.
- Earlier plan for the tab bar, kept for the record: one document = no tab bar. QTabBar::autoHide is not enough:
  QMdiArea keeps the tab bar's height as a viewport margin, and overriding its
  resize handling to drop the margin recursed (the viewport resize re-enters
  it). The plan is a frameless, maximized sub-window for a single document and
  tabs from the second one on.
- **The classic layout is one toggle away**: View → Studio Layout.

## 5. Tools: 37 → 9 slots [done]

Slots are grouped by what you want to do. Each slot remembers its last
variant; clicking the active slot again, holding it, or right-clicking it
shows its variants.

| Slot | Variants (first = default) | Notes |
|---|---|---|
| **Paint** | Freehand Brush, Dynamic Brush, Multibrush, Smart Patch | Dynamic and Multibrush are ways to paint, not separate jobs. Smart Patch is "paint to repair". |
| **Blend** | Freehand Brush with its own remembered brush | Smudging is one of the three things painters do with a brush; first use picks a bundled smudge/blend preset. |
| **Erase** | Freehand Brush in eraser mode | Krita's E toggle, made visible. Keeps Krita's separate eraser size. |
| **Select** | Rectangle, Ellipse, Freehand (lasso), Polygon, Contiguous (magic wand), Similar Color, Magnetic, Bezier | Bezier selection is last: rarely used, still there. |
| **Move** | Move, Transform, Crop, Select Shapes | Crop is a transform of the canvas; Select Shapes is how vector objects are moved. |
| **Fill** | Fill, Gradient, Enclose and Fill, Colorize Mask | All four answer "fill this area". |
| **Shape** | Rectangle, Ellipse, Line, Polygon, Polyline, Bezier Curve, Freehand Path, Edit Shapes, Calligraphy, Comic Panel Knife | The bezier and path tools live here instead of on screen. |
| **Text** | Text | |
| **Guides** | Assistants, Reference Images, Measure | Setup tools, used before painting rather than during. |

No slot (still in Find, still on their keys):

| Tool | Why |
|---|---|
| Zoom, Pan | Gestures, wheel and Space are faster; a mode switch is a detour. |
| Color Sampler | A momentary action: Ctrl+click while painting. A tool slot would cost two switches per sample. |

Interaction details:

- **The brush decides the slot.** If the active brush is the one Blend
  remembers, Blend is lit; otherwise Paint. After a restart the highlighted
  slot always matches the brush in use.
- **Leaving Paint or Blend remembers its brush**, so Paint → Blend → Paint
  returns to the exact brush.
- Variant menus show each tool's shortcut, which teaches it.

## 6. The top bar is the tool's context [done]

Before: gradients, patterns, colors, brush preset, brush editor, blend mode,
two sliders, mirror, window layouts and workspaces, always, for every tool.

After, per tool:

| Entry | Shown for |
|---|---|
| Brush preset, brush editor, brush blend mode, mirror painting | tools that paint with a brush: Paint/Blend/Erase variants, Line, the shape tools |
| Gradients | the Gradient tool |
| Patterns | Fill, Enclose and Fill, Colorize Mask, the shape tools |
| Foreground/background colors | everything that applies color |
| Size and opacity sliders | never: they are on the left rail |
| **Tool chip** (icon, name, chevron) | always, first in the bar |
| Workspaces, window layouts | never: set up once, they are in Find |
| New/Open/Save, Undo/Redo toolbars | never: ☰ and Ctrl+Z |

**The tool chip** says which tool the bar is about: the rail shows the slot
(Select), the chip the variant (Contiguous Selection). Clicking it opens that
tool's complete options right below it, sized to their contents. Before, the
options lived in a panel at the far right edge, the farthest point from the
tool that needs them, and Select, Move and Guides had an empty bar. Tool
Options is no longer on the rail by default for the same reason.

Self-critique: the chip opens the whole options form. Select would still
profit from inline mode buttons (replace/add/subtract), but Shift and Alt
already do that while dragging, so the form is enough for now.

## 7. Find anything [done]

Ctrl+K, the field at the right of the top bar, or Ctrl+Enter (the old
"Search Actions").

- **Indexes**: tools, every command with its classic menu path as context,
  52 effects with their category, all brush presets with their thumbnails,
  138 blend modes, panels, the layers of the image, 16 settings pages
  (each opening the right tab), workspaces.
- **Ranking**: predictable matches first, in this order: the start of the
  name, the start of a word, the initials ("gb" → Gaussian Blur), all words
  as prefixes ("gaus bl"), a substring inside a word (3+ letters only; "gb"
  inside "RGBA" is an accident). Fuzzy, typo-tolerant matches only when there
  are fewer than five of those and the query has 3+ letters. On top: a head
  start for the kinds usually searched (tools, effects, commands) and
  frecency, `6·log2(1+uses) + 14·e^(−days/3)`. Blend modes get a small
  penalty so 138 of them don't drown other results.
- **Aliases: the user's vocabulary, not only Krita's.** "lasso", "magic
  wand", "eyedropper", "paint bucket", "free transform", "curves",
  "hue/saturation", "healing brush", "smudge" find Freehand Selection,
  Contiguous Selection, Color Sampler, Fill, Transform, Color Adjustment
  Curves, HSV Adjustment, Smart Patch and the blending brushes. The result
  shows Krita's name, so the alias also teaches it.
- **Nothing found says so** instead of showing an empty box.
- **Empty query shows Recent**, so Find is also "do that again".

## 8. Blend modes: 138 → 27 by intent [done]

The picker (`KisBlendModePicker`) replaces the combo box list everywhere
except layer styles:

| Intent | Modes |
|---|---|
| Normal | Normal |
| Darken | Multiply, Color Burn, Linear Burn, Darken, Darker Color |
| Lighten | Screen, Linear Dodge (Add), Color Dodge, Lighten, Lighter Color |
| Contrast | Overlay, Soft Light, Hard Light, Vivid Light, Linear Light, Pin Light, Hard Mix |
| Compare | Difference, Exclusion, Subtract, Divide |
| Color | Hue, Saturation, Color, Luminosity |
| Transparency | Erase, Behind, Dissolve |

- Every mode shows a **swatch made with its real composite op**: a warm color
  over a gray ramp (lightness behavior) and a hue ramp (color behavior),
  drawn over a checkerboard. For the transparency modes the backdrop fades
  out to the right and Dissolve is drawn at 50 %, otherwise Erase would show
  a blank, Behind nothing and Dissolve plain Normal.
- Names drop the variant qualifier where only one variant is shown
  ("Soft Light", not "Soft Light (Photoshop)"); search and "All modes" keep
  full names.
- **Typing searches all 138**; "All 138 modes" shows the rest by category.
- **Live preview on the canvas** for the layer blend mode: hovering or arrow
  keys apply the mode to the layer without undo entries; Escape restores it;
  picking is one undo step.
  The *brush* blend mode cannot be previewed on the canvas (it affects strokes
  not yet painted), so it shows swatches only. This was a mistake in the first
  mockup, caught in review.
- **Cycling (Alt+Shift+= / Alt+Shift+−, or the wheel over the box) steps
  through the 27 only**, in the order Darken → Lighten → Contrast → Compare →
  Color. Stepping through 138 mostly meant stepping through modes nobody uses.

Excluded from the essentials, and why:

| Excluded | Why |
|---|---|
| HSI/HSL/HSV versions of Hue/Saturation/Color/Lightness (24) | Same intent as the HSY ones, which match perceived lightness best. |
| Binary (XOR, NAND, …), Modulo (6) | Bit tricks and wrap-arounds, not painting operations. |
| IFS Illusions, P-Norm, Penumbra, Quadratic family | Niche variations of light/dark modes already covered. |
| HDR variants, Copy channel, normal map ops | Technical; found by search when needed. |

## 9. Filters become Effects [next]

The Filter menu sorted 52 filters into 9 implementation categories. Effects
are grouped by intent, browsed as live thumbnails of the current layer,
edited inline, and kept editable by default (a filter mask; "Bake" makes it
destructive). Visible by default:

| Intent | Shown | One step away ("More") |
|---|---|---|
| Adjust | Levels, Curves, HSV/HSL, Color Balance, Desaturate, Invert, Threshold, Posterize, Gradient Map, Color to Alpha, Auto Contrast | Cross-channel Curves, ASC-CDL, Dodge, Burn, Normalize, Maximize/Minimize Channel |
| Blur | Gaussian, Lens, Motion | Box Blur |
| Sharpen | Unsharp Mask | Sharpen, Gaussian High Pass, Mean Removal |
| Noise | Add Noise, Reduce Noise | Wavelet Noise Reducer, Random Pick |
| Stylize | Pixelize, Halftone, Oil Paint, Edge Detection | Emboss (the 6 variants become one with a direction option), Index Colors, Palettize, Raindrops, Phong Bumpmap, Height to Normal Map, Color Transfer, Propagate Colors, Round Corners, Reset Transparent, Fast Color Overlay |
| Distort | Wave | Small Tiles |
| Generate | Solid Color, Gradient, Pattern, Noise, Screentone | Multigrid, SeExpr |

Gaussian Blur is shown, Phong Bumpmap is not: frequency of use for painting,
not completeness, decides what is on screen. G'MIC appears as one entry
("G'MIC, 500+ filters").

## 10. Panels: 32 dockers [partly done]

On the rail by default: Layers, Brushes, Color, Palette, History (Tool
Options moved to the tool chip, section 6). Everything else is in the rail's "All panels" menu and in Find; the
rail is customized by right-click and drag. **[done]**

Consolidation **[next/later]**:

| Today | Becomes | Why |
|---|---|---|
| Advanced, Small, Specific, Wide Gamut and Artistic color selectors, Digital Mixer | **[done: `KisStudioColorPanel`, replaces the Advanced Color Selector on the rail]** One Color panel with an OKLCH picker (perceptually uniform lightness, chroma and hue), a recent-colors strip and the palette | Six ways to do one job. HSV's "value" is not perceived lightness; OKLCH's L is. |
| Brush Presets + Brush Preset History | Brushes, with a Recent row and stroke previews grouped by medium | History is just a view of the same presets. |
| Undo History + Snapshots | History | Both answer "go back to an earlier state". |
| Channels, Compositions | Inside Layers | They are views of the layer stack. |
| Overview, Grid and Guides | The canvas HUD | View state belongs on the view. |
| Animation Timeline, Onion Skins, Curves | Appear when the document is animated | Irrelevant to still images. |
| Arrange, Symbol Libraries, Text Properties | Appear with vector/text tools | Contextual. |
| Recorder, Task Sets, LUT, Log Viewer, Touch Docker, On-Canvas Brush Editor, Storyboard | "All panels" | Specialist tools. |

## 11. Layers panel [done, first pass]

Rows mark only what is unusual (hidden, locked, alpha locked, "Multiply 60%");
every mark shows on hover or selection. Blend mode and opacity sit under the
list as properties of the selected layer, raise/lower/properties buttons are
gone (drag, double click), sliders everywhere fill neutral instead of
selection blue. Still to do: inline editing in the selected row.


- Rows: thumbnail, name, a one-line summary only when not default
  ("Multiply · 60 %", "Alpha locked"), visibility.
- Blend mode and opacity move from the panel header into the selected row
  (they are properties of one layer, not of the panel).
- Color labels as a thin bar, not a full-row tint.
- Bottom: "+ Layer" (paint layer, the other 13 types in its menu), mask,
  alpha lock, duplicate, delete. Properties via double-click.

## 12. Menus: where the 266 commands went

| Classic menu | Where it lives now |
|---|---|
| File | ☰: New, Open, Recent, Save, Save As, Export. Sessions, templates, incremental saves, animation import/render, document info: Find. |
| Edit | Keys (undo, cut, copy, paste) and the selection bar for fill/stroke/clear; special pastes in Find. |
| View | Canvas HUD [next] for zoom, rotation, mirror, grid/guides/snapping; display modes in Find. |
| Image | ☰ → Image: size, canvas, trim, rotate, flip, color space, background, properties. Split, wavelet decompose, separate: Find. Duplicates removed. |
| Layer | Layers panel and its context menu; everything also in Find. Duplicates removed. |
| Select | The selection action bar next to the selection [next]; Select All/Deselect/Invert on their keys. |
| Filter | Effects (section 9). |
| Tools | Scripts: Find. |
| Settings | ☰ → Settings; each settings page is findable by what it is about ("autosave", "pressure"). Docker and toolbar management is direct manipulation on the rails. |
| Window, Help | ☰ → Help; windows and workspaces in Find. |

## 13. Perception and prediction

- **Brush size slider: logarithmic.** Weber–Fechner: 2→4 px and 200→400 px are
  the same perceived step. [done]
- **Opacity slider: linear**, deliberately (see principle 7). [done]
- **Zoom steps: geometric** (Krita already does this).
- **Value check**: one HUD toggle that shows the canvas in grayscale (OKLab L),
  the check painters make constantly. [next]
- **Predicted next actions** [later]: after a paste, the transform handles are
  active; after a selection, the selection bar shows the likely actions
  (fill, clear, transform, new layer from selection); Find ranks by your
  history; the empty Find shows what you did recently.

## 14. Settings [later]

Ten pages organized by implementation (General has eight tabs mixing file
handling, cursor and touch painting). Reorganize by question the user has
("my pen feels wrong", "Krita is slow", "colors look different"), make every
setting searchable from Find, and show the 15 most-changed settings first.

---

### Implementation map

| Area | Files |
|---|---|
| Theme | `krita/data/themes/KritaStudio*.colors`, `krita/data/kritarc`, `libs/ui/kis_config.cc` (canvas surround) |
| Widget style | `libs/widgetutils/KisModernStyle.*` |
| Layout shell, rails, top bar, ☰ | `libs/ui/KisStudioLayout.*` |
| Tool rail, color chip, size dot / opacity tile | `libs/ui/KisStudioToolRail.*`, `KisStudioLayout::createBrushRail()`, `KisStudioBrushSlider` |
| Inline tool options (transform modes, selection modes, gradient shapes, fill) | `libs/ui/KisStudioToolStrip.*` |
| Canvas HUD | `libs/ui/KisStudioCanvasHud.*` |
| Color panel (OKLCH) | `libs/ui/KisStudioColorPanel.*` |
| Brush sliders | `libs/ui/KisStudioBrushSlider.*`, `KisPaintopBox::brushSlider()` |
| Find anything | `libs/ui/KisStudioFind.*` |
| Blend modes | `libs/ui/widgets/KisBlendModePicker.*`, `kis_cmb_composite.*`, `LayerBox::slotPreviewBlendMode()` |
| Popups (Find, blend picker) | `libs/ui/widgets/KisPopupCloser.*`: frameless tool windows that close like popups (Qt::Popup can't be translucent on Windows) |
| UI testing | `KRITA_DEV_PROFILE` builds render their own windows to a PNG on a posted `KritaDevCapture` message (`krita/main.cc`), so tests never read the screen |
| Single-document tabs | `KisMdiArea` in `libs/ui/KisMainWindow.cpp` |
| Icons | `krita/pics/svg/*_studio_*.svg`, `*_hamburger_menu.svg` |

## 15. Base systems [done, first pass]

Everything above builds on three systems instead of one-off widgets:

- **Items.** Every tool, effect, command, brush, blend mode and panel Find
  knows is an item with a key ("tool:…", "effect:…"). Items can be dragged
  out of Find or a card and dropped on the tools panel.
- **The tools panel is a list of items.** It floats over the canvas, as tall
  as its contents, and may be empty. Drag along it to move, off it to
  remove; the "+" that appears on hover opens Find as a chooser that lists
  every tool and effect with nothing typed (touch: no keyboard needed).
  Scrolling over a slot steps through its variants.
- **The card.** Right click or long press any button: what it is, its
  shortcut as a key cap that records a new one when clicked (saved like the
  shortcuts dialog, taken from whatever had it), its variants (drag one out
  to make it a button, right click to hide it), pin/remove. Badges show
  every button's shortcut.
- **Effects on the canvas.** Any effect, from Find, the panel, a shortcut or
  the old menus, previews live on the whole layer with its settings in a
  small panel at the bottom; Enter/✓ keeps, Esc/✕ drops, the mask button
  keeps it editable (`KisFilterManager::setInlineEditor`).
- **The top bar is the title bar** (Windows): no system caption, empty top
  bar drags the window, buttons at its end (`KisStudioWindowFrame`,
  `StudioCustomTitleBar=false` turns it off).
