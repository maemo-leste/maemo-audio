#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include "maemo-ladder-cache.h"
#include "maemo-vol-map.h"

struct entry {
	struct entry *next;
	char *csv;
	struct maemo_vol_map *map;
};

struct maemo_ladder_cache {
	struct entry *head;
	unsigned hits;
	unsigned parses;
};

/* Parse a comma-separated list of centibel values.
 *
 * Deliberately forgiving about punctuation and strict about numbers.
 * The string arrives from a config file or a metadata value, where a
 * stray quote or a trailing comma is a typo nobody should lose a volume
 * curve over. But a token that is not a number at all is skipped rather
 * than guessed at, because inventing a level is worse than dropping one.
 *
 * This tolerance is why the parser lives here rather than being inlined
 * at each call site: the applet, the module's own configured tables and
 * the cache all need the exact same rules, and a divergence between
 * them would mean the same file meaning two different things. */
double *maemo_vol_parse_cb_csv(const char *csv, unsigned *count)
{
	unsigned cap = 16, n = 0;
	double *v = malloc(cap * sizeof(*v));
	const char *p = csv;

	if (v == NULL || csv == NULL)
		return NULL;

	while (*p != '\0') {
		char *end;
		double d;

		while (*p == ',' || *p == ' ' || *p == '"' || *p == '\'' ||
		       *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (*p == '\0')
			break;

		errno = 0;
		d = strtod(p, &end);
		if (end == p) {
			/* Not a number where one was expected. Skip one
			 * character and keep going so a single stray does
			 * not lose the whole table. */
			p++;
			continue;
		}
		if (errno == ERANGE) {
			p = end;
			continue;
		}

		if (n == cap) {
			double *grown = realloc(v, cap * 2 * sizeof(*v));
			if (grown == NULL) {
				free(v);
				return NULL;
			}
			v = grown;
			cap *= 2;
		}
		v[n++] = d;
		p = end;
	}

	if (n == 0) {
		free(v);
		return NULL;
	}
	*count = n;
	return v;
}

struct maemo_ladder_cache *maemo_ladder_cache_new(void)
{
	return calloc(1, sizeof(struct maemo_ladder_cache));
}

void maemo_ladder_cache_free(struct maemo_ladder_cache *c)
{
	struct entry *e;

	if (c == NULL)
		return;

	e = c->head;
	while (e != NULL) {
		struct entry *next = e->next;
		if (e->map != NULL)
			maemo_vol_map_free(e->map);
		free(e->csv);
		free(e);
		e = next;
	}
	free(c);
}

static struct entry *find(struct maemo_ladder_cache *c, const char *csv)
{
	for (struct entry *e = c->head; e != NULL; e = e->next)
		if (strcmp(e->csv, csv) == 0)
			return e;
	return NULL;
}

const struct maemo_vol_map *
maemo_ladder_cache_get(struct maemo_ladder_cache *c, const char *csv)
{
	struct entry *e;
	double *cb;
	unsigned n = 0;
	struct maemo_vol_map *map;

	if (c == NULL || csv == NULL)
		return NULL;

	e = find(c, csv);
	if (e != NULL) {
		c->hits++;
		return e->map;
	}

	cb = maemo_vol_parse_cb_csv(csv, &n);
	if (cb == NULL)
		return NULL;

	/* The slider table and the hardware ladder are the same array:
	 * one entry per hardware position, so the mapping between them
	 * is the identity. That is not a shortcut, it is the property
	 * the tuning file is authored to satisfy. */
	/* One cB list serves both halves, but the slider gets the silence step
 * prepended and the hardware ladder does not. See the note on
 * maemo_vol_map_new_from_cb_silence: silence is amplitude 0, not a low
 * centibel value, and without it the bottom of the slider sits at the
 * ladder's lowest hardware level (-4000 cB = -13.3 dBFS) and music keeps
 * playing. */
	map = maemo_vol_map_new_from_cb_silence(cb, n);
	free(cb);

	if (map == NULL)
		return NULL;

	e = calloc(1, sizeof(*e));
	if (e == NULL) {
		/* Out of memory for the index. Hand back the map rather
		 * than dropping it, but it will be rebuilt next time --
		 * correctness over caching under pressure. */
		c->parses++;
		return map;
	}

	e->csv = strdup(csv);
	if (e->csv == NULL) {
		free(e);
		c->parses++;
		return map;
	}
	e->map = map;

	e->next = c->head;
	c->head = e;
	c->parses++;

	return map;
}

unsigned maemo_ladder_cache_entries(const struct maemo_ladder_cache *c)
{
	unsigned n = 0;
	for (const struct entry *e = c ? c->head : NULL; e != NULL; e = e->next)
		n++;
	return n;
}

unsigned maemo_ladder_cache_hits(const struct maemo_ladder_cache *c)
{
	return c ? c->hits : 0;
}

unsigned maemo_ladder_cache_parses(const struct maemo_ladder_cache *c)
{
	return c ? c->parses : 0;
}
