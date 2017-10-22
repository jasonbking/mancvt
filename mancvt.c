/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <err.h>
#include <errno.h>
#include <regex.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/debug.h>

#define	CHUNK_SZ	(128)

typedef struct input {
	char **lines;
	size_t nlines;
	size_t alloc;
} input_t;

static const char r_xrstr[] =
   "\\\\fB\\([.A-Za-z0-9_-]\\{1,\\}\\)\\\\fR(\\([1-9][A-Z]*\\))";
static regex_t r_xr; /* cross reference */

static void *zalloc(size_t);
static char *xasprintf(const char *, ...);
static void input_resv(input_t *, size_t);
static void insert_line(input_t *, size_t, char *);
static void split_line(input_t *, size_t, size_t);
static void delete_line(input_t *, size_t);
static input_t *read_file(const char *);
static boolean_t starts_with(const char *, const char *);
static void cross_references(input_t *);
static void split_paragraphs(input_t *);
static void simple(input_t *);

int
main(int argc, const char **argv)
{
	input_t *in = NULL;

	fprintf(stderr, "%s\n", r_xrstr);
	VERIFY0(regcomp(&r_xr, r_xrstr, 0));

	in = read_file(argv[1]);

	cross_references(in);
	split_paragraphs(in);
	simple(in);

	for (size_t i = 0; i < in->nlines; i++)
		(void) printf("%s", in->lines[i]);
	(void) fputc('\n', stdout);

	return (0);
}

/* New sentence, new line */
static void
split_paragraphs(input_t *in)
{
	boolean_t skip = B_FALSE;

	for (size_t i = 0; i < in->nlines; i++) {
		char *line = in->lines[i];
		char *p = NULL;

		if (skip) {
			if (starts_with(line, ".fi"))
				skip = B_FALSE;
			continue;
		}

		if (line[0] == '\0')
			continue;

		if (starts_with(line, ".nf")) {
			skip = B_TRUE;
			continue;
		}

		if (starts_with(line, ".\\\""))
			continue;

		if (line[0] == '.')
			continue;
		if ((p = strchr(line + 1, '.')) == NULL)
			continue;
		if (p[1] == '\n')
			continue;

		p++;	/* skip over . */
		while (isspace(*p))
			p++;

		split_line(in, i, (p - line));

		/* trim trailing whitespace from old line */
		while (p > line && isspace(p[-1]))
			p--;

		p[0] = '\n';
		p[1] = '\0';
	}
}

static void
cross_references(input_t *in)
{
	boolean_t skip = B_FALSE;
	for (size_t i = 0; i < in->nlines; i++) {
		char *line = in->lines[i];
		int rc;

		if (skip) {
			if (starts_with(line, ".fi"))
				skip = B_FALSE;
			continue;
		}

		if (starts_with(line, ".nf")) {
			skip = B_TRUE;
			continue;
		}

		if (line[0] == '.')
			continue;

		regmatch_t match[3] = { 0 };

		rc = regexec(&r_xr, line, 3, match, 0);
		switch (rc) {
		case 0:
			break;
		case REG_NOMATCH:
			continue;
		default:
			fprintf(stderr, "%zu  Error matching\n", i + 1);
			continue;
		}

		size_t linenum = i;
		size_t namelen = match[1].rm_eo - match[1].rm_so + 1;
		size_t seclen = match[2].rm_eo - match[2].rm_so + 1;
		char name[namelen];
		char sec[seclen];
		char *xrline = NULL;
		char *end = "";

		if (line[match[0].rm_eo] == '.')
			end = " .";
		if (line[match[0].rm_eo] == ',')
			end = " ,";

		(void) strlcpy(name, &line[match[1].rm_so], namelen);
		(void) strlcpy(sec, &line[match[2].rm_so], seclen);

		xrline = xasprintf(".Xr %s %s%s\n", name, sec, end);

		if (match[0].rm_so > 1)
			split_line(in, linenum++, match[0].rm_so);

		split_line(in, linenum, match[0].rm_eo - match[0].rm_so);
		delete_line(in, linenum);
		insert_line(in, linenum, xrline);
	}
}

