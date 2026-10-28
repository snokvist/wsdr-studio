// SPDX-License-Identifier: GPL-2.0-only
// The page's only bridge to the shell: it asks for the session token (never in a URL or on a command
// line), and keeps its preferences in a file of the app's (not in the page's origin, which includes
// the backend's port). Runs sandboxed with context isolation.
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('wsdr', {
  session: () => ipcRenderer.invoke('wsdr:session'),
  prefs: {
    load: () => ipcRenderer.sendSync('wsdr:prefs-load'),
    save: (s) => ipcRenderer.send('wsdr:prefs-save', s),
  },
});
