#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "gettext.h"
#include <zip.h>
#include <time.h>

#include "dive.h"
#include "file.h"

/* Crazy windows sh*t */
#ifndef O_BINARY
#define O_BINARY 0
#endif

int readfile(const char *filename, struct memblock *mem)
{
	int ret, fd;
	struct stat st;
	char *buf;

	mem->buffer = NULL;
	mem->size = 0;

	fd = open(filename, O_RDONLY | O_BINARY, 0);
	if (fd < 0)
		return fd;
	ret = fstat(fd, &st);
	if (ret < 0)
		goto out;
	ret = -EINVAL;
	if (!S_ISREG(st.st_mode))
		goto out;
	ret = 0;
	if (!st.st_size)
		goto out;
	buf = malloc(st.st_size+1);
	ret = -1;
	errno = ENOMEM;
	if (!buf)
		goto out;
	mem->buffer = buf;
	mem->size = st.st_size;
	ret = read(fd, buf, mem->size);
	if (ret < 0)
		goto free;
	buf[ret] = 0;
	if (ret == mem->size)
		goto out;
	errno = EIO;
	ret = -1;
free:
	free(mem->buffer);
	mem->buffer = NULL;
	mem->size = 0;
out:
	close(fd);
	return ret;
}


static void zip_read(struct zip_file *file, char **error, const char *filename)
{
	int size = 1024, n, read = 0;
	char *mem = malloc(size);

	while ((n = zip_fread(file, mem+read, size-read)) > 0) {
		read += n;
		size = read * 3 / 2;
		mem = realloc(mem, size);
	}
	mem[read] = 0;
	parse_xml_buffer(filename, mem, read, &dive_table, NULL, error);
	free(mem);
}

static int try_to_open_zip(const char *filename, struct memblock *mem, char **error)
{
	int success = 0;
	/* Grr. libzip needs to re-open the file, it can't take a buffer */
	struct zip *zip = zip_open(filename, ZIP_CHECKCONS, NULL);

	if (zip) {
		int index;
		for (index = 0; ;index++) {
			struct zip_file *file = zip_fopen_index(zip, index, 0);
			if (!file)
				break;
			zip_read(file, error, filename);
			zip_fclose(file);
			success++;
		}
		zip_close(zip);
	}
	return success;
}

static int try_to_xslt_open_csv(const char *filename, struct memblock *mem, char **error)
{
	char *buf;

	if (readfile(filename, mem) < 0) {
		if (error) {
			int len = strlen(translate("gettextFromC","Failed to read '%s'")) + strlen(filename);
			*error = malloc(len);
			snprintf(*error, len, translate("gettextFromC","Failed to read '%s'"), filename);
		}

		return 1;
	}

	/* Surround the CSV file content with XML tags to enable XSLT
	 * parsing
	 */
	buf = realloc(mem->buffer, mem->size + strlen("<csv></csv>"));
	if (buf != NULL) {
		memmove(buf + 5, mem->buffer, mem->size);
		memcpy(buf, "<csv>", 5);
		memcpy(mem->buffer + mem->size + 5, "</csv>", 7);
		mem->buffer = buf;
		mem->size += strlen("<csv></csv>");
	} else
		return 1;

	return 0;
}

static int try_to_open_db(const char *filename, struct memblock *mem, char **error)
{
	return parse_dm4_buffer(filename, mem->buffer, mem->size, &dive_table, error);
}