static size_t
do_th(input_t *in, size_t linenum)
{
	char datestr[32] = { 0 };
	char *dateline = NULL;
	char *line = in->lines[linenum];
	char *p = line;
	char *os = strdup(".Os\n");
	size_t cnt = 0;
	time_t now = time(NULL);

	if (os == NULL)
		err(EXIT_FAILURE, "Out of memory");

	(void) cftime(datestr, "%b %e, %Y", &now);

	for (p = line; *p != '\0'; p++) {
		if (*p == ' ' && ++cnt == 3)
			break;
	}

	if (cnt != 3)
		return (0);

	dateline = xasprintf(".Dd %s\n", datestr);
	insert_line(in, linenum, dateline);
	line[1] = 'D';
	line[2] = 't';
	p[0] = '\n';
	p[1] = '\0';

	insert_line(in, linenum + 2, os);
	return (2);
}

static void
do_name(input_t *in, size_t linenum)
{
	char *line = in->lines[linenum];
	char *pos = strstr(line, " \\- ");
	char *nameline = NULL;
	char *descline = NULL;

	if (pos == NULL)
		return;

	descline = xasprintf(".Nd %s\n", pos + 4);
}

static void
simple(input_t *in)
{
	size_t start = 0;
	char *line = NULL;

	if (starts_with(in->lines[0], "'\\\" te"))
		delete_line(in, 0);

	for (size_t i = start; i < in->nlines; i++) {
again:
		line = in->lines[i];

		/* .SH -> .Sh / etc. */
		if (starts_with(line, ".SH ")) {
			line[2] = 'h';
			continue;
		}
		if (starts_with(line, ".DT ")) {
			line[2] = 't';
			continue;
		}
		if (starts_with(line, ".SS ")) {
			line[2] = 's';
			continue;
		}
		if (strcmp(line, ".sp\n") == 0) {
			delete_line(in, i);
			goto again;
		}
		if (strcmp(line, ".LP\n") == 0) {
			/* Remove .LP after section ir subsection start */
			if (i > 0) {
				char *prev = in->lines[i - 1];
				if (starts_with(prev, ".Sh ") ||
				    starts_with(prev, ".Ss ")) {
					delete_line(in, i);
					goto again;
				}
			}

			line[1] = 'P';
			line[2] = 'p';
			continue;
		}
		if (starts_with(line, ".TH ")) {
			do_th(in, i);
			continue;
		}
	}
}

static void
delete_line(input_t *in, size_t linenum)
{
	VERIFY3U(linenum, <, in->nlines);

	if (linenum + 1 == in->nlines) {
		free(in->lines[linenum]);
		in->lines[--in->nlines] = NULL;
		return;
	}

	char *line = in->lines[linenum];
	char **src = in->lines + linenum + 1;
	char **dest = in->lines + linenum;
	size_t amt = (in->nlines - linenum - 1) * sizeof (char *);

	(void) memmove(dest, src, amt);
	in->nlines--;
	free(line);
}

/* Insert line before linenum */
static void
insert_line(input_t *in, size_t linenum, char *newline)
{
	char **src = in->lines + linenum;
	char **dest = src + 1;
	size_t amt = (in->nlines - linenum) * sizeof (char *);

	input_resv(in, 1);
	(void) memmove(dest, src, amt);
	in->lines[linenum] = newline;
	in->nlines++;
}

static void
split_line(input_t *in, size_t linenum, size_t col)
{
	VERIFY3U(linenum, <, in->nlines);
	VERIFY3U(col, >, 0);

	char *line = in->lines[linenum];
	char *after;
	size_t linelen = strlen(line);
	size_t afterlen;

	VERIFY3U(col, <, linelen);

	afterlen = linelen - col + 2;
	after = zalloc(afterlen);

	(void) strlcpy(after, &line[col], afterlen);
	input_resv(in, 1);

	line[col] = '\n';
	line[col + 1] = '\0';

	if (linenum + 1 == in->nlines)
		in->lines[in->nlines++] = after;
	else
		insert_line(in, linenum + 1, after);
}

static input_t *
read_file(const char *file)
{
	input_t *in = NULL;
	FILE *f = NULL;
	char *buf = NULL;
	size_t len = 0;
	ssize_t n;

	in = zalloc(sizeof (*in));

	if ((f = fopen(file, "r")) == NULL)
		err(EXIT_FAILURE, "Cannot open %s", file);

	while ((n = getline(&buf, &len, f)) > 0) {
		input_resv(in, 1);
		in->lines[in->nlines++] = buf;
		buf = NULL;
		len = 0;
	}

	if (n < 0 && errno != 0)
		err(EXIT_FAILURE, "Error reading %s", file);

	(void) fclose(f);

	return (in);
}

