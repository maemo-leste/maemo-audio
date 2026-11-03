# The PipeWire side: the control module.
#
# It links the mixer rather than copying from it, because the ladder and its
# cache live there.  The core also dlopens the mixer through the SPA loader
# redirect, and the linker dedupes the two by soname, so the process ends up
# with exactly one copy of each.

shared_module('pipewire-module-maemo-mix',
  [
    'module-maemo-mix.c',
    'maemo-hw-ctl.c',
    'maemo-route.c',
  ],
  dependencies: [mixer_dep, pipewire_dep, spa_dep, alsa_dep, math_dep],
  install: true,
  install_dir: pipewire_moduledir,
  # The mixer is not on the ordinary library path, it is a plugin sitting in
  # the SPA tree.
  install_rpath: spa_plugindir / 'maemo-flat-volume')
