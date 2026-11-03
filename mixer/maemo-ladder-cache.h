/* Cache of parsed tuning ladders, keyed on the curve itself.
 *
 * The ladders are static config. They are published on the sink node by
 * WirePlumber rules and changed only by editing a file and restarting;
 * nothing at runtime alters one. So a parsed ladder never needs
 * invalidating, and re-parsing the same string is pure waste -- yet
 * before this existed every call state change and every sink appearance
 * rebuilt the whole map, including a pow() per entry.
 *
 * Keying on the curve rather than on the device that published it means
 * two devices sharing a curve share one map. On droid4 the third
 * device's media and in-call ladders are byte-identical, so that swap
 * costs nothing at all.
 *
 * The cache owns every map it builds. Callers borrow and must not free.
 * Borrowed pointers stay valid for the life of the cache, which is the
 * life of the module -- there is no eviction, because there is nothing
 * to evict: the working set is a handful of curves.
 */
#ifndef MAEMO_LADDER_CACHE_H
#define MAEMO_LADDER_CACHE_H

#include <stddef.h>
#include "maemo-export.h"

struct maemo_ladder_cache;
struct maemo_vol_map;

MAEMO_EXPORT struct maemo_ladder_cache *maemo_ladder_cache_new(void);
MAEMO_EXPORT void maemo_ladder_cache_free(struct maemo_ladder_cache *c);

/* The map for this curve: parsed on first request, handed back from
 * cache afterwards. The slider and the ladder are both built from the
 * same table, because one entry per hardware position is the property
 * the tuning file is written to satisfy -- it is what makes no slider
 * position dead.
 *
 * Returns NULL if the csv cannot be turned into a valid ladder (empty,
 * or not ascending). That is a configuration fault, distinct from a
 * cache miss, and the caller decides how loud to be about it. Failures
 * are deliberately not cached: a broken ladder that keeps being asked
 * for should keep being visible rather than silently remembered. */
MAEMO_EXPORT const struct maemo_vol_map *
maemo_ladder_cache_get(struct maemo_ladder_cache *c, const char *csv);

/* Diagnostics. entries is how many distinct curves are held; parses is
 * how many requests actually cost a parse, so hits == requests - parses. */
MAEMO_EXPORT unsigned maemo_ladder_cache_entries(const struct maemo_ladder_cache *c);
unsigned maemo_ladder_cache_hits(const struct maemo_ladder_cache *c);
MAEMO_EXPORT unsigned maemo_ladder_cache_parses(const struct maemo_ladder_cache *c);

/* Parse a comma-separated list of centibel values into a fresh array.
 * Public because the module also needs it for its own configured
 * tables, and the tolerance rules (stray quotes, trailing commas,
 * whitespace) belong in one place.
 *
 * Returns a malloc'd array the caller owns, or NULL if nothing parsed.
 * *count is set only on success. */
double *maemo_vol_parse_cb_csv(const char *csv, unsigned *count);

#endif /* MAEMO_LADDER_CACHE_H */
