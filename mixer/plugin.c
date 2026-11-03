/* Maemo mixer: N inputs, one output, one level.
 *
 * SPDX-License-Identifier: MIT
 *
 * The stock DSP mixer has the right topology (512 dynamic inputs, F32 mono
 * planes) but no level of its own: it declares port properties, resets them
 * and then never reads them, so nothing can be written anywhere. This is that
 * topology with a working volume attached -- one gain, applied to the sum,
 * set on the node.
 *
 * Exported under two names:
 *   audio.mixer.dsp          the name a context.spa-libs redirect targets,
 *                            which makes this the core's per-port mix handle
 *   audio.maemo.mixer        for explicit loading and node creation
 *
 * The .so is still called libspa-maemo-flat-volume.so. That name describes a
 * job this plugin no longer does; renaming it means changing the spa-libs
 * redirect and the install paths, which is its own change.
 */
#include <spa/support/plugin.h>
#include <spa/support/log.h>

extern const struct spa_handle_factory spa_maemo_flat_volume_factory;
extern const struct spa_handle_factory spa_maemo_flat_volume_named_factory;

SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_maemo_flat_volume_factory;
		break;
	case 1:
		*factory = &spa_maemo_flat_volume_named_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
