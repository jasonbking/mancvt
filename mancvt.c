/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include <alloca.h>
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

#ifndef ARRAY_SIZE
#define	ARRAY_SIZE(a)	(sizeof (a) / sizeof (a[0]))
#endif

typedef struct input {
	char **lines;
	size_t nlines;
	size_t alloc;
} input_t;

static const char *close_delim = ".,:;?!)]";

static const char r_xrstr[] =
   "\\\\fB\\([.A-Za-z0-9_-]\\{1,\\}\\)\\\\fR(\\([1-9][A-Z]*\\))";
static regex_t r_xr; /* cross reference */

enum {
	SYMBOLS,
	VARIABLES,
	DEFINES,
	TYPES
};

static struct {
	const char *templ;
	const char *cmd;
	regex_t *regex;
	size_t nregex;
} subtbl[] = {
	{ "\\\\fB\\(%s\\)\\\\fR", "Sy", NULL, 0 },
	{ "\\\\fI\\(%s\\)\\\\fR", "Va", NULL, 0 },
	{ "\\\\fB\\(%s\\)\\\\fR", "Dv", NULL, 0 },
	{ "\\\\fB\\(%s\\)\\\\fR", "Vt", NULL, 0 }
};

static void *zalloc(size_t);
static char *xasprintf(const char *, ...);
static char *xstrdup(const char *);
static void add_sub(int, const char *);
static void input_resv(input_t *, size_t);
static void insert_line(input_t *, size_t, char *);
static void split_line(input_t *, size_t, size_t);
static void delete_line(input_t *, size_t);
static void replace_with_cmd(input_t *restrict, size_t, size_t, size_t,
    const char *restrict);
static void check_regexes(input_t *, size_t, const char *, regex_t *, size_t);
static input_t *read_file(const char *);
static boolean_t starts_with(const char *, const char *);
static void cross_references(input_t *);
static void split_paragraphs(input_t *);
static void simple(input_t *);
static void code(input_t *);
static void subs(input_t *);
static void name(input_t *in);
static void extra_spaces(input_t *);
static void blank_lines(input_t *);

static void
usage(const char *progname)
{
	(void) fprintf(stderr, "Usage: %s [-s sym] file\n", progname);
	exit(EXIT_FAILURE);
}

int
main(int argc, char * const *argv)
{
	input_t *in = NULL;
	int c;

	while ((c = getopt(argc, argv, "D:s:t:v:")) != -1) {
		switch(c) {
		case 'D':
			add_sub(DEFINES, optarg);
			break;
		case 's':
			add_sub(SYMBOLS, optarg);
			break;
		case 't':
			add_sub(TYPES, optarg);
			break;
		case 'v':
			add_sub(VARIABLES, optarg);
			break;
		case '?':
			usage(argv[0]);
		}
	}

	VERIFY0(regcomp(&r_xr, r_xrstr, 0));

	in = read_file(argv[optind]);

	name(in);
	cross_references(in);
	subs(in);
	simple(in);
	code(in);
	split_paragraphs(in);
	extra_spaces(in);
	blank_lines(in);

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

		/* Ignore non-breaking space followed by . at start of line */
		if (starts_with(line, "\\&."))
			continue;

		/* Skip lines that start with / */
		if (line[0] == '.')
			continue;
		if ((p = strchr(line + 1, '.')) == NULL)
			continue;
		/* Skip escaped periods */
		if (p[-1] == '\\')
			continue;
		/* Or . at the end of a line */
		if (p[1] == '\n')
			continue;

		p++;	/* skip over . */
		while (*p != '\0' && isspace(*p))
			p++;

		if (*p != '\0') {
			split_line(in, i, (p - line));
			/* trim trailing whitespace from old line */
			while (p > line && isspace(p[-1]))
				p--;

			p[0] = '\n';
			p[1] = '\0';
		}
	}
}

static void
cross_references(input_t *in)
{
	boolean_t skip = B_FALSE;

	for (size_t i = 0; i < in->nlines; i++) {
		char *line = in->lines[i];
		int rc;

		/* Skip over non-formatted (.nf) spans of text */
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

		int xrlen = match[1].rm_eo - match[1].rm_so;
		int seclen = match[2].rm_eo - match[2].rm_so;
		size_t cmdlen = 3 /* 'Xr ' */ + xrlen + 1 + seclen + 2;
		char cmd[cmdlen];

		(void) snprintf(cmd, cmdlen, "Xr %.*s %.*s",
		    xrlen, line + match[1].rm_so,
		    seclen, line + match[2].rm_so);

		replace_with_cmd(in, i, match[0].rm_so, match[0].rm_eo, cmd);
	}
}