static timestamp_t parse_date(const char *date)
{
	int hour, min, sec;
	struct tm tm;
	char *p;

	memset(&tm, 0, sizeof(tm));
	tm.tm_mday = strtol(date, &p, 10);
	if (tm.tm_mday < 1 || tm.tm_mday > 31)
		return 0;
	for (tm.tm_mon = 0; tm.tm_mon < 12; tm.tm_mon++) {
		if (!memcmp(p, monthname(tm.tm_mon), 3))
			break;
	}
	if (tm.tm_mon > 11)
		return 0;
	date = p+3;
	tm.tm_year = strtol(date, &p, 10);
	if (date == p)
		return 0;
	if (tm.tm_year < 70)
		tm.tm_year += 2000;
	if (tm.tm_year < 100)
		tm.tm_year += 1900;
	if (sscanf(p, "%d:%d:%d", &hour, &min, &sec) != 3)
		return 0;
	tm.tm_hour = hour;
	tm.tm_min = min;
	tm.tm_sec = sec;
	return utc_mktime(&tm);
}

enum csv_format {
	CSV_DEPTH, CSV_TEMP, CSV_PRESSURE
};

static void add_sample_data(struct sample *sample, enum csv_format type, double val)
{
	switch (type) {
	case CSV_DEPTH:
		sample->depth.mm = feet_to_mm(val);
		break;
	case CSV_TEMP:
		sample->temperature.mkelvin = F_to_mkelvin(val);
		break;
	case CSV_PRESSURE:
		sample->cylinderpressure.mbar = psi_to_mbar(val*4);
		break;
	}
}

/*
 * Cochran comma-separated values: depth in feet, temperature in F, pressure in psi.
 *
 * They start with eight comma-separated fields like:
 *
 *   filename: {C:\Analyst4\can\T036785.can},{C:\Analyst4\can\K031892.can}
 *   divenr: %d
 *   datetime: {03Sep11 16:37:22},{15Dec11 18:27:02}
 *   ??: 1
 *   serialnr??: {CCI134},{CCI207}
 *   computer??: {GeminiII},{CommanderIII}
 *   computer??: {GeminiII},{CommanderIII}
 *   ??: 1
 *
 * Followed by the data values (all comma-separated, all one long line).
 */
static int try_to_open_csv(const char *filename, struct memblock *mem, enum csv_format type)
{
	char *p = mem->buffer;
	char *header[8];
	int i, time;
	timestamp_t date;
	struct dive *dive;
	struct divecomputer *dc;

	for (i = 0; i < 8; i++) {
		header[i] = p;
		p = strchr(p, ',');
		if (!p)
			return 0;
		p++;
	}

	date = parse_date(header[2]);
	if (!date)
		return 0;

	dive = alloc_dive();
	dive->when = date;
	dive->number = atoi(header[1]);
	dc = &dive->dc;

	time = 0;
	for (;;) {
		char *end;
		double val;
		struct sample *sample;

		errno = 0;
		val = strtod(p,&end); // FIXME == localization issue
		if (end == p)
			break;
		if (errno)
			break;

		sample = prepare_sample(dc);
		sample->time.seconds = time;
		add_sample_data(sample, type, val);
		finish_sample(dc);

		time++;
		dc->duration.seconds = time;
		if (*end != ',')
			break;
		p = end+1;
	}
	record_dive(dive);
	return 1;
}

static int open_by_filename(const char *filename, const char *fmt, struct memblock *mem, char **error)
{
	/* Suunto Dive Manager files: SDE, ZIP; divelogs.de files: DLD */
	if (!strcasecmp(fmt, "SDE") || !strcasecmp(fmt, "ZIP") || !strcasecmp(fmt, "DLD"))
		return try_to_open_zip(filename, mem, error);

	/* CSV files */
	if (!strcasecmp(fmt, "CSV")) {
		int len = strlen(translate("gettextFromC","Failed to read '%s'. Use import for CSV files.")) + strlen(filename);
		*error = malloc(len);
		snprintf(*error, len, translate("gettextFromC","Failed to read '%s'. Use import for CSV files."), filename);
		return 1;
	}

#if ONCE_COCHRAN_IS_SUPPORTED
	/* Truly nasty intentionally obfuscated Cochran Anal software */
	if (!strcasecmp(fmt, "CAN"))
		return try_to_open_cochran(filename, mem, error);
#endif

	/* Cochran export comma-separated-value files */
	if (!strcasecmp(fmt, "DPT"))
		return try_to_open_csv(filename, mem, CSV_DEPTH);
	if (!strcasecmp(fmt, "TMP"))
		return try_to_open_csv(filename, mem, CSV_TEMP);
	if (!strcasecmp(fmt, "HP1"))
		return try_to_open_csv(filename, mem, CSV_PRESSURE);

	return 0;
}

