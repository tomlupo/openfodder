// Open Fodder web build: this runs inside the Emscripten runtime (--pre-js), where FS and IDBFS live.

// Saved games go to /Saves. Keep that folder in IndexedDB so saves survive reloads and relaunches
// from the Home Screen; autoPersist writes each change back as it happens.
Module['preRun'] = [].concat(Module['preRun'] || []);
Module['preRun'].push(function () {
  try {
    FS.mkdir('/Saves');
  } catch (e) {
    // The data package already made it
  }
  try {
    FS.mount(IDBFS, { autoPersist: true }, '/Saves');
  } catch (e) {
    console.warn('Open Fodder: saves will not survive a reload (' + e + ')');
    return;
  }

  addRunDependency('of-saves');
  FS.syncfs(true, function (err) {
    if (err) console.warn('Open Fodder: could not read saved games (' + err + ')');
    removeRunDependency('of-saves');
  });
});
