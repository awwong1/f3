#define _POSIX_C_SOURCE 200112L
#define _XOPEN_SOURCE 600

#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <err.h>
#include <sys/time.h>
#include <argp.h>

#include "utils.h"
#include "version.h"

/* Argp's global variables. */
const char *argp_program_version = "F3 Read " F3_STR_VERSION;

/* Arguments. */
static char adoc[] = "<PATH>";

static char doc[] = "F3 Read -- validate .h2w files to test "
	"the real capacity of the drive";

static struct argp_option options[] = {
	{"start-at",		's',	"NUM",		0,
		"First NUM.h2w file to be read",			1},
	{"end-at",		'e',	"NUM",		0,
		"Last NUM.h2w file to be read",				0},
	{"show-progress",	'p',	"NUM",		0,
		"Show progress if NUM is not zero",			0},
	{ 0 }
};

struct args {
	long        start_at;
	long        end_at;
	int	    show_progress;
	const char  *dev_path;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct args *args = state->input;
	long l;

	switch (key) {
	case 's':
		l = arg_to_long(state, arg);
		if (l <= 0)
			argp_error(state,
				"NUM must be greater than zero");
		args->start_at = l - 1;
		break;

	case 'e':
		l = arg_to_long(state, arg);
		if (l <= 0)
			argp_error(state,
				"NUM must be greater than zero");
		args->end_at = l - 1;
		break;

	case 'p':
		args->show_progress = !!arg_to_long(state, arg);
		break;

	case ARGP_KEY_INIT:
		args->dev_path = NULL;
		break;

	case ARGP_KEY_ARG:
		if (args->dev_path)
			argp_error(state,
				"Wrong number of arguments; only one is allowed");
		args->dev_path = arg;
		break;

	case ARGP_KEY_END:
		if (!args->dev_path)
			argp_error(state,
				"The disk path was not specified");
		if (args->start_at > args->end_at)
			argp_error(state,
				"Option --start-at must be less or equal to option --end-at");
		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, adoc, doc, NULL, NULL, NULL};

static inline void update_dt(struct timeval *dt, const struct timeval *t1,
	const struct timeval *t2)
{
	dt->tv_sec  += t2->tv_sec  - t1->tv_sec;
	dt->tv_usec += t2->tv_usec - t1->tv_usec;
	if (dt->tv_usec >= 1000000) {
		dt->tv_sec++;
		dt->tv_usec -= 1000000;
	}
}

#define TOLERANCE	2

#define PRINT_STATUS(s)	printf("%s%7" PRIu64 "/%9" PRIu64 "/%7" PRIu64 "/%7" \
	PRIu64, (s), *ptr_ok, *ptr_corrupted, *ptr_changed, *ptr_overwritten)

#define BLANK	"                                 "
#define CLEAR	("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b" \
		 "\b\b\b\b\b\b\b\b\b\b\b\b\b")

static void validate_file(const char *path, int number,
	uint64_t *ptr_ok, uint64_t *ptr_corrupted, uint64_t *ptr_changed,
	uint64_t *ptr_overwritten, uint64_t *ptr_size, int *ptr_read_all,
	struct timeval *ptr_dt, int progress)
{
	char *full_fn;
	const char *filename;
	const int num_int64 = SECTOR_SIZE >> 3;
	uint64_t sector[num_int64];
	FILE *f;
	int fd;
	size_t sectors_read;
	uint64_t expected_offset;
	int final_errno;
	struct timeval t1, t2;
	/* Progress time. */
	struct timeval pt1 = { .tv_sec = -1000, .tv_usec = 0 };

	*ptr_ok = *ptr_corrupted = *ptr_changed = *ptr_overwritten = 0;

	full_fn = full_fn_from_number(&filename, path, number);
	assert(full_fn);
	printf("Validating file %s ... %s", filename, progress ? BLANK : "");
	fflush(stdout);
#ifdef __CYGWIN__
	/* We don't need write access, but some kernels require that
	 * the file descriptor passed to fdatasync(2) to be writable.
	 */
	f = fopen(full_fn, "rb+");
#else
	f = fopen(full_fn, "rb");
#endif
	if (!f)
		err(errno, "Can't open file %s", full_fn);
	fd = fileno(f);
	assert(fd >= 0);

