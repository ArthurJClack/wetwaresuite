wetwaresuite - ready-to-run build
=================================

unzip this folder anywhere. nothing to install. the only thing that goes online
is a once-a-day update check from the app windows (turn it off in any app's Aa
menu, or run: wetwaresuite-files app.update_settings auto=false). see
docs/tutorials/updates.md.

open an app by double-clicking it:

  wetwaresuite-image        photos and painting (photo, paint)
  wetwaresuite-draw         vector, fonts, whiteboard (vector, type, board)
  wetwaresuite-model        3D model, sculpt, render, animate
  wetwaresuite-game         game assets and levels (forge, levels)
  wetwaresuite-cad          CAD, blueprints, 3D print slicer
  wetwaresuite-electronics  circuits, PCBs, firmware flashing
  wetwaresuite-sound        audio editing and beats
  wetwaresuite-video        video cutting and screen capture
  wetwaresuite-office       documents, page layout, notes
  wetwaresuite-maps         offline maps
  wetwaresuite-files        file tools, converting, library, automation
  wetwaresuite-gui          one window with every tool
  wetwaresuite              every tool, headless (batch scripts and MCP)
  wetwaresuite-hub          install manager
  wetwaresuite-share        share and live-sync files

you can also drag a file onto an app, or run it from a terminal:
  wetwaresuite-image open photo.png
  wetwaresuite help

first run warnings (the builds are not code-signed yet):

  windows: if "Windows protected your PC" appears, click "More info" then "Run anyway".
  macOS:   right-click the app, choose Open, then Open again. or run once in Terminal:
             xattr -dr com.apple.quarantine path/to/this/folder
  linux:   if double-click does nothing, run: chmod +x wetwaresuite-*

tutorials: https://github.com/ArthurJClack/wetwaresuite/tree/main/docs/tutorials
license: GPL-3.0 (see LICENSE)
