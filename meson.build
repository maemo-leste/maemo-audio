# maemo-audio
#
# Two artifacts.  The mixer is an SPA plugin, a mixer with a volume of its
# own; the module is the PipeWire side that drives it and owns the hardware
# handoff.
#
# Both install directories come from the pkg-config files of the projects we
# plug into, so neither a multiarch change nor a prefix change is something
# this build has to know about.

project('maemo-audio', 'c',
  version: '1.0',
  # gnu11, not c11: spa/utils/string.h uses locale_t and newlocale, which a
  # strict -std=c11 hides behind __STRICT_ANSI__.
  #
  # warning_level 1, which is -Wall. Level 2 adds -Wextra, and all it adds here
  # is unused parameter in the SPA callbacks and missing field initializer in
  # the positional struct initializers, both of which are the shape of the API
  # rather than a defect.
  default_options: ['c_std=gnu11', 'warning_level=1'])

cc = meson.get_compiler('c')

# The maemo build of PipeWire: the mixer needs the node methods that carry
# port mix info, which arrived with 1.6.
pipewire_dep = dependency('libpipewire-0.3', version: '>= 1.6.0')
# libspa-0.2.pc carries its own ABI version (0.2), not the PipeWire release,
# so the release check above is the one that means anything.
spa_dep      = dependency('libspa-0.2')
alsa_dep     = dependency('alsa')
math_dep     = cc.find_library('m')

# Where the two loaders look.  The mixer is one level below the SPA plugin
# directory because the SPA loader is redirected into that tree, see
# 55-spalib-redirect.conf in the package.
spa_plugindir      = spa_dep.get_variable(pkgconfig: 'plugindir')
pipewire_moduledir = pipewire_dep.get_variable(pkgconfig: 'moduledir')

subdir('mixer')
subdir('module')
