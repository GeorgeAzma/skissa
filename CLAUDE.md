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
4. **Shortest clear text.** "Layer 5", not "Paint Layer 5". Drop type names, restated context and "Click to". Tooltips are one short line; when they explain, they're visual: `0% see-through → 100% solid`, one choice per line, a concrete example ("Ink → Copy of Ink").
5. **Direct manipulation over forms.** Drag a value on its own label or row (layer opacity, brush settings rows, scrubbers). Handles on the canvas instead of number fields (symmetry). Typing is a double-click away. **Right-click resets a value to its default.** Shift drags finer.
6. **Show, don't name.** Live previews on the real image (effects on canvas, blend modes as thumbnails of the actual picture, brush tooltips with strokes), curves as little drawings, themes as swatches.
7. **Fewer options, better defaults.** Remove settings nobody should need (they were removed: contrast, swap, animation cache, AVX toggles, undo-to-disk…). Merge duplicates. Put an option where its subject is (the eraser's own size is on the Size page) and also in Settings so it can be found.
8. **Organized by intent, found by search.** Group by what the artist wants ("Blur", "Darken"), not by implementation. Everything is reachable with Find (Ctrl+K), including words from other apps.
9. **Perceptual controls.** Brush size moves by ratios (log); opacity stays linear.
10. **Stable places, personal content.** Primary controls don't move. Customizing uses the same gestures as using (drag, right-click) and always has a reset.
11. **One undo.** Ctrl+Z also undoes settings changes, interleaved with document steps by time.
12. **Stay Krita underneath.** Files and presets the fork writes must open in upstream Krita and the other way round. Never change a preset just by loading it. Keep upstream code paths intact so merges from upstream stay easy.

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

- **Studio layout** (`KisStudioLayout`): the whole shell. Left rail (tools, color chip), right rail (panels as flyouts that pin), top bar = the current tool's context (tool chip with the brush name, brush/blend/size/opacity, symmetry). Canvas-only mode brings the bars back while the pointer is at a canvas edge (`peek`). The classic layout is the `view_studio_layout` toggle.
- **Tools** (`KisStudioToolRail`, `KisStudioToolStrip`): 37 tools in 9 slots (Paint, Blend, Erase, Select, Move, Fill, Shape, Text, Guides). Slots remember their variant; the brush in use decides whether Paint or Blend is lit. Inline tool options live in the strip.
- **Find** (`KisStudioFind`): one index of items with keys (`tool:…`, `effect:…`, `brush:…`, settings, commands with their classic menu path). Ranking: prefix, word start, initials, all-words, then fuzzy; frecency from usage; aliases from other apps. Right-click or long-press any button opens its card (`KisStudioItemCard`): shortcut as a key cap you can re-record, variants, pin/remove. Tooltips show a button's shortcut (`ShortcutTips` in `KisStudioFind.cpp`).
- **Settings** (`KisSettingsRegistry` + `KisStudioSettings`):
  - The registry is the single source of truth for every setting: id, default, how it's read and written, and flags (`Restart`, `Machine`, `Fork`, `NewViews`). Tooltips live in `explanations()`.
  - `forgetRetired()` is a versioned, one-time migration that clears settings the fork removed. Bump its version and keep it idempotent.
  - The registry also keeps the change history behind settings undo (`KisSettingsUndo` hooks the document undo stack).
  - The panel is cards (built with `Builder`: `toggle`, `number`, `choice`, `color`, `row`…), user sections, a Quick section, a Changed filter, a search box and an Undo for changes since it opened.
  - A setting that needs a restart gets a quiet refresh mark that lights up once changed, plus a Restart button. Never write "After a restart" in text.
  - `KisSettingsTransfer` copies settings between this fork and a regular Krita (Take/Give/Restore).
- **Theme** (`KisStudioTheme`): a theme is a few params (lightness, tint, tint amount, accent) turned into a generated `.colors` scheme. The presets are Dark, Darker, Graphite, Night, Moss, Light, Paper and Sky. The generated scheme is "Skissa"; the shipped ones are "Skissa Dark/Light". `remake()` renames themes stored under the old placeholder name. Links and focus lines use the accent's hue at a readable lightness.
- **Glass** (`KisStudioGlass`, `opengl/KisOpenGLGlass`):
  - Panels over the canvas are painted *by the canvas* (GL blur/refraction behind them), and the widget paints only its content.
  - A panel is see-through only after a canvas frame has drawn all of it (`Panels::drewOver`); until then it paints solid.
  - Translucent popup windows must never have fully clear pixels: Windows passes the pointer through those to the canvas, which hides the cursor. The style fills them with alpha 1.
  - Captures (`window->grab()`) re-render popups, so they never show first-frame glass bugs.
- **Effects on the canvas** (`KisStudioEffectOverlay`, `KisFilterManager::setInlineEditor`): any filter previews live on the layer with a small draggable glass panel. Enter keeps it, Esc drops it, the mask button keeps it editable.
- **Brush editor rows** (`kis_paintop_option`, `kis_paintop_settings_widget`, `kis_categorized_item_delegate`, `plugins/paintops/libpaintop`):
  - Each option's row shows its strength (`KisPaintOpOption::RowValue`): drag it, double-click to type, right-click for the brush's value from when it was picked (`rememberRowValues`). A value at 100% is hidden until hover. Locking an option is its lock icon.
  - The Size row is the brush diameter.
  - "The pen is on" means a sensor is ticked. There is no Enable Pen Settings checkbox (`rowCheckedLens`, `sensorsLens`). Opacity and Flow get a row checkbox for the pen. MyPaint rows don't, because MyPaint drops input curves when saved with the pen off.
  - Saved curves (`KisCurvePresetStrip`) are global, stored in `[SavedCurves]`, and renamed inline.
- **Symmetry** (`KisStudioSymmetry`, `plugins/tools/basictools/kis_tool_multihand`): one button with Mirror/Radial/Tile modes. Radial and Tile use the Multibrush tool and return to the previous tool. Handles are drawn with `paintToolOutline`.
- **Its own folders** (`libs/global/KisProfile`): nothing is shared with a regular Krita on the same computer. Config files and logs live in `<LocalAppData>/<name>/` (`KisProfile::configFile`), resources and other app data in `<AppData>/<name>/krita` (the Qt organization name), autosaves of unsaved images in `<Temp>/<name>`; the single-instance key differs. Any new file the fork writes goes through `KisProfile`, never a bare `QStandardPaths::GenericConfigLocation` or `"kritarc"`. Krita's files are only ever read (Take Krita's settings), never written. The dev profile keeps its old layout inside `qttest`; `KRITA_DEV_APART=1` runs the new layout there for testing.
- **Small systems:**
  - Empty lists say why they're empty (`KisStudioEmptyStates`).
  - The brush tooltip is a card showing the other picture (stroke or icon), tags and location (`resourcewidgets/KisIconToolTip`).
  - Autosave waits for a pause in work, at most a minute (`KisDocument`).
  - Per-layer stroke logs are saved in .kra (`image/KisStrokeLog`).
  - Krita reopens the last image at start, guarded against crash loops.
  - Frame pacing for VRR screens (`KisUpdatePacer`).

