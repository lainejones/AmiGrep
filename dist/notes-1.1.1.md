A full menu bar and settings for AmiGrepGUI. Searching works as before; the CLI `AmiGrep` is unchanged (still 1.1).

**What's new in AmiGrepGUI**
- **Menus** (right mouse button): Project (About, Iconify, Quit), Search (Search, Stop, Choose drawer, Clear results), Result (View, Open drawer) and Settings, with Amiga-key shortcuts. About shows the version and the licence.
- **Result > View** runs the viewer on the picked hit's file; **Result > Open drawer** opens its drawer in Workbench (needs workbench.library v44, OS 3.5+), like a double-click.
- **Settings > Search defaults...**: what Find, In, Files and Case hold at start. **Use current** takes what is in the main window now.
- **Settings > Programs...**: the viewer View runs (default `SYS:Utilities/MultiView`).
- Each page has **Save** (`ENV:` and `ENVARC:AmiGrep.prefs`), **Use** (`ENV:` only, until reboot), **Defaults** and **Cancel**.
- **Settings > Snapshot window** saves the window's place and size; **Unsnapshot window** goes back to the default at the next start.
- The window is laid out from the screen font and fits a 640x200 screen with Topaz 8. It opens at most 520 pixels wide and can be resized; if the screen font is too big for the layout it uses Topaz 8. The status line is cut to fit the window.
- After a resize, the window is laid out again once the messages already queued have been handled, instead of in the middle of them.
- AmiGrep is GPL-3.0; the `LICENSE` file is in the drawer.

Use the **.lha**: it keeps the executable bit, which a .zip can't store (see the README).
