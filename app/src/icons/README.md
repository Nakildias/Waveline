# Icons

These are [Tabler Icons](https://github.com/tabler/tabler-icons), © 2020-2026
Paweł Kuna, MIT licensed. The full licence text is in
[`LICENSES/TablerIcons-MIT.txt`](../../../LICENSES/TablerIcons-MIT.txt) at the
root of this repository, and MIT requires it to accompany any copy of these
files, including binaries they are compiled into.

Unlike the other third-party pieces this project uses, these files are
*redistributed*: they are committed here and baked into `waveline-mixer` by the Qt
resource system (see `qt_add_resources` in `app/CMakeLists.txt`). Do not remove
the licence file.

The SVGs are upstream's, unmodified. They stroke with `currentColor`, which Qt
does not understand, so `Theme::iconPixmap()` substitutes a real colour into the
markup before handing it to QSvgRenderer.