static void parse_file_buffer(const char *filename, struct memblock *mem, char **error)
{
	char *fmt = strrchr(filename, '.');
	if (fmt && open_by_filename(filename, fmt+1, mem, error))
		return;

	parse_xml_buffer(filename, mem->buffer, mem->size, &dive_table, NULL, error);
}

void parse_file(const char *filename, char **error)
{
	struct memblock mem;
	char *fmt;

	if (readfile(filename, &mem) < 0) {
		/* we don't want to display an error if this was the default file */
		if (prefs.default_filename && ! strcmp(filename, prefs.default_filename))
			return;

		if (error) {
			int len = strlen(translate("gettextFromC","Failed to read '%s'")) + strlen(filename);
			*error = malloc(len);
			snprintf(*error, len, translate("gettextFromC","Failed to read '%s'"), filename);
		}

		return;
	}

	fmt = strrchr(filename, '.');
	if (fmt && (!strcasecmp(fmt + 1, "DB") || !strcasecmp(fmt + 1, "BAK"))) {
		if (!try_to_open_db(filename, &mem, error)) {
			free(mem.buffer);
			return;
		}
	}

	parse_file_buffer(filename, &mem, error);
	free(mem.buffer);
}

#define MAXCOLDIGITS 3
#define MAXCOLS 100
void parse_csv_file(const char *filename, int timef, int depthf, int tempf, char **error)
{
	struct memblock mem;
	char *params[11];
	char timebuf[MAXCOLDIGITS];
	char depthbuf[MAXCOLDIGITS];
	char tempbuf[MAXCOLDIGITS];
	time_t now;
	struct tm *timep;
	char curdate[9];
	char curtime[6];

	if (timef >= MAXCOLS || depthf >= MAXCOLS || tempf >= MAXCOLS) {
		int len = strlen(translate("gettextFromC", "Maximum number of supported columns on CSV import is %d")) + MAXCOLDIGITS;
		*error = malloc(len);
		snprintf(*error, len, translate("gettextFromC", "Maximum number of supported columns on CSV import is %d"), MAXCOLS);

		return;
	}
	snprintf(timebuf, MAXCOLDIGITS, "%d", timef);
	snprintf(depthbuf, MAXCOLDIGITS, "%d", depthf);
	snprintf(tempbuf, MAXCOLDIGITS, "%d", tempf);
	time(&now);
	timep = localtime(&now);
	strftime(curdate, sizeof(curdate), "%Y%m%d", timep);

	/* As the parameter is numeric, we need to ensure that the leading zero
	* is not discarded during the transform, thus prepend time with 1 */
	strftime(curtime, sizeof(curtime), "1%H%M", timep);

	params[0] = "timeField";
	params[1] = timebuf;
	params[2] = "depthField";
	params[3] = depthbuf;
	params[4] = "tempField";
	params[5] = tempbuf;
	params[6] = "date";
	params[7] = curdate;
	params[8] = "time";
	params[9] = curtime;
	params[10] = NULL;

	if (filename == NULL)
		return;

	if (readfile(filename, &mem) < 0) {
		if (error) {
			int len = strlen(translate("gettextFromC","Failed to read '%s'")) + strlen(filename);
			*error = malloc(len);
			snprintf(*error, len, translate("gettextFromC","Failed to read '%s'"), filename);
		}

		return;
	}

	if (try_to_xslt_open_csv(filename, &mem, error))
		return;

	parse_xml_buffer(filename, mem.buffer, mem.size, &dive_table, (const char **)params, error);
	free(mem.buffer);
}
