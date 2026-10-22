<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/readme/icon-dark.png">
  <img src="docs/readme/icon-light.png" width="64" alt="">
</picture>

# Skissa
Fork of Krita, with a redesigned UI targeting intuition.

![Skissa](docs/readme/hero.jpg)

<table>
<tr>
<td width="50%"><img width="420" src="docs/readme/move.webp" alt="Panels and the top bar dragged anywhere"><br><b>Put anything anywhere</b></td>
<td width="50%"><img width="420" src="docs/readme/effects.webp" alt="An effect previewed on the image"><br><b>Effects on the image</b></td>
</tr>
<tr>
<td><img width="420" src="docs/readme/brush.webp" alt="Dragging values in the brush editor"><br><b>Drag any value</b></td>
<td><img width="420" src="docs/readme/find.jpg" alt="Ctrl+K finding effects, brushes and settings"><br><b>Ctrl+K</b></td>
</tr>
<tr>
<td><img width="420" src="docs/readme/themes.webp" alt="Themes on glass"><br><b>Themes and glass</b></td>
<td><img width="420" src="docs/readme/blend-modes.jpg" alt="Blend modes shown on the picture"><br><b>Blend modes you can see</b></td>
</tr>
<tr>
<td><img width="420" src="docs/readme/brush-editor.jpg" alt="The quick brush editor"><br><b>Brushes</b></td>
<td><img width="420" src="docs/readme/settings.jpg" alt="Settings as cards"><br><b>Settings</b></td>
</tr>
<tr>
<td><img width="420" src="docs/readme/color.jpg" alt="The color panel"><br><b>Color</b></td>
<td><img width="420" src="docs/readme/menu.jpg" alt="The menu"><br><b>Every Krita menu</b></td>
</tr>
<tr>
<td><img width="420" src="docs/readme/symmetry.webp" alt="Radial symmetry, center dragged into place"><br><b>Symmetry</b></td>
<td><img width="420" src="docs/readme/start.jpg" alt="The start page"><br><b>Start page</b></td>
</tr>
<tr>
<td><img width="420" src="docs/readme/brush-sheet.jpg" alt="Every setting of a brush on one sheet, its modes as pictures"><br><b>A whole brush on one sheet</b></td>
<td><img width="420" src="docs/readme/mixing.jpg" alt="Blue smudged into yellow in RGB, OKLab and as pigment, which turns green"><br><b>Paint that mixes like paint</b></td>
</tr>
<tr>
<td><img width="420" src="docs/readme/guides.jpg" alt="The guides tool: line, perspective, ellipse and curve on the top bar"><br><b>Guides</b></td>
<td><img width="420" src="docs/readme/make-brush.jpg" alt="A brush made from a selection of the picture"><br><b>A brush from any selection</b></td>
</tr>
</table>

The code was written by an LLM, on top of Krita's code. All the UI designs were done manually.

[Support Krita](https://krita.org/support-us/donations)

### [Install](../../releases)
- unzip
- launch `bin\krita.exe`

> [!NOTE]
> Skissa uses separate settings files from Krita, so your Krita settings stay unchanged and can be imported into Skissa.

### Philosophy Behind It
I've always wondered why the UI of so much software is so cluttered, for example you know those menu bars, where you have a bunch of different items like: `File`, `Edit`, `View`, `Help`... and to actually find what you want you have to go through them; like why? clearly if someone were to make an ideal UI, you would not need to search for anything, it would all intuitively be available in front of you; so I made Skissa to demonstrate my ideas of what an intuitive UI might look like, I have reworked the UI from scratch, based on first principles, meaning forgetting everything about previous conceptions of how UI is done and designing from fundamentals, like the way human perception works, what purpose people have when they open the canvas, how frequent certain actions are, so that we can minimize the amount of effort needed for using them, whilst keeping the screen as clear as possible so that the canvas is the center of attention. I've made every UI customizable to accommodate different styles, you can drag almost any command onto the top bar to add it as a button. This is not supposed to be a product or anything, it is a demonstration of ideas for intuitive UI design, in hopes that maybe someone might get inspired by some UI design choices, or give feedback.

---

An independent fork of [Krita](https://krita.org), modified by GeorgeAzma since 2026. Not affiliated with or endorsed by the Krita Foundation or KDE; Krita is a trademark of the Stichting Krita Foundation. GPL-3.0-or-later, like Krita. Screenshot art: "Kiki Paints Over the Waves" by Tyson Tan, Krita's splash, [CC BY-SA 4.0](https://tysontan.com/gallery/gallery-mascots/kiki-splash-2026/). See [NOTICE](NOTICE).