/*
 * Look for:
 *     ...
 *     .in +2
 *     .nf
 *     .... Code block ...
 *     .fi
 *     .in -2
 * and replace with
 *     .Bd -literal -offset 2n
 *     ... Code block ....
 *     .Ed
 */
static void
code(input_t *in)
{
	boolean_t in_code = B_FALSE;
	for (size_t i = 0; i < in->nlines; i++) {
		char *line = in->lines[i];
		char *nextline = in->lines[i + 1];

		if (!in_code && starts_with(line, ".in +2") &&
		    nextline != NULL && starts_with(nextline, ".nf")) {
			in_code = B_TRUE;
			free(line);
			in->lines[i] = xstrdup(".Bd -literal -offset 2n\n");
			delete_line(in, i + 1);
			continue;
		}

		if (in_code && starts_with(line, ".fi") &&
		    nextline != NULL && starts_with(nextline, ".in -2")) {
			in_code = B_FALSE;
			line[1] = 'E';
			line[2] = 'd';
			delete_line(in, i + 1);
			continue;
		}
	}

	VERIFY(!in_code);
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

static size_t
do_nameline(input_t *in, size_t linenum)
{
	char *line = in->lines[linenum];
	char *pos = strstr(line, " \\- ");
	char *nameline = NULL;
	char *descline = NULL;
	size_t count = 0;

	if (pos == NULL)
		return (0);

	pos[0] = '\0';
	descline = xasprintf(".Nd %s", pos + 4);
	insert_line(in, linenum + 1, descline);

	for (char *nm = strtok(line, ", ");
	    nm != NULL;
	    nm = strtok(NULL, ", ")) {
		nameline = xasprintf(".Nm %s\n", nm);
		insert_line(in, linenum + count + 1, nameline);
		count++;
	}
	delete_line(in, linenum);

	return (count);
}

static void
name(input_t *in)
{
	boolean_t in_sect = B_FALSE;

	for (size_t i = 0; i < in->nlines; i++) {
		char *line = in->lines[i];

		if (!in_sect) {
			if (starts_with(line, ".Sh NAME") ||
			    starts_with(line, ".SH NAME"))
				in_sect = B_TRUE;
			continue;
		}

		if (starts_with(line, ".Sh") || starts_with(line, ".SH"))
			break;

		i += do_nameline(in, i);

		/* TODO: search other sections for names and replace with Nm */
	}
}

static void
subs(input_t *in)
{
	boolean_t skip = B_FALSE;

	for (size_t i = 0; i < in->nlines; i++) {
		if (skip) {
			if (starts_with(in->lines[i], ".fi"))
				skip = B_FALSE;
			continue;
		}

		if (starts_with(in->lines[i], ".nf")) {
			skip = B_TRUE;
			continue;
		}

		for (size_t j = 0; j < ARRAY_SIZE(subtbl); j++) {
			check_regexes(in, i, subtbl[j].cmd, subtbl[j].regex,
			    subtbl[j].nregex);
		}
	}
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
extra_spaces(input_t *in)
{
	boolean_t skip = B_FALSE;

	for (size_t i = 0; i < in->nlines; i++) {
		char *line = in->lines[i];
		size_t len = strlen(line);

		if (skip) {
			if (starts_with(line, ".Ed") ||
			    starts_with(line, ".fi"))
				skip = B_FALSE;
			continue;
		}

		if (starts_with(line, ".nf") ||
		    starts_with(line, ".Bd")) {
			skip = B_TRUE;
			continue;
		}

		if (line[0] == '.')
			continue;

		for (char *p = line; p[0] != '\0'; p++) {
			if (p[0] != ' ')
				continue;

			char *start = p;
			while (p[1] == ' ')
				p++;

			if (p - start > 0) {
				size_t amt = (size_t)(p - start);
				(void) memmove(start, p, len - amt);
				len -= amt;
			}
		}
	}
}

static void
blank_lines(input_t *in)
{
	boolean_t skip = B_FALSE;

	for (size_t i = 0; i < in->nlines; i++) {
		char *line = in->lines[i];
		char *p = line;
		size_t len = strlen(line);

		if (skip) {
			if (starts_with(line, ".Ed") ||
			    starts_with(line, ".fi"))
				skip = B_FALSE;
			continue;
		}

		if (starts_with(line, ".nf") ||
		    starts_with(line, ".Bd")) {
			skip = B_TRUE;
			continue;
		}

		while (*p != '\0' && isspace(*p))
			p++;

		if (*p == '\0')
			delete_line(in, i--);
	}
}

/*
 * Replaces span of line with a given command, including breaking line as
 * necessary, and handle any leading or trailing delimiters
 */
static void
replace_with_cmd(input_t *restrict in, size_t linenum, size_t start, size_t end,
    const char *restrict cmd)
{
	char *line = in->lines[linenum];
	char *cmdline = NULL;
	char *p = NULL;
	char *suffix = NULL;
	size_t len = strlen(line);

	/*
	 * If the command not at the end of the line, look for any
	 * delimiters after the end of the command we're replacing and
	 * fill suffix with them, then split the line after any subsequence
	 * whitespace so something like '..... \fBfoo(1M)\fR. Foobar ...'
	 * will set suffix to ' .' and split the line so the next line starts
	 * with 'Foobar'.
	 */
	if (end < len - 1) {
		const char *endp = line + end;
		size_t seplen = 0;

		if ((seplen = strspn(endp, close_delim)) > 0) {
			suffix = alloca(seplen * 2 + 1);
			for (size_t i = 0; i < seplen; i++) {
				suffix[i * 2] = ' ';
				suffix[i * 2 + 1] = endp[i];
			}
			suffix[seplen * 2] = '\0';
		}

		end += seplen;
		while (isspace(line[end]))
			end++;

		if (end != len)
			split_line(in, linenum, end);
	}

	if (start > 0) {
		while (start > 0 && isspace(line[start]))
			start--;

		if (start > 0) {
			split_line(in, linenum++, start);
			line = in->lines[linenum];
		}
	}

	cmdline = xasprintf(".%s%s\n", cmd, (suffix != NULL) ? suffix : "");
	free(line);
	in->lines[linenum] = cmdline;
}

static void
check_regexes(input_t *in, size_t linenum, const char *cmd,
    regex_t *regexes, size_t nregex)
{
	for (size_t i = 0; i < nregex; i++) {
		char *line = in->lines[linenum];
		regmatch_t match[2] = { 0 };

		switch (regexec(&regexes[i], line, 2, match, 0)) {
		case 0:
			break;
		case REG_NOMATCH:
			continue;
		default:
			(void) fprintf(stderr, "regex failed\n");
			continue;
		}

		int matchlen = match[1].rm_eo - match[1].rm_so + 1;
		size_t cmdlen = matchlen + strlen(cmd) + 1;
		char cmdstr[cmdlen];

		(void) snprintf(cmdstr, cmdlen, "%s %.*s", cmd, matchlen,
		    line + match[1].rm_so);

		replace_with_cmd(in, linenum, match[0].rm_so, match[0].rm_eo,
		    cmdstr);
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

/* Break line at col, character at line[col] becomes start of new line */
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
		input_resv(in, 2);
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

	(void) memset(temp + in->alloc, 0, CHUNK_SZ * sizeof (char *));
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

static void
add_sub(int which, const char *str)
{
	regex_t r = { 0 };
	char *rstr = xasprintf(subtbl[which].templ, str);
	regex_t *new = realloc(subtbl[which].regex,
	    (subtbl[which].nregex + 1) * sizeof (regex_t));

	if (new == NULL)
		err(EXIT_FAILURE, "Out of memory");
	subtbl[which].regex = new;

	if (regcomp(&subtbl[which].regex[subtbl[which].nregex++], rstr, 0) != 0)
		err(EXIT_FAILURE, "Could not convert '%s' to regex", str);

	free(rstr);
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

static char *
xstrdup(const char *src)
{
	char *s = strdup(src);

	if (s == NULL)
		err(EXIT_FAILURE, "Out of memory");

	return (s);
}

static void *
zalloc(size_t len)
{
	void *p = calloc(1, len);

	if (len == NULL)
		err(EXIT_FAILURE, "Out of memory");

	return (p);
}