static boolean_t
error_constant(char *p, FILE *out)
{
	static const char *errors[] = {
	    "EPERM", "ENOENT", "ESRCH", "EINTR", "EIO", "ENXIO",
	    "E2BIG", "ENOEXEC", "EBADF", "ECHILD", "EAGAIN", "ENOMEM",
	    "EACCES", "EFAULT", "ENOTBLK", "EBUSY", "EEXIST", "EXDEV",
	    "ENODEV", "ENOTDIR", "EISDIR", "EINVAL", "ENFILE", "EMFILE",
	    "ENOTTY", "ETXTBSY", "EFBIG", "ENOSPC", "ESPIPE", "EROFS",
	    "EMLINK", "EPIPE", "EDOM", "ERANGE", "ENOMSG", "EIDRM",
	    "ECHRNG", "EL2NSYNC", "EL3HLT", "EL3RST", "ELNRNG", "EUNATCH",
	    "ENOCSI", "EL2HLT", "EDEADLK", "ENOLCK", "ECANCELED",
	    "ENOTSUP", "EDQUOT", "EBADE", "EBADR", "EXFULL", "ENOANO",
	    "EBADRQC", "EBADSLT", "EDEADLOCK", "EBFONT", "EOWNERDEAD",
	    "ENOTRECOVERABLE", "ENOSTR", "ENODATA", "ETIME", "ENOSR",
	    "ENONET", "ENOPKG", "EREMOTE", "ENOLINK", "EADV", "ESRMNT",
	    "ECOMM", "EPROTO", "ELOCKUNMAPPED", "ENOTACTIVE", "EMULTIHOP",
	    "EBADMSG", "ENAMETOOLONG", "EOVERFLOW", "ENOTUNIQ", "EBADFD",
	    "EREMCHG", "ELIBACC", "ELIBBAD", "ELIBSCN", "ELIBMAX",
	    "ELIBEXEC", "EILSEQ", "ENOSYS", "ELOOP", "ERESTART",
	    "ESTRPIPE", "ENOTEMPTY", "EUSERS", "ENOTSOCK", "EDESTADDRREQ",
	    "EMSGSIZE", "EPROTOTYPE", "ENOPROTOOPT", "EPROTONOSUPPORT",
	    "ESOCKTNOSUPPORT", "EOPNOTSUPP", "EPFNOSUPPORT", "EAFNOSUPPORT",
	    "EADDRINUSE", "EADDRNOTAVAIL", "ENETDOWN", "ENETUNREACH",
	    "ENETRESET", "ECONNABORTED", "ECONNRESET", "ENOBUFS",
	    "EISCONN", "ENOTCONN", "ESHUTDOWN", "ETOOMANYREFS", "ETIMEDOUT",
	    "ECONNREFUSED", "EHOSTDOWN", "EHOSTUNREACH", "EWOULDBLOCK",
	    "EALREADY", "EINPROGRESS", "ESTALE",
	};

	if (p[0] != 'E')
		return (B_FALSE);

	char *end = p;
	while (*end != '\0' && !isspace(*end))
		end++;

	size_t len = (size_t)(end - p) + 1;
	char str[len];

	(void) strlcpy(str, p, len);
	for (size_t i = 0; i < sizeof (errors) / sizeof (errors[0]); i++) {
		if (strcmp(str, errors[i]) == 0)
			return (B_TRUE);
	}
	return (B_FALSE);
}

static void
input_resv(input_t *in, size_t n)
{
	if (in->nlines + n < in->alloc)
		return;

	char **temp = NULL;
	size_t newalloc = in->alloc + CHUNK_SZ;
	size_t newsz = newalloc * sizeof (char *);

	temp = realloc(in->lines, newsz);
	if (temp == NULL)
		err(EXIT_FAILURE, "Out of memory");

	in->lines = temp;
	in->alloc = newalloc;
}

static boolean_t
starts_with(const char *str, const char *chr)
{
	if (strncmp(str, chr, strlen(chr)) == 0)
		return (B_TRUE);
	return (B_FALSE);
}

static char *
xasprintf(const char *fmt, ...)
{
	char *str = NULL;
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vasprintf(&str, fmt, ap);
	va_end(ap);

	if (ret == -1)
		err(EXIT_FAILURE, "Out of memory");

	return (str);
}

static void *
zalloc(size_t len)
{
	void *p = calloc(1, len);

	if (len == NULL)
		err(EXIT_FAILURE, "Out of memory");

	return (p);
}