## Dev workflow (Windows)

The build environment is outside the repo, in `C:\code\krita-deps`: llvm-mingw, KDE's prebuilt Qt5 deps in `env\_install`, the build dir `build`, and helper scripts in `devtools`.

- **One command:** `devtools\dev.ps1` builds, closes the dev Krita and waits for it, installs and launches (~10–20 s). Flags: `-NoBuild`, `-NoLaunch`. Build errors are matched case-sensitively; cmake's Python `ModuleNotFoundError` lines are harmless.
- **Always run the dev build through `run-krita.bat`** / `launchdev.ps1` (hidden console). It sets `KRITA_DEV_PROFILE=1` (Qt test mode), so settings and resources live in `%LOCALAPPDATA%\qttest` and `%APPDATA%\qttest`, with a separate single-instance key. **Never touch the owner's real Krita profile** (`%LOCALAPPDATA%\kritarc`, `%APPDATA%\krita`).
- **Driving the app:** the dev build listens for posted window messages (`krita/main.cc`): `KritaDevCapture` (renders its own windows to a PNG; no screen capture, ever), `KritaDevMouse`, `KritaDevKey`, `KritaDevTree`, `KritaDevExport`, `KritaDevShortcuts`. Helpers: `devinput.ps1` (move, click, rclick, line, type, key, capture), `tree.ps1`, `kritawin.ps1`.
  - Captures miss QPainter-only canvas decorations; GL-drawn tool outlines show.
  - Synthetic mouse moves don't move the OS cursor: use the event's `globalPos`, not `QCursor::pos()`.
  - Inside popups, Qt can hand `QContextMenuEvent::pos()` in window coordinates: map from `globalPos()`.
