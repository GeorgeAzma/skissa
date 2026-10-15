# Skissa: how we build it

This is the working guide for this fork. Read it before changing anything; it explains what we value, how the main systems fit together and how to work with the owner. **It is a living document: when you learn something that a future agent needs (a principle the owner states, a mistake worth not repeating, a system you added), add it here in the right section, briefly, and remove what has become false.**

## What this is

Skissa (Swedish: "to sketch") is a personal fork of [Krita](https://invent.kde.org/graphics/krita) (upstream: KDE). Branch `modern-style`. The owner is a digital artist who paints in Krita daily and wants it to feel as obvious and visual as the tablet apps (Procreate-like) without losing Krita's power. The fork rebuilds the interface ("Studio"); the painting engine, file formats and resources stay Krita's.

The owner works fast and reviews results afterwards. They mostly talk about how things look and feel, not about code.

## How to work with the owner

- **Automate; don't ask what you can decide.** Build, install, relaunch, test and commit without checking in. Ask only for decisions that are really theirs (design direction they reserved, outward-facing actions like pushing or publishing).
- **"Give me suggestions" means text only.** Don't implement suggestions until they say "implement", "do it", or pick numbers. When they pick, do exactly those.
- **Finish every task in a message**, including ones sent mid-turn. Keep a list.
- **Verify visually.** Launch the dev build and capture the window (see Dev workflow) before saying a UI change works. Say plainly what you could not verify.
- **Report like a person:** what changed, what they'll notice, what's left. Short.
- When the owner says something feels wrong, believe it and find the cause; don't argue from the code.

## Design principles

These come from the owner's feedback over many sessions. They decide most questions.

1. **The canvas is the product.** Chrome is a cost. Everything else appears where and when it's needed and gets out of the way.
2. **Needing explanation is a design problem.** Before adding a hint or tooltip, make the control self-evident: a picture, a live preview, direct manipulation, a better default, or removing the option.
3. **No grey helper text. Ever.** No info icons, no descriptions under settings, no "click to…" labels. A short tooltip is the fallback. Grey (secondary ink) is for quiet *values or state*, never for instructions.
4. **Sentence case.** "Open recent", not "Open Recent". Krita's Title Case commands, menus and panel names are shown in sentence case in English (`KisSentenceCase`, applied where actions, menus and panels get their text; names and acronyms keep their capitals); translations are untouched.
5. **Shortest clear text.** "Layer 5", not "Paint Layer 5". Drop type names, restated context and "Click to". Tooltips are one short line; when they explain, they're visual: `0% see-through → 100% solid`, one choice per line, a concrete example ("Ink → Copy of Ink").
6. **Direct manipulation over forms.** Drag a value on its own label or row (layer opacity, brush settings rows, scrubbers). Handles on the canvas instead of number fields (symmetry). Typing is a double-click away, in place: the number itself becomes editable, no box (`widgets/KisStudioInlineEdit`). **Right-click resets a value to its default.** Shift drags finer.
7. **Show, don't name.** Live previews on the real image (effects on canvas, blend modes as thumbnails of the actual picture, brush tooltips with strokes), curves as little drawings, themes as swatches.
8. **Fewer options, better defaults.** Remove settings nobody should need (they were removed: contrast, swap, animation cache, AVX toggles, undo-to-disk…). Merge duplicates. Put an option where its subject is (the eraser's own size is on the Size page) and also in Settings so it can be found.
9. **Organized by intent, found by search.** Group by what the artist wants ("Blur", "Darken"), not by implementation. Everything is reachable with Find (Ctrl+K), including words from other apps.
10. **Perceptual controls.** Brush size moves by ratios (log); opacity stays linear.
11. **Stable places, personal content.** Primary controls don't move. Customizing uses the same gestures as using (drag, right-click) and always has a reset.
12. **One undo.** Ctrl+Z also undoes settings changes, interleaved with document steps by time.
13. **Stay Krita underneath.** Files and presets the fork writes must open in upstream Krita and the other way round. Never change a preset just by loading it. Keep upstream code paths intact so merges from upstream stay easy.

## Look and feel

All tokens live in `libs/widgetutils/KisModernStyle.h`. Use them; don't invent numbers.

- **Spacing:** `hairline` 2, `space1` 4, `space2` 8, `space3` 12, `space4` 16. Nothing else.
- **Corners:** squircle corners everywhere (`KisModernStyle::squircle`, quarters of |x|⁴+|y|⁴=1). Radii by size: `cornerRadius` 5 (controls), `tileRadius` 8, `cardRadius` 10, `panelRadius` 14. Checkboxes are full squircles (`fullSquircleRadius`). Radio buttons stay circles.
- **Motion:** one curve, an exponential ease-out (`motionCurve()`), and three durations: `enterDuration` 180 ms, `exitDuration` 120 ms, `changeDuration` 160 ms.
- **Ink:** text in the palette's text color; quiet values in `secondaryInk()`; fills and tracks as text color at low alpha (works on any theme and on glass). The accent is optional (themes can have none).
- **Glass:** panels, popups and dialogs are frosted glass over the canvas, with edge refraction and a thin Fresnel edge light. See the Glass system below before touching any panel painting.
- **Icons:** `krita/pics/svg/{dark,light}_studio_*.svg`, generated by `krita/pics/svg/make_studio_icons.py` (24 px grid, round strokes; a switch that is on is drawn solid). The app icon is a bare pencil S, white with a soft shadow on dark themes and ink on light ones (`light_`/`dark_krita-branding.svgz`: Krita names icons by their own color, so `dark_` is the ink one); the window and taskbar icon follows Windows' own light/dark setting live (`taskbarIcon()` in `KisApplication`), and places Windows takes from the .exe (file browser, Start, shortcuts) get the white S, whose shadow grows stronger at small sizes so it reads on light backgrounds too; Qt's SVG has no blur, so the shadow is a small PNG inside the SVG. All of it comes from `krita/pics/branding/make_icon.py`: the .kra/.krz file icons and in-app glyphs, the macOS `.icon` bundles and `.icns`, Android's launcher icons and the Windows Store tiles (whose taskbar S follows the light or dark taskbar); the splash wordmark is `krita/data/splash/banner.svg`.

## Main systems

A map, so you know where things are and how they think. Files are in `libs/ui/` unless noted.

- **Studio layout** (`KisStudioLayout`): the whole shell. Left rail (tools, color chip), right rail (panels as flyouts that pin), top bar = the current tool's context (tool chip with the brush name, the tool's own options, size/opacity, the brush and blend mode), then its buttons (`KisStudioBarButtons`: symmetry, shapes, eraser, alpha lock, reload by default; any command can be added, `StudioBarItems`; the paintop box's own buttons are hidden while Studio is on). Canvas-only mode brings the bars back while the pointer is at a canvas edge (`peek`). The classic layout is the `view_studio_layout` toggle.
- **Panel columns** (`KisStudioPanelColumn`, `PanelStack` in `KisStudioLayout.cpp`): pinned panels stack in a glass pill on either side of the canvas: the column with the panel buttons, and a far one across the canvas (`StudioLayoutFarPanels`) for panels dropped near that edge. Each panel is as tall as its contents (measured from its main scroll area: `mainScrollArea`, `contentsHeight`), the pill as tall as its panels; when they don't fit, panels fitted to their contents give up room first. The line between two panels resizes the one above (`StudioLayoutPanelSizes`; right-click fits it again). Krita's resource lists are kept as lists in a column (`keepUpright`), since short ones turn into a strip of tiles.
- **Start page** (`KisStudioStartPage`): with no image open, the Studio shows its own page in place of Krita's welcome page (kept aside, put back on leaving the Studio): new images as tiles drawn in their shape (the size made last, screen, square, A4, 4K, portrait, the clipboard's image, Custom for Krita's dialog), the recent images as large thumbnails (right-click: Forget), Open, and files dropped anywhere. A size tile makes the image at once with the last image's color, background and layers.
- **Tools** (`KisStudioToolRail`, `KisStudioToolStrip`): 37 tools in 9 slots (Paint, Blend, Erase, Select, Move, Fill, Shape, Text, Guides). Slots remember their variant; the brush in use decides whether Paint or Blend is lit. Inline tool options live in the strip.
- **Find** (`KisStudioFind`): one index of items with keys (`tool:…`, `effect:…`, `brush:…`, settings, commands with their classic menu path). Ranking: prefix, word start, initials, all-words, then fuzzy; frecency from usage; aliases from other apps. Right-click or long-press any button opens its card (`KisStudioItemCard`): shortcut as a key cap you can re-record, variants, pin/remove. Tooltips show a button's shortcut after its name (`KisStudioTips::install`).
- **Settings** (`KisSettingsRegistry` + `KisStudioSettings`):
  - The registry is the single source of truth for every setting: id, default, how it's read and written, and flags (`Restart`, `Machine`, `Fork`, `NewViews`). Tooltips live in `explanations()`.
  - `forgetRetired()` is a versioned, one-time migration that clears settings the fork removed. Bump its version and keep it idempotent.
  - The registry also keeps the change history behind settings undo (`KisSettingsUndo` hooks the document undo stack).
  - A write made while another setting is being written (its `written` hook) joins that step, and `setValues()` makes several writes one step, so one Ctrl+Z undoes a theme preset with the theme switch it caused. Write settings through the registry, not `KConfigGroup`, or they fall out of undo.
  - The panel is cards (built with `Builder`: `toggle`, `number`, `choice`, `color`, `row`…), user sections, a Quick section, a Changed filter, a search box and an Undo for changes since it opened.
  - A setting that needs a restart gets a quiet refresh mark that lights up once changed, plus a Restart button. Never write "After a restart" in text.
  - `KisSettingsTransfer` copies settings from a regular Krita (Take); Krita's own files are never written.
- **Theme** (`KisStudioTheme`): a theme is a few params (lightness, tint, tint amount, accent) turned into a generated `.colors` scheme. The presets are Dark, Darker, Graphite, Night, Moss, Light, Paper and Sky. The generated scheme is "Skissa"; the shipped ones are "Skissa Dark/Light". `remake()` renames themes stored under the old placeholder name. Links and focus lines use the accent's hue at a readable lightness.
- **Glass** (`KisStudioGlass`, `opengl/KisOpenGLGlass`):
  - Panels over the canvas are painted *by the canvas* (GL blur/refraction behind them), and the widget paints only its content.
  - A panel is see-through only after a canvas frame has drawn all of it (`Panels::drewOver`); until then it paints solid.
  - Translucent popup windows must never have fully clear pixels: Windows passes the pointer through those to the canvas, which hides the cursor. The style fills them with alpha 1.
  - Captures (`window->grab()`) re-render popups, so they never show first-frame glass bugs.
  - Each panel's blur is kept until something it reads changes: the canvas reports what it redrew (`canvasRedrawn()`), and only panels over that part blur again. The glow around the image is remade only for changes near its edge.
- **Effects on the canvas** (`KisStudioEffectOverlay`, `KisFilterManager::setInlineEditor`): any filter previews live on the layer with a small draggable glass panel. Enter keeps it, Esc drops it, the mask button keeps it editable. An effect opens with the settings it was last applied with (Krita's own "Last Used", `KisFilterManager::lastUsedConfiguration`); the panel's reset shows while they differ from the defaults. The picker's tiles use the same settings, rendered at the level of detail the thumbnail is to the image, so a 20 px blur looks as it will.
- **Brush editor rows** (`kis_paintop_option`, `kis_paintop_settings_widget`, `kis_categorized_item_delegate`, `plugins/paintops/libpaintop`):
  - Each option's row shows its strength (`KisPaintOpOption::RowValue`): drag it, double-click to type, right-click for the brush's value from when it was picked (`rememberRowValues`). A value at 100% is hidden until hover. Locking an option is its lock icon.
  - The Size row is the brush diameter.
  - "The pen is on" means a sensor is ticked. There is no Enable Pen Settings checkbox (`rowCheckedLens`, `sensorsLens`). Opacity and Flow get a row checkbox for the pen. MyPaint rows don't, because MyPaint drops input curves when saved with the pen off.
  - Saved curves (`KisCurvePresetStrip`) are global, stored in `[SavedCurves]`, and renamed inline.
- **Quick brush editor** (`KisStudioBrushEditor`, `KisStudioPenPopup`, `plugins/paintops/libpaintop/KisBrushTipEditor`):
  - Opens from a click on the brush's chip on the top bar, its action, or a second click on the active Paint/Blend/Erase slot. A test stroke, the brush's parts as tabs (Tip, Mask, Texture, Color: the option categories it has; then Stroke), each with its pictures and its settings in use as rows; "+ Add" shows the rest as tiles drawn as what they do to a row of dabs (`widgets/KisStudioDabPictures`). Choices are pictures too: the color sources as little strokes in the current colors, Build up and Wash as overlapping see-through dabs (`KisStudioChips::setTileColumns`, a picture with its name under it). Everything opens the full editor.
  - It edits the paintop box's live option widgets (`KisPaintopBox::currentOptionWidget()`), the same state the full editor shows; `setQuickEditorShown` keeps them read from the preset while it is open.
  - An option shows itself through `KisPaintOpOption`: `rowValue()` (a row), `penInputs()`/`setPenInputActive`/`setPenCurve`/`penMode` (the pen popup: inputs as chips, each with its own curve; editing one makes the option's curves per input), `createPenInputPage` (Fade's length…), `hasQuickPage`/`createQuickPage(category)` (pictures: the tip, mask modes, patterns, color source, painting mode). An option may give a page to another tab (the tip's Noise and Coverage are on Texture).
  - The tip editor draws the real dab (`KisAutoBrushFactory`) with handles: size along and across (kept in proportion unless the chain is open; Shift flips it for one drag), soft edge (fade, shown as softness), angle; values are written on it. Krita keeps a tip wider than tall, so a taller one is stored turned a quarter. Round, square, star (spikes > 2) and picture tips are one row; Text tips are only kept for presets that have them. Angle selectors have no flip buttons.
  - The Size and Spacing rows are the tip's diameter and spacing (`KisBrushBasedPaintopOptionWidget::paintOpOptionAdded`).
  - Everything is the brush sheet (`KisStudioBrushSheet`): a column per part with its pictures and every setting (off ones quiet), the options without a row as "Name ›" chips that show Krita's own page, lent by the settings widget (`lendPage`/`returnPage`), a pen grid (settings × inputs, a curve where on), Save, Save as new, a new brush of another engine, and Copy as a picture (without the buttons). Krita's editor stays behind its ⋯ (scratchpad, renaming, Instant Preview).
  - While either editor is open, a brush change paints the last stroke again with it (`KisLastStrokeReplay`, the editors' re-stroke toggle): the freehand helper records what a stroke painted (dabs, lines, curves, its color), and the stroke's step is undone (`setIndex`, not `undo()`, which takes settings changes back first) and painted again. A smoothing change paints it again from the pen's own path (recorded before smoothing) through the smoothing as now set; the stabilizer is replayed by its millisecond samples, close to but not exactly as live. Only for a stroke of that brush, only while its step is the newest with nothing undone after it.
  - A row that is an effect the brush can go without (Rotation, Scatter) comes off by its cross under the pointer, or pulled up or down off the editor; pulled onto the top bar, it is pinned there. Sideways is always the value.
  - Any row pins to the top bar (its pin, or pulled up onto the bar) as `brushsetting:<KisPaintOpOption::id()>` (Find's `brush:` keys are presets); the bar shows it for any brush that has that setting.
- **Smoothing** (`KisStudioSmoothing`): the brush tool's line smoothing as one amount (0 off, then the distance by ratios), shown as a fill behind the brush's name on the top bar's tool chip. Dragging the chip changes it, a right click resets it; a click opens the brush editor (there is no brush editor button on the bar any more). Each brush keeps its own smoothing (`[StudioBrushSmoothing]`, by the brush's file, so a rename keeps it; older entries by name are still read; a brush never changed takes on the one in use). The stabilizer evens pressure and tilt out over only ~25 ms around where its line is (`stabilizedSensorsReach`), not its whole window as Krita does, which flattened quick strokes' pressure at high strengths. Held W and a sideways drag on the canvas change it too (`KisDragValueAction`, Drag to Change). Profiles saved before a canvas action got a new mode get its bundled key (`modes` in the profile's General group).
- **Stroke tab** (`KisStudioStrokePage`, the brush editor's last tab for the freehand brush tools): a wobbly hand line run through Krita's own weighted and stabilizer math as set, the kinds as pills (Off is Krita's Basic; None isn't offered), each kind's few settings (one Strength, or slow and fast strokes apart once its link is opened; Follows zoom for weighted), one Smooth Pressure for both kinds (the classic options' Stabilize Sensors follows it), Pixel perfect for pixel brushes (named so, or a hard auto tip without anti-aliasing), Shapes, snapping to guides when the image has some (off at 0, `KisToolBrush`'s assistant properties): all of the brush tool's options, so it has no link to them; Dyna and Multibrush get Tool options for their own.
- **Pinned controls** (`KisStudioControls`): a brush setting (`brushsetting:<option id>`) or one of Krita's (`setting:<registry id>`: switches, numbers, choices that apply at once) as a row, on the top bar or in the Controls panel (`KisStudioControlsPanel`, a dock). Settings rows show a pin under the pointer; the panel's "+" finds any setting; rows are pulled out to move them between the bar and the panel, or off to remove them. A fraction setting (0 to a few) shows as percent.
- **Line art fill** (`libs/image/floodfill/KisLineArtFill`, `KisColorDrop`):
  - `KisLineArtFill::grow` runs after Krita's flood fill (`KisFillPainter::setLineArt`): a watershed on the difference from the clicked color, so the fill reaches under the lines to where they meet the next area. Every one-color area with an inside competes (paper, earlier fills, the solid middle of thick lines); thin bands (soft line edges), specks and corners the gap closing cut off are taken in. Only when filling another layer than the one read; the Fill tool has Under lines and Specks.
  - A color dragged onto the canvas (from the rail chip, the color panel's swatches, a palette) previews the area in that color; resting, then moving sideways, tunes the threshold. The drop fills a "Colors" layer of its own when the active layer holds the lines (below clear line art, Multiply above opaque), sizes the gap closing from the line width, and leaves a chip that keeps filling. With a modifier held, Krita's own color drop runs instead.
  - A color dropped onto a layer row recolors it (`KisNodeModel::dropMimeData`); between rows it still makes a fill layer. Layer → Auto Flats gives every enclosed area its own color (`KisLineArtFill::flats`).
- **Shapes** (`tool/KisStrokeShape`, `KisToolFreehand`): with the Shapes button on, a stroke held still for half a second at its end becomes a clean line, arc, ellipse (near-round → circle), polygon (near-square → rectangle) or polyline; the helper's `repaintAs` drops the stroke and paints the shape, carrying its pressure, as one undo step. Polygons fit a line to the middle of each side and put corners where they meet (rounded corners and overshoots still snap); the stroke must go round once without crossing itself and turn at each corner. Thresholds are in screen pixels, tuned on synthetic hand strokes so scribbles, waves and spirals never snap. The colorize mask brush opts out (`m_snapsToShapes`).
- **Time-lapse** (`plugins/dockers/recorder`): a panel with a scrubbable preview of the recorded frames, a record button, the clip length (drag), MP4/GIF/WebM chips and an Export that saves "<image> time-lapse.<ext>" beside the image; the rest is in its ⋯ menu. Krita's recorder and ffmpeg export underneath.
- **Symmetry** (`KisStudioSymmetry`, `plugins/tools/basictools/kis_tool_multihand`): one button with Mirror/Radial/Tile modes. Radial and Tile use the Multibrush tool and return to the previous tool. Handles are drawn with `paintToolOutline`.
- **Window frame** (`KisStudioWindowFrame`): the window draws its own title bar. Maximized, its client area is the screen's work area (asked from Windows, not Qt, whose state lags behind), less 2 px on an auto-hiding taskbar's edge so the taskbar can still come up. Windows' window animations are off while in canvas-only mode, so leaving it doesn't animate from the restored size.
- **Its own folders** (`libs/global/KisProfile`): nothing is shared with a regular Krita on the same computer. Config files and logs live in `<LocalAppData>/<name>/` (`KisProfile::configFile`), resources and other app data in `<AppData>/<name>/krita` (the Qt organization name), autosaves of unsaved images in `<Temp>/<name>`; the single-instance key differs. Any new file the fork writes goes through `KisProfile`, never a bare `QStandardPaths::GenericConfigLocation` or `"kritarc"`. Krita's files are only ever read (Take Krita's settings), never written. The dev profile keeps its old layout inside `qttest`; `KRITA_DEV_APART=1` runs the new layout there for testing.
- **Name and links** (`KisProfile::name()`, `KisProfile::homepage()`): text that means this app says Skissa; text about Krita the project, its files or its docs stays Krita. Report Bug opens the homepage's issues. Krita's update checker is off (`ENABLE_UPDATERS`).
- **Saving on Windows** (`KisImportExportManager`): the new file is written beside the old one and swapped in (`MoveFileEx`), so a failed save never leaves a broken file.
- **Releases** (`packaging/windows/make-release.py`): a portable zip that follows the DLL imports of everything it ships, with the license texts and a SOURCE.txt pointing at the exact tag. Build it from a staged copy of the install (`cmake --install build --prefix <copy>`) when the dev Krita is in use. DLLs loaded by name at run time (ANGLE, the crash handler) have no import to follow: they are in its `LOADED` list.
- **Translations** (`packaging/skissa-translations.py`): the fork's text gets only translations Krita already has (same text elsewhere, Krita→Skissa sentences), written to `po-skissa/<lang>.po`; Krita's `po/` stays exactly as upstream has it, and the build joins the two (`cmake/modules/SkissaTranslations.cmake`, in place of `ki18n_install`, compiling a language only when its files change). A word or two is reused only when Krita translates it the same way in several places; `OTHER_SENSE` lists text used in another sense. Never write translations by hand. Rerun it after merging upstream translations or changing text.
- **What Studio leaves out**: classic-only menu items (toolbars, styles, themes menu, status bar, window tiling, About KDE) are hidden while Studio is on (`hiddenToolbarActions`), and Find skips hidden actions. Panel plugins the Studio covers otherwise are not built (`plugins/dockers/CMakeLists.txt`: the small, specific and artistic color selectors, brush history, tasksets, log, touch docker), nor is the G'MIC bridge (G'MIC-Qt itself isn't built, so its menu entry only failed); their sources stay for merges. The toolbox is core, so it is only hidden from the panel picker and Find (`KisStudioPanelPicker::isRetired`). After dropping a plugin, delete its DLL from the install: `cmake --install` never removes old files.
- **Small systems:**
  - Studio popups (`widgets/KisStudioPopup`): a glass panel in its own window, closed by a press elsewhere unless pinned; popups opened from it count as inside (`KisPopupCloser`). A plain `QWidget` (no `Q_OBJECT`, no mouse tracking) counts as background that drags the window (`KisStudioGlass::setMovableByBackground`): give interactive widgets their own class. A widget that ignores presses lets them reach that background too (`KisStudioChips` takes its own).
  - Empty lists say why they're empty (`KisStudioEmptyStates`).
  - Tooltips teach the gestures, so people learn to expect them. They are written as what the control is, then a line per gesture worded alike everywhere ("Drag: change", "Double-click: type", "Right-click: reset", "Scroll: step"; `KisStudioTips`), but shown without those colons: the name, then the gestures in a row, each a little mouse doing it (the pressed button lit in the accent, an arrow for a drag, the wheel) and a word (`KisStudioTips::html`, which every tooltip goes through). The owner dislikes colon-and-line blocks of text in tooltips. Value rows add their gestures by themselves.
  - Touch mode (`KisTouchMode`, fingers can't hover) shows what otherwise appears only on hover: layer eyes and marks, value rows' pins and crosses, settings pins, the rail's "+". A finger held still at the end of a drag isn't a reset (value rows, scrubbers, the tool chip, the tip editor check that the press didn't drag). Menus open on the context menu event, not a right-button press, so a held finger opens them too.
  - Choices whose names are jargon carry a one-line tooltip: pen inputs (`KisStudioPenPopup::inputTip`), brush settings (`KisStudioBrushEditor::optionTip`, by option id), texture and mask modes, color sources, tip chips. A gesture is written on its own line ("Drag: move\nClick: fold"), never "click to"; it shows as a picture.
  - The brush tooltip is a card showing the other picture (stroke or icon), tags and location (`resourcewidgets/KisIconToolTip`).
  - Autosave waits for a pause in work, at most a minute (`KisDocument`).
  - Per-layer stroke logs are saved in .kra (`image/KisStrokeLog`).
  - Krita reopens the last image at start, guarded against crash loops.
  - Frame pacing for VRR screens (`KisUpdatePacer`).

## Dev workflow (Windows)

The build environment is outside the repo, in `C:\code\krita-deps`: llvm-mingw, KDE's prebuilt Qt5 deps in `env\_install`, the build dir `build`, and helper scripts in `devtools`.

- **The fork's own files in `libs/ui` are listed in `libs/ui/skissa.cmake`**, not in kritaui's list: they build as their own part of kritaui, so editing one rebuilds in ~5 s instead of ~14 s (Qt's moc otherwise re-checks every kritaui header). Add new `libs/ui` files there. A form header (`ui_*.h`) they include goes in its `kritaui_skissa_forms` list.
- **One command:** `devtools\dev.ps1` builds, closes the dev Krita and waits for it, installs and launches (~10–20 s). Flags: `-NoBuild`, `-NoLaunch`. Build errors are matched case-sensitively; cmake's Python `ModuleNotFoundError` lines are harmless.
- **Always run the dev build through `run-krita.bat`** / `launchdev.ps1` (hidden console). It sets `KRITA_DEV_PROFILE=1` (Qt test mode), so settings and resources live in `%LOCALAPPDATA%\qttest` and `%APPDATA%\qttest`, with a separate single-instance key. **Never touch the owner's real Krita profile** (`%LOCALAPPDATA%\kritarc`, `%APPDATA%\krita`).
- **Driving the app:** the dev build listens for posted window messages (`krita/main.cc`): `KritaDevCapture` (renders its own windows to a PNG; no screen capture, ever), `KritaDevMouse`, `KritaDevKey`, `KritaDevTree`, `KritaDevExport`, `KritaDevShortcuts`, `KritaDevColorDrop`. Helpers: `devinput.ps1` (move, click, rclick, line, type, key, capture), `pen.ps1` (a pen stroke whose pressure swells and fades; KritaDevMouse actions 12–14 carry the pressure), `colordrop.ps1` (a color over the canvas, dropped, left; auto flats; recolor: a real drag can't be faked), `tree.ps1`, `kritawin.ps1`.
  - Captures miss QPainter-only canvas decorations; GL-drawn tool outlines show.
  - Synthetic mouse moves don't move the OS cursor: use the event's `globalPos`, not `QCursor::pos()`.
  - Inside popups, Qt can hand `QContextMenuEvent::pos()` in window coordinates: map from `globalPos()`.
- **The shell sees a stale copy of `%LOCALAPPDATA%`** (the desktop app is an MSIX package). To read or edit the real dev `kritarc`, copy it with a WMI-launched process (`Invoke-CimMethod Win32_Process Create`), and edit it only while the dev Krita is closed.
- **Back up the dev profile before a test writes to it,** and restore exactly what was there. The owner also works in the dev build; tests have overwritten their saved curves and theme before.
- **It's fine to close the dev Krita** (unsaved changes and all) to rebuild. Only kill processes whose path is under `C:\code\krita-deps`.
- **README clips:** `devtools\rec.py` (Python, Pillow) drives the app and captures frames; save them as animated WebP (sharper and smaller than GIF). A capture takes ~1 s, so clips are stop-motion with chosen frame times.
- **Startup:** `KRITA_DEV_SAMPLE=<file>` samples it, `profile.py <file> --timeline 250` shows where the time went (`--callers`/`--callees` follow a function). `QT_LOGGING_RULES=krita.performance.debug=true` prints Krita's own phase timings. Most of it is Krita's (the OpenGL probe ~0.9 s, mostly the driver starting; opening the image), not the fork's.
- `KRITA_DEV_TOUR=<folder>` runs `krita/KisDevTour.cpp`, which photographs the whole UI. `KRITA_DEV_FRAMELOG` and `KRITA_DEV_SAMPLE` are for performance work.
- **Tests (the safety net):** `devtools\tests.ps1` builds Krita's unit tests in `build-tests` (a second build of the same source), runs them, runs the failures again one at a time (some only fail when run together), and reports any failure untouched Krita doesn't share. `known-failing-tests.txt` lists those Krita shares, checked against `build-upstream` (`baseline-tests.bat`, a worktree of the merged upstream commit). Run it after a merge from Krita and after changes to Krita's own code (libs/image, pigment, flake, file formats). The UI tour (`KRITA_DEV_TOUR`, below) is the crash sweep for the interface.
- **The test image** is `C:\code\krita-deps\sunset.png` (the default in `launchdev.ps1`); `glass-test.jpg` is the Krita artwork.

## Updating from Krita

`origin` is Krita (KDE), `skissa` is the fork's own repo. Merge Krita in often (small merges are easy); never rebase the branch.

- **One step:** `devtools\update.ps1` runs `packaging/skissa-update.py`: fetch, merge (rerere on, so a conflict settled once is settled again the same way), the fork's translations refreshed and committed, build and install, start and capture. Exit 2 leaves the merge open with the files to settle: settle, commit, run it again. Check the capture, then push.
- **Keep Krita's files mergeable:**
  - Put new code in new files and call it from Krita's with a line or two.
  - Turn Krita's code off in the build (CMake), don't delete it: an unbuilt file stays as upstream has it and merges cleanly.
  - Don't append to Krita's lists or files when a file of the fork's own can hold it (`po-skissa/`, `libs/ui/skissa.cmake`).
- **Check a merge before doing it:** `git merge-tree --write-tree --name-only HEAD origin/master` lists the files that would conflict.

## Legal and credit

- Never use Krita's name or logo as this app's name or icon. "An independent fork of Krita" in plain text is fine, with the not-affiliated line (README, NOTICE, About dialog).
- Keep every copyright and SPDX header; new files get the owner's header (`GeorgeAzma`). GPL-3.0 section 5a: the modification notice stays in README, NOTICE and the About dialog.
- Credit art by author, title and license: the Kiki splash is Tyson Tan's, CC BY-SA 4.0. Only use images whose source is known.
- A release zip ships the third-party license texts and points to the dependencies' sources, and each release links its exact git tag.

## Code style

- Match Krita's style: 4 spaces, the file's brace style, `i18nc` with a context for every user-visible string.
- **Comments are short, natural English** that say why or what is not obvious. No history ("was", "now", "fixed"), no mention of reviews or conversations, and no comments that restate the code.
- No dead code, no commented-out code, no debug output left behind.
- **Commit messages:** a subject that says what the change does for the user ("Brush settings: each row is its value"), a short body if needed, and the `Co-Authored-By` line the session asks for. Stage only your own files and hunks: other agents may be working in the same tree.

## Open questions and known issues

- **The app is named Skissa** everywhere it names itself. The exe stays `krita.exe` and the internal ids (`krita` component, `.kra`) stay, as they should. The splash art is Kiki, Krita's mascot (credited, CC BY-SA); art of the fork's own would separate it further.
- **The homepage `github.com/GeorgeAzma/skissa` is assumed** (`KisProfile::homepage()`, `make-release.py`); change both if the repo lives elsewhere.
- **The README says plainly that all the code is AI-written** and the ideas are the owner's; keep that true and visible. Its donate button goes to Krita.
- Take Krita's settings copies settings only, not the user's own brushes and bundles from Krita's resource folder.
- **Pre-5.0 brush engine paths were removed** (legacy smudge, spray center bias, hard flow). Old presets that used them paint slightly differently.
- **G'MIC-Qt isn't built** in `krita-deps`; only the bridge plugin is. The idea on the table is wrapping the useful G'MIC filters as native effects.
- **The owner asked to be reminded** of shader filters and custom blend modes written the same way (shader-like code).
- **Brush editor redesign:** the quick editor, the sheet and pinning are done; the owner will review them and ask for changes.
- **Ideas the owner liked:** shader-based custom filters in the effect panel (Shadertoy-style GLSL; the sliders come from uniforms), and a brush editor redesign around a tip/mask/texture stack and a dynamics grid (sensors × properties).
- **Ideas weighed and left:** handles to nudge a snapped shape (an editing state after every stroke), time-lapse trimming (the export's ffmpeg templates are saved settings that read every frame; trimming means changing them or copying frames), a theme taken from the image (a theme is a stable personal choice), a canvas flip gesture (not wanted). Lasso fill: Krita's Freehand Path brush already does it.

## Lessons (append, newest last)

- Check the dev profile's state before blaming code: a "wrong" value is often a setting (e.g. "Largest brush" capping the size display).
- A setting the fork stops showing must still be read correctly, or be migrated. Deleting a key upstream still reads is a bug.
- Count only deliberate input as "the user is working": pen hover sends moves all the time.
- When a feature writes test data into the profile, prefer the UI (with the panel's Undo) over editing files.
- Public text (README, release notes) is shown, not told: a clip or screenshot per feature, a line of words at most, plain first-person voice. Bold lead-ins, feature lists and comparison tables read as machine-written to the owner.
- After a build, check the installed DLL's time: an install can silently keep the old one when the dev Krita was still running.
- Another session may be driving the dev Krita too (and may have started it with other variables, such as `KRITA_DEV_APART`). If a capture shows a fresh-looking profile, check the process's command line and parent before suspecting the code.
- Windows caches program icons by path: after the .exe's icon changes, Task Manager and Explorer can keep showing the old one until `ie4uinit.exe -show` refreshes the cache.
- Creating a `QOpenGLFramebufferObject` in the middle of a frame leaves bound the framebuffer Qt last knew of, not the one that was bound: the rest of the frame lands off screen. Create them through `KisOpenGLGlass`'s `texture()`, which keeps the binding. (This was the glass missing under a menu the first time it opened.)
- A fill run through `FillProcessingVisitor` changes the layer but not the canvas: add a `KisUpdateCommand` with its dirty rect to the same stroke, as the fill tool does.
- Fills that read the whole picture must not take earlier fills for lines: what counts as a line (for line width, for the layer that holds the lines) is dark ink, not "differs from the clicked color".
- Python one-liners in a bash heredoc turn `\n` inside C++ strings into real newlines: make escape-heavy edits with the Edit tool.
- PowerShell's `[int]` rounds (to even), it doesn't truncate: use `[math]::Floor` in test scripts. `Set-Content -Encoding utf8` writes a BOM, which CMake files can't take.
- In a fresh profile (`KRITA_DEV_APART`) the UI scale differs, so devinput coordinates are logical pixels there; in the dev profile they match the capture's pixels.
- Find's application filter opens its own card on any button's right click: a button with a card of its own sets the `ownContextMenu` property.
- Widgets made before `KisStudioGlass::installMenuGlass()` were polished without it (resource lists' item tooltips): the install pass converts those, and anything new must be polished before its native window is created.
- A long press arrives as a plain right click; `longPressAt` on `qApp` tells them apart.
- Ctrl+Z in a test also undoes settings changes (one undo), so undoing test strokes can undo a toggle made for the test.
- The UI tour (`KRITA_DEV_TOUR`) decides what is safe to press from a button's text, tooltip, object name and accessible name: a button with none of these (an icon-only button whose tooltip was removed) must still get a name, or the tour may press Close. Run it with `KRITA_DEV_APART=1` so it can't touch the dev profile.
- The owner uses the dev build between turns (pins, tweaks): back up the profile right before each test, not once per session, and capture before clicking by coordinates, since the bar may have moved.
- A stroke's step reaches the undo stack a moment after the image goes idle: wait a turn of the event loop before deciding a stroke was cancelled.
- A drag owned by the widget it starts from dies with it: anything rebuilt while the drag runs (a brush picked, a card refreshed) takes the QDrag along and Qt crashes. Parent drags to `qApp` and delete them after `exec()`.
- A tool's `updateSettingsViews()` must block its widgets' signals while it sets them: one set first writes its stale neighbours back into the options (the smoothing sliders, the combo's ranges).
- Anything that keeps the brush's option widget in step (`KisPaintopBox::useOptionWidget`) takes and gives back a use; one shared flag let one closing editor stop another, or pinned rows go stale.
- "Pen pressure doesn't work" can be the pen arriving as a mouse: The tablet driver can refuse WinTab to every program, upstream Krita included (seen with the owner's XP-Pen; why is not known yet). At start the app tries to open WinTab and uses Windows Ink for that session when it is refused (`winTabRefused()` in `krita/main.cc`), without changing the saved setting. Test the app side with `pen.ps1`; for the real pen, log whether the canvas gets tablet or mouse presses.
- A console program started through WMI (`Win32_Process.Create`) hangs when Windows Terminal is the default console: Terminal takes the console and the program never runs. Start them detached (`Win32_ProcessStartup` with `CreateFlags = 8`, and `ShowWindow = 0`).
- A second Krita started with `KRITA_DEV_APART` hands itself to a running dev Krita and quits: close the dev Krita before the UI tour.