	/* If the kernel follows our advice, f3read won't ever read from cache
	 * even when testing small memory cards without a remount, and
	 * we should have a better reading-speed measurement.
	 */
	assert(!fdatasync(fd));
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));

	/* Obtain initial time. */
	assert(!gettimeofday(&t1, NULL));
	/* Help the kernel to help us. */
	assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL));

	sectors_read = fread(sector, SECTOR_SIZE, 1, f);
	final_errno = errno;
	expected_offset = (uint64_t)number * GIGABYTES;
	while (sectors_read > 0) {
		uint64_t rn;
		int error_count, i;

		assert(sectors_read == 1);

		rn = sector[0];
		error_count = 0;
		for (i = 1; error_count <= TOLERANCE && i < num_int64; i++) {
			rn = random_number(rn);
			if (rn != sector[i])
				error_count++;
		}

		if (expected_offset == sector[0]) {
			if (error_count == 0)
				(*ptr_ok)++;
			else if (error_count <= TOLERANCE)
				(*ptr_changed)++;
			else
				(*ptr_corrupted)++;
		} else if (error_count <= TOLERANCE)
			(*ptr_overwritten)++;
		else
			(*ptr_corrupted)++;

		sectors_read = fread(sector, SECTOR_SIZE, 1, f);
		final_errno = errno;
		expected_offset += SECTOR_SIZE;

		if (progress) {
			struct timeval pt2;
			assert(!gettimeofday(&pt2, NULL));
			/* Avoid often printouts. */
			if (delay_ms(&pt1, &pt2) >= 200) {
				PRINT_STATUS(CLEAR);
				fflush(stdout);
				pt1 = pt2;
			}
		}
	}
	assert(!gettimeofday(&t2, NULL));
	update_dt(ptr_dt, &t1, &t2);

	*ptr_read_all = feof(f);
	*ptr_size = ftell(f);

	PRINT_STATUS(progress ? CLEAR : "");
	if (!*ptr_read_all) {
		assert(ferror(f));
		printf(" - NOT fully read due to \"%s\"",
			strerror(final_errno));
	}
	printf("\n");

	fclose(f);
	free(full_fn);
}

static void report(const char *prefix, uint64_t i)
{
	double f = (double) (i * SECTOR_SIZE);
	const char *unit = adjust_unit(&f);
	printf("%s %.2f %s (%" PRIu64 " sectors)\n", prefix, f, unit, i);
}

static inline double dt_to_s(struct timeval *dt)
{
	double ret = (double)dt->tv_sec + ((double)dt->tv_usec / 1000000.);
	assert(ret >= 0);
	return ret > 0 ? ret : 1;
}

static void iterate_files(const char *path, const long *files,
	long start_at, long end_at, int progress)
{
	uint64_t tot_ok, tot_corrupted, tot_changed, tot_overwritten, tot_size;
	struct timeval tot_dt = { .tv_sec = 0, .tv_usec = 0 };
	double read_speed;
	const char *unit;
	int and_read_all = 1;
	int or_missing_file = 0;
	long number = start_at;

	UNUSED(end_at);

	tot_ok = tot_corrupted = tot_changed = tot_overwritten = tot_size = 0;
	printf("                  SECTORS "
		"     ok/corrupted/changed/overwritten\n");

	while (*files >= 0) {
		uint64_t sec_ok, sec_corrupted, sec_changed,
			sec_overwritten, file_size;
		int read_all;

		or_missing_file = or_missing_file || (*files != number);
		for (; number < *files; number++) {
			char *full_fn;
			const char *filename;
			full_fn = full_fn_from_number(&filename, "", number);
			assert(full_fn);
			printf("Missing file %s\n", filename);
			free(full_fn);
		}
		number++;

		validate_file(path, *files, &sec_ok, &sec_corrupted,
			&sec_changed, &sec_overwritten,
			&file_size, &read_all, &tot_dt, progress);
		tot_ok += sec_ok;
		tot_corrupted += sec_corrupted;
		tot_changed += sec_changed;
		tot_overwritten += sec_overwritten;
		tot_size += file_size;
		and_read_all = and_read_all && read_all;
		files++;
	}
	assert(tot_size / SECTOR_SIZE ==
		(tot_ok + tot_corrupted + tot_changed + tot_overwritten));

	/* Notice that not reporting `missing' files after the last file
	 * in @files is important since @end_at could be very large.
	 */

	report("\n  Data OK:", tot_ok);
	report("Data LOST:", tot_corrupted + tot_changed + tot_overwritten);
	report("\t       Corrupted:", tot_corrupted);
	report("\tSlightly changed:", tot_changed);
	report("\t     Overwritten:", tot_overwritten);
	if (or_missing_file)
		printf("WARNING: Not all F3 files in the range %li to %li are available\n",
			start_at + 1, number);
	if (!and_read_all)
		printf("WARNING: Not all data was read due to I/O error(s)\n");

	/* Reading speed. */
	read_speed = (double)tot_size / dt_to_s(&tot_dt);
	unit = adjust_unit(&read_speed);
	printf("Average reading speed: %.2f %s/s\n", read_speed, unit);
}

int main(int argc, char **argv)
{
	const long *files;

	struct args args = {
		/* Defaults. */
		.start_at	= 0,
		.end_at		= LONG_MAX - 1,
		/* If stdout isn't a terminal, supress progress. */
		.show_progress	= isatty(STDOUT_FILENO),
	};

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);
	print_header(stdout, "read");

	files = ls_my_files(args.dev_path, args.start_at, args.end_at);

	iterate_files(args.dev_path, files, args.start_at, args.end_at,
		args.show_progress);
	free((void *)files);
	return 0;
}
