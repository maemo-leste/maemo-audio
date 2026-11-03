#ifndef MAEMO_EXPORT_H
#define MAEMO_EXPORT_H

/* What this library hands to the rest of the process.
 *
 * The mixer is built -fvisibility=hidden: it is dlopened into pipewire,
 * where a bare name like port_set_format or clear_buffers is something
 * somebody else already has, and the node internals (mixer_*) have no
 * business being visible at all.
 *
 * What does have to be visible is the maemo_* control layer. The control
 * module in module/ resolves those symbols against this .so when it is
 * loaded, so a public function that is not marked is not a slightly
 * smaller API -- it is a module that fails to dlopen with "no such file
 * or directory" and nothing in the log to say why.
 *
 * So: mark everything the module calls, hide the rest, and when a new
 * entry point is added to one of these headers, mark it here or the
 * module will not load. The list is exactly what
 * `nm -D --undefined-only libpipewire-module-maemo-mix.so` asks for. */
#define MAEMO_EXPORT __attribute__((visibility("default")))

#endif /* MAEMO_EXPORT_H */
