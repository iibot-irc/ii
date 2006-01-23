/*
 * (C)opyright MMV-MMVI Anselm R. Garbe <garbeam at gmail dot com>
 * (C)opyright MMV-MMVI Nico Golde <nico at ngolde dot de>
 * See LICENSE file for license details.
 */

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <string.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef nil
#define nil NULL /* for those who don't understand, nil is used in Plan9 */
#endif

enum { TOK_NICKSRV = 0, TOK_USER, TOK_CMD, TOK_CHAN, TOK_ARG, TOK_TEXT, TOK_LAST };

typedef struct Channel Channel;
struct Channel {
	int fd;
	char *name;
	Channel *next;
};

static int irc;
static Channel *channels = nil;
static char *host = "irc.freenode.net";
static char nick[32];			/* might change while running */
static char path[_POSIX_PATH_MAX];
static char message[PIPE_BUF]; /* message buf used for communication */

static void usage()
{
	fprintf(stderr, "%s",
			"ii - irc it - " VERSION "\n"
			"  (C)opyright MMVI Anselm R. Garbe, Nico Golde\n"
			"usage: ii [-i <irc dir>] [-s <host>] [-p <port>]\n"
			"          [-n <nick>] [-k <password>] [-f <fullname>]\n");
	exit(EXIT_SUCCESS);
}

/* creates directories top-down, if necessary */
static void create_dirtree(const char *dir)
{
	char tmp[256];
	char *p;
	size_t len;

	snprintf(tmp, sizeof(tmp),"%s",dir);
	len = strlen(tmp);
	if(tmp[len - 1] == '/')
		tmp[len - 1] = 0;
	for(p = tmp + 1; *p; p++)
		if(*p == '/') {
			*p = 0;
			mkdir(tmp, S_IRWXU);
			*p = '/';
		}
	mkdir(tmp, S_IRWXU);
}

static int get_filepath(char *filepath, size_t len, char *channel, char *file)
{
	if(channel) {
		if(!snprintf(filepath, len, "%s/%s", path, channel))
			return 0;
		create_dirtree(filepath);
		return snprintf(filepath, len, "%s/%s/%s", path, channel, file);
	}
	return snprintf(filepath, len, "%s/%s", path, file);
}

static void create_filepath(char *filepath, size_t len, char *channel, char *suffix)
{
	if(!get_filepath(filepath, len, channel, suffix)) {
		fprintf(stderr, "%s", "ii: path to irc directory too long\n");
		exit(EXIT_FAILURE);
	}
}

static Channel *get_channel(int fd)
{
	Channel *c;
	for (c = channels; c && c->fd != fd; c = c->next);
	return c;
}

static int open_channel(char *name)
{
	static char infile[256];
	create_filepath(infile, sizeof(infile), name, "in");
	if(access(infile, F_OK) == -1)
		mkfifo(infile, S_IRWXU);
	return open(infile, O_RDONLY | O_NONBLOCK, 0);
}

static void add_channel(char *name)
{
	Channel *c;
	int fd;

	for(c = channels; c; c = c->next)
		if(!strcmp(name, c->name))
			return; /* already handled */

    fd = open_channel(name);
	if(fd == -1) {
		perror("ii: cannot create in channel");
		return;
	}
	c = calloc(1, sizeof(Channel));
	if(!c) {
		perror("ii: cannot allocate memory");
		exit(EXIT_FAILURE);
	}
	if(channels)
		channels = c;
	else
		c->next = channels;
	c->fd = fd;
	c->name = strdup(name);
}

static void rm_channel(Channel *c)
{
	Channel *p;
	if(channels == c)
		channels = channels->next;
	else {
		for(p = channels; p && p->next != c; p = p->next);
		if(p->next == c)
			p->next = c->next;
	}
	free(c->name);
	free(c);
}

static void login(char *key, char *fullname)
{
	if(key)
		snprintf(message, PIPE_BUF,
				 "PASS %s\r\nNICK %s\r\nUSER %s localhost %s :%s\r\n", key,
				 nick, nick, host, fullname ? fullname : nick);
	else
		snprintf(message, PIPE_BUF, "NICK %s\r\nUSER %s localhost %s :%s\r\n",
				 nick, nick, host, fullname ? fullname : nick);
	write(irc, message, strlen(message));	/* login */
}