- **The shell sees a stale copy of `%LOCALAPPDATA%`** (the desktop app is an MSIX package). To read or edit the real dev `kritarc`, copy it with a WMI-launched process (`Invoke-CimMethod Win32_Process Create`), and edit it only while the dev Krita is closed.
- **Back up the dev profile before a test writes to it,** and restore exactly what was there. The owner also works in the dev build; tests have overwritten their saved curves and theme before.
- **It's fine to close the dev Krita** (unsaved changes and all) to rebuild. Only kill processes whose path is under `C:\code\krita-deps`.
- **README clips:** `devtools\rec.py` (Python, Pillow) drives the app and captures frames; save them as animated WebP (sharper and smaller than GIF). A capture takes ~1 s, so clips are stop-motion with chosen frame times.
- `KRITA_DEV_TOUR=<folder>` runs `krita/KisDevTour.cpp`, which photographs the whole UI. `KRITA_DEV_FRAMELOG` and `KRITA_DEV_SAMPLE` are for performance work.
- **The test image** is `C:\code\krita-deps\sunset.png` (the default in `launchdev.ps1`); `glass-test.jpg` is the Krita artwork.

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

- **The app is named Skissa** (`KisProfile::name()`, which also names the folders). Window title, About, splash, icons, theme names, dialog titles and the exe's file info use it. Still Krita's, to do before a public release:
  - about 100 sentences that name Krita as the app ("Krita will back up…"); they could take `KisProfile::name()`;
  - the exe is `krita.exe`, and the internal ids (`krita` component, `.kra`) stay, as they should;
  - Help → Report Bug goes to bugs.kde.org; it should go to the fork's own issues once its repo exists, and the About "Source code" link should point there too;
  - the splash art is Kiki, Krita's mascot (credited, CC BY-SA); art of the fork's own would separate it further.
- Take Krita's settings copies settings only, not the user's own brushes and bundles from Krita's resource folder.
- **Pre-5.0 brush engine paths were removed** (legacy smudge, spray center bias, hard flow). Old presets that used them paint slightly differently.
- **G'MIC-Qt isn't built** in `krita-deps`; only the bridge plugin is. The idea on the table is wrapping the useful G'MIC filters as native effects.
- **Ideas the owner liked:** shader-based custom filters in the effect panel (Shadertoy-style GLSL; the sliders come from uniforms), and a brush editor redesign around a tip/mask/texture stack and a dynamics grid (sensors × properties).

## Lessons (append, newest last)

- Check the dev profile's state before blaming code: a "wrong" value is often a setting (e.g. "Largest brush" capping the size display).
- A setting the fork stops showing must still be read correctly, or be migrated. Deleting a key upstream still reads is a bug.
- Count only deliberate input as "the user is working": pen hover sends moves all the time.
- When a feature writes test data into the profile, prefer the UI (with the panel's Undo) over editing files.
- Public text (README, release notes) is shown, not told: a clip or screenshot per feature, a line of words at most, plain first-person voice. Bold lead-ins, feature lists and comparison tables read as machine-written to the owner.
- After a build, check the installed DLL's time: an install can silently keep the old one when the dev Krita was still running.
- Another session may be driving the dev Krita too (and may have started it with other variables, such as `KRITA_DEV_APART`). If a capture shows a fresh-looking profile, check the process's command line and parent before suspecting the code.
- Windows caches program icons by path: after the .exe's icon changes, Task Manager and Explorer can keep showing the old one until `ie4uinit.exe -show` refreshes the cache.
