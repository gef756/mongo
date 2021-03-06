/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "test_util.i"

static struct {
	char *progname;				/* Program name */

	WT_CONNECTION *wt_conn;			/* WT_CONNECTION handle */
	WT_SESSION *wt_session;			/* WT_SESSION handle */

	char *config_open;			/* Command-line configuration */

	uint32_t c_cache;			/* Config values */
	uint32_t c_key_max;
	uint32_t c_ops;
	uint32_t c_k;				/* Number of hash iterations */
	uint32_t c_factor;			/* Number of bits per item */
	uint32_t c_srand;

	uint8_t **entries;
} g;

void cleanup(void);
void populate_entries(void);
void run(void);
void setup(void);
void usage(void);

extern char *__wt_optarg;
extern int __wt_optind;

int
main(int argc, char *argv[])
{
	int ch;

	if ((g.progname = strrchr(argv[0], DIR_DELIM)) == NULL)
		g.progname = argv[0];
	else
		++g.progname;

	/* Set default configuration values. */
	g.c_cache = 10;
	g.c_ops = 100000;
	g.c_key_max = 100;
	g.c_k = 8;
	g.c_factor = 16;
	g.c_srand = 3233456;

	/* Set values from the command line. */
	while ((ch = __wt_getopt(g.progname, argc, argv, "c:f:k:o:s:")) != EOF)
		switch (ch) {
		case 'c':			/* Cache size */
			g.c_cache = (u_int)atoi(__wt_optarg);
			break;
		case 'f':			/* Factor */
			g.c_factor = (u_int)atoi(__wt_optarg);
			break;
		case  'k':			/* Number of hash functions */
			g.c_k = (u_int)atoi(__wt_optarg);
			break;
		case 'o':			/* Number of ops */
			g.c_ops = (u_int)atoi(__wt_optarg);
			break;
		case 's':			/* Number of ops */
			g.c_srand = (u_int)atoi(__wt_optarg);
			break;
		default:
			usage();
		}

	argc -= __wt_optind;
	argv += __wt_optind;

	setup();
	run();
	cleanup();

	return (EXIT_SUCCESS);
}

void
setup(void)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	int ret;
	char config[512];

	if ((ret = system("rm -f WiredTiger* *.bf")) != 0)
		testutil_die(ret, "system cleanup call failed");

	/*
	 * This test doesn't test public Wired Tiger functionality, it still
	 * needs connection and session handles.
	 */

	/*
	 * Open configuration -- put command line configuration options at the
	 * end so they can override "standard" configuration.
	 */
	snprintf(config, sizeof(config),
	    "create,error_prefix=\"%s\",cache_size=%" PRIu32 "MB,%s",
	    g.progname, g.c_cache, g.config_open == NULL ? "" : g.config_open);

	if ((ret = wiredtiger_open(NULL, NULL, config, &conn)) != 0)
		testutil_die(ret, "wiredtiger_open");

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "connection.open_session");

	g.wt_conn = conn;
	g.wt_session = session;
	populate_entries();
}

void
run(void)
{
	WT_BLOOM *bloomp;
	WT_ITEM item;
	WT_SESSION_IMPL *sess;
	uint32_t fp, i;
	int ret;
	const char *uri = "file:my_bloom.bf";

	/* Use the internal session handle to access private APIs. */
	sess = (WT_SESSION_IMPL *)g.wt_session;

	if ((ret = __wt_bloom_create(
	    sess, uri, NULL, g.c_ops, g.c_factor, g.c_k, &bloomp)) != 0)
		testutil_die(ret, "__wt_bloom_create");

	item.size = g.c_key_max;
	for (i = 0; i < g.c_ops; i++) {
		item.data = g.entries[i];
		if ((ret = __wt_bloom_insert(bloomp, &item)) != 0)
			testutil_die(ret, "__wt_bloom_insert: %d", i);
	}

	if ((ret = __wt_bloom_finalize(bloomp)) != 0)
		testutil_die(ret, "__wt_bloom_finalize");

	for (i = 0; i < g.c_ops; i++) {
		item.data = g.entries[i];
		if ((ret = __wt_bloom_get(bloomp, &item)) != 0) {
			fprintf(stderr, "get failed at record: %d\n", i);
			testutil_die(ret, "__wt_bloom_get");
		}
	}
	if ((ret = __wt_bloom_close(bloomp)) != 0)
		testutil_die(ret, "__wt_bloom_close");

	if ((ret = g.wt_session->checkpoint(g.wt_session, NULL)) != 0)
		testutil_die(ret, "WT_SESSION.checkpoint");
	if ((ret = __wt_bloom_open(
	    sess, uri, g.c_factor, g.c_k, NULL, &bloomp)) != 0)
		testutil_die(ret, "__wt_bloom_open");
	for (i = 0; i < g.c_ops; i++) {
		item.data = g.entries[i];
		if ((ret = __wt_bloom_get(bloomp, &item)) != 0)
			testutil_die(ret, "__wt_bloom_get");
	}

	/*
	 * Try out some values we didn't insert - choose a different size to
	 * ensure the value doesn't overlap with existing values.
	 */
	item.size = g.c_key_max + 10;
	item.data = calloc(item.size, 1);
	memset((void *)item.data, 'a', item.size);
	for (i = 0, fp = 0; i < g.c_ops; i++) {
		((uint8_t *)item.data)[i % item.size] =
		    'a' + ((uint8_t)rand() % 26);
		if ((ret = __wt_bloom_get(bloomp, &item)) == 0)
			++fp;
	}
	free((void *)item.data);
	printf("Out of %d ops, got %d false positives, %.4f%%\n",
	    g.c_ops, fp, 100.0 * fp/g.c_ops);
	if ((ret = __wt_bloom_drop(bloomp, NULL)) != 0)
		testutil_die(ret, "__wt_bloom_drop");
}

void
cleanup(void)
{
	uint32_t i;
	int ret;

	for (i = 0; i < g.c_ops; i++)
		free(g.entries[i]);
	free(g.entries);
	if ((ret = g.wt_session->close(g.wt_session, NULL)) != 0)
		testutil_die(ret, "WT_SESSION.close");
	if ((g.wt_conn->close(g.wt_conn, NULL)) != 0)
		testutil_die(ret, "WT_CONNECTION.close");
}

/*
 * Create and keep all the strings used to populate the bloom filter, so that
 * we can do validation with the same set of entries.
 */
void
populate_entries(void)
{
	uint32_t i, j;
	uint8_t **entries;

	srand(g.c_srand);

	entries = calloc(g.c_ops, sizeof(uint8_t *));
	if (entries == NULL)
		testutil_die(ENOMEM, "key buffer malloc");

	for (i = 0; i < g.c_ops; i++) {
		entries[i] = calloc(g.c_key_max, sizeof(uint8_t));
		if (entries[i] == NULL)
			testutil_die(ENOMEM, "key buffer malloc 2");
		for (j = 0; j < g.c_key_max; j++)
			entries[i][j] = 'a' + ((uint8_t)rand() % 26);
	}

	g.entries = entries;
}

/*
 * usage --
 *	Display usage statement and exit failure.
 */
void
usage(void)
{
	fprintf(stderr, "usage: %s [-cfkos]\n", g.progname);
	fprintf(stderr, "%s",
	    "\t-c cache size\n"
	    "\t-f number of bits per item\n"
	    "\t-k size of entry strings\n"
	    "\t-o number of operations to perform\n"
	    "\t-s random seed for run\n");

	exit(EXIT_FAILURE);
}