static int tcpopen(unsigned short port)
{
	int fd;
	struct sockaddr_in sin;
	struct hostent *hp = gethostbyname(host);

	memset(&sin, 0, sizeof(struct sockaddr_in));
	if(hp == nil) {
		perror("ii: cannot retrieve host information");
		exit(EXIT_FAILURE);
	}
	sin.sin_family = AF_INET;
	bcopy(hp->h_addr, (char *) &sin.sin_addr, hp->h_length);
	sin.sin_port = htons(port);
	if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("ii: cannot create socket");
		exit(EXIT_FAILURE);
	}
	if(connect(fd, (const struct sockaddr *) &sin, sizeof(sin)) < 0) {
		perror("ii: cannot connect to host");
		exit(EXIT_FAILURE);
	}
	return fd;
}

static size_t tokenize(char **result, size_t reslen, char *str, char delim)
{
	char *p, *n;
	size_t i;

	if(!str)
		return 0;
	for(n = str; *n == ' '; n++);
	p = n;
	for(i = 0; *n != 0;) {
		if(i == reslen)
			return 0;
		if(*n == delim) {
			*n = 0;
			result[i++] = p;
			p = ++n;
		} else
			n++;
	}
	result[i++] = p;
	return i + 2;				/* number of tokens */
}

static void print_out(char *channel, char *buf)
{
	static char outfile[256];
	FILE *out;
	static char buft[8];
	time_t t = time(0);

	create_filepath(outfile, sizeof(outfile), channel, "out");
	out = fopen(outfile, "a");
	strftime(buft, sizeof(buft), "%R", localtime(&t));
	fprintf(out, "%s %s\n", buft, buf);
	fclose(out);
}

static void proc_channels_privmsg(char *channel, char *buf)
{
	snprintf(message, PIPE_BUF, "<%s> %s", nick, buf);
	print_out(channel, message);
	snprintf(message, PIPE_BUF, "PRIVMSG %s :%s\r\n", channel, buf);
	write(irc, message, strlen(message));
}

static void proc_channels_input(int fd, char *buf)
{
	static char infile[256];
	char *p;
	Channel *c = get_channel(fd);

	/*int ret = 1; */
	if(c->name[0] != '/' && c->name[0] != 0) {
		proc_channels_privmsg(c->name, buf);
		return;
	}
	switch (buf[1]) {
	case 'j':
		p = strchr(&buf[3], ' ');
		if(p)
			*p = 0;
		snprintf(message, PIPE_BUF, "JOIN %s\r\n", &buf[3]);
		add_channel(&buf[3]);
		break;
	case 't':
		snprintf(message, PIPE_BUF, "TOPIC %s :%s\r\n", c->name, &buf[3]);
		break;
	case 'a':
		snprintf(message, PIPE_BUF, "-!- %s is away \"%s\"", nick, &buf[3]);
		print_out(c->name, message);
		if(buf[2] == 0)
			snprintf(message, PIPE_BUF, "AWAY\r\n");
		else
			snprintf(message, PIPE_BUF, "AWAY :%s\r\n", &buf[3]);
		break;
	case 'm':
		p = strchr(&buf[3], ' ');
		if(p) {
			*p = 0;
			add_channel(&buf[3]);
			proc_channels_privmsg(&buf[3], p + 1);
		}
		return;
		break;
	case 'n':
		snprintf(nick, sizeof(nick),"%s", buf);
		snprintf(message, PIPE_BUF, "NICK %s\r\n", &buf[3]);
		break;
	case 'l':
		if(c->name[0] == 0)
			return;
		if(buf[2] == ' ')
			snprintf(message, PIPE_BUF, "PART %s :%s\r\n", c->name, &buf[3]);
		else
			snprintf(message, PIPE_BUF,
					 "PART %s :ii - 500SLOC are too much\r\n", c->name);
		write(irc, message, strlen(message));
		close(fd);
		create_filepath(infile, sizeof(infile), c->name, "in");
		unlink(infile);
		rm_channel(c);
		return;
		break;
	default:
		snprintf(message, PIPE_BUF, "%s\r\n", &buf[1]);
		break;
	}
	write(irc, message, strlen(message));
}

static void proc_server_cmd(char *buf)
{
	char *argv[TOK_LAST], *cmd, *p;
	int i;
	if(!buf)
		return;

	for(i = 0; i < TOK_LAST; i++)
		argv[i] = nil;

	/*
	   <message>  ::= [':' <prefix> <SPACE> ] <command> <params> <crlf>
	   <prefix>   ::= <servername> | <nick> [ '!' <user> ] [ '@' <host> ]
	   <command>  ::= <letter> { <letter> } | <number> <number> <number>
	   <SPACE>    ::= ' ' { ' ' }
	   <params>   ::= <SPACE> [ ':' <trailing> | <middle> <params> ]
	   <middle>   ::= <Any *non-empty* sequence of octets not including SPACE
	   or NUL or CR or LF, the first of which may not be ':'>
	   <trailing> ::= <Any, possibly *empty*, sequence of octets not including NUL or CR or LF>
	   <crlf>     ::= CR LF
	 */
	if(buf[0] == ':') {		/* check prefix */
		p = strchr(buf, ' ');
		*p = 0;
		for(++p; *p == ' '; p++);
		cmd = p;
		argv[TOK_NICKSRV] = &buf[1];
		if((p = strchr(buf, '!'))) {
			*p = 0;
			argv[TOK_USER] = ++p;
		}
	} else
		cmd = buf;

	/* remove CRLFs */
	for(p = cmd; p && *p != 0; p++)
		if(*p == '\r' || *p == '\n')
			*p = 0;

	if((p = strchr(cmd, ':'))) {
		*p = 0;
		argv[TOK_TEXT] = ++p;
	}
	tokenize(&argv[TOK_CMD], TOK_LAST - TOK_CMD + 1, cmd, ' ');

	if(!strncmp("PING", argv[TOK_CMD], 5)) {
		snprintf(message, PIPE_BUF, "PONG %s\r\n", argv[TOK_TEXT]);
		write(irc, message, strlen(message));
		return;
	} else if(!argv[TOK_NICKSRV] || !argv[TOK_USER]) {	/* server command */
		snprintf(message, PIPE_BUF, "%s", argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
		print_out(0, message);
		return;
	} else if(!strncmp("ERROR", argv[TOK_CMD], 6))
		snprintf(message, PIPE_BUF, "-!- error %s", argv[TOK_TEXT] ? argv[TOK_TEXT] : "unknown");
	else if(!strncmp("JOIN", argv[TOK_CMD], 5)) {
		if(argv[TOK_TEXT]!=nil){
			p = strchr(argv[TOK_TEXT], ' ');
		if(p)
			*p = 0;
		}
		argv[TOK_CHAN] = argv[TOK_TEXT];
		snprintf(message, PIPE_BUF, "-!- %s(%s) has joined %s", argv[TOK_NICKSRV], argv[TOK_USER], argv[TOK_TEXT]);
	} else if(!strncmp("PART", argv[TOK_CMD], 5)) {
		snprintf(message, PIPE_BUF, "-!- %s(%s) has left %s", argv[TOK_NICKSRV], argv[TOK_USER], argv[TOK_CHAN]);
	} else if(!strncmp("MODE", argv[TOK_CMD], 5))
		snprintf(message, PIPE_BUF, "-!- %s changed mode/%s -> %s %s", argv[TOK_NICKSRV], argv[TOK_CMD + 1], argv[TOK_CMD + 2], argv[TOK_CMD + 3]);
	else if(!strncmp("QUIT", argv[TOK_CMD], 5))
		snprintf(message, PIPE_BUF, "-!- %s(%s) has quit %s \"%s\"", argv[TOK_NICKSRV], argv[TOK_USER], argv[TOK_CHAN], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	else if(!strncmp("NICK", argv[TOK_CMD], 5))
		snprintf(message, PIPE_BUF, "-!- %s changed nick to %s", argv[TOK_NICKSRV], argv[TOK_TEXT]);
	else if(!strncmp("TOPIC", argv[TOK_CMD], 6))
		snprintf(message, PIPE_BUF, "-!- %s changed topic to \"%s\"", argv[TOK_NICKSRV], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	else if(!strncmp("KICK", argv[TOK_CMD], 5))
		snprintf(message, PIPE_BUF, "-!- %s kicked %s (\"%s\")", argv[TOK_NICKSRV], argv[TOK_ARG], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	else if(!strncmp("NOTICE", argv[TOK_CMD], 7))
		snprintf(message, PIPE_BUF, "-!- \"%s\")", argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	else if(!strncmp("PRIVMSG", argv[TOK_CMD], 8))
		snprintf(message, PIPE_BUF, "<%s> %s", argv[TOK_NICKSRV], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	if(!argv[TOK_CHAN] || !strncmp(argv[TOK_CHAN], nick, strlen(nick)))
		print_out(argv[TOK_NICKSRV], message);
	else
		print_out(argv[TOK_CHAN], message);
}

static int read_line(int fd, size_t res_len, char *buf)
{
	size_t i = 0;
	char c;
	do {
		if(read(fd, &c, sizeof(char)) != sizeof(char))
			return -1;
		buf[i++] = c;
	}
	while(c != '\n' && i < res_len);
	buf[i - 1] = 0;			/* eliminates '\n' */
	return 0;
}

static void handle_channels_input(Channel *c)
{
	static char buf[PIPE_BUF];
	if(read_line(c->fd, PIPE_BUF, buf) == -1) {
		int fd = open_channel(c->name);
		if(fd != -1)
			c->fd = fd;
		else
			rm_channel(c);
		return;
	}
	proc_channels_input(c->fd, buf);
}

static void handle_server_output()
{
	static char buf[PIPE_BUF];
	if(read_line(irc, PIPE_BUF, buf) == -1) {
		perror("ii: remote host closed connection");
		exit(EXIT_FAILURE);
	}
	proc_server_cmd(buf);
}

static void run()
{
	Channel *c;
	int r, maxfd;
	fd_set rd;

	for(;;) {
		FD_ZERO(&rd);
		maxfd = irc;
		FD_SET(irc, &rd);
		for(c = channels; c; c = c->next) {
			if(maxfd < c->fd) {
				maxfd = c->fd;
				FD_SET(c->fd, &rd);
			}
		}

		r = select(maxfd + 1, &rd, 0, 0, 0);
		if(r == -1 && errno == EINTR)
			continue;
		if(r < 0) {
			perror("ii: error on select()");
			exit(EXIT_FAILURE);
		} else if(r > 0) {
			if(FD_ISSET(irc, &rd))
				handle_server_output();
			for(c = channels; c; c = c->next)
				if(FD_ISSET(c->fd, &rd))
					handle_channels_input(c);
		}
	}
}

int main(int argc, char *argv[])
{
	int i;
	unsigned short port = 6667;
	struct passwd *spw = getpwuid(getuid());
	char *key = nil;
	char prefix[_POSIX_PATH_MAX];
	char *fullname = nil;

	if(!spw) {
		fprintf(stderr,"ii: getpwuid() failed\n"); 
		exit(EXIT_FAILURE);
	}
	snprintf(nick, sizeof(nick), "%s", spw->pw_name);
	snprintf(prefix, sizeof(prefix),"%s", spw->pw_dir);

	if(argc == 2 && argv[1][0] == '-' && argv[1][1] == 'h')
		usage();

	for(i = 1; (i + 1 < argc) && (argv[i][0] == '-'); i++) {
		switch (argv[i][1]) {
		case 'i': snprintf(prefix,sizeof(prefix),"%s", argv[++i]); break;
		case 's': host = argv[++i]; break;
		case 'p': port = atoi(argv[++i]); break;
		case 'n': snprintf(nick,sizeof(nick),"%s", argv[++i]); break;
		case 'k': key = argv[++i]; break;
		case 'f': fullname = argv[++i]; break;
		default: usage(); break;
		}
	}
	irc = tcpopen(port);
	if(!snprintf(path, sizeof(path), "%s/irc/%s", prefix, host)) {
		fprintf(stderr, "%s", "ii: path to irc directory too long\n");
		exit(EXIT_FAILURE);
	}
	create_dirtree(path);

	add_channel(""); /* master channel */
	login(key, fullname);
	run();

	return 0;
}
