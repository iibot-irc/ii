/*
 * (C)opyright MMV Anselm R. Garbe <garbeam at gmail dot com>
 *                 Nico Golde <nico at ngolde dot de>
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

static int irc;
static char *fifo[_POSIX_PATH_MAX];
static char *server = "irc.freenode.net";
static char nick[32];			/* might change while running */
static char path[_POSIX_PATH_MAX];
static char buf[PIPE_BUF]; /* buffers used for communication */
static char _buf[PIPE_BUF]; /* help buffer */

static int add_channel(char *channel);

static void usage()
{
	fprintf(stderr, "%s",
			"ii - irc it - " VERSION "\n"
			"  (C)opyright MMV Anselm R. Garbe, Nico Golde\n"
			"usage: ii [-i <irc dir>] [-s <server>] [-p <port>]\n"
			"          [-n <nick>] [-k <password>] [-f <fullname>]\n");
	exit(EXIT_SUCCESS);
}

static void login(char *key, char *fullname)
{
	if(key)
		snprintf(buf, PIPE_BUF,
				 "PASS %s\r\nNICK %s\r\nUSER %s localhost %s :%s\r\n", key,
				 nick, nick, server, fullname ? fullname : nick);
	else
		snprintf(buf, PIPE_BUF, "NICK %s\r\nUSER %s localhost %s :%s\r\n",
				 nick, nick, server, fullname ? fullname : nick);
	write(irc, buf, strlen(buf));	/* login */
}

static int tcpopen(unsigned short port)
{
	int fd;
	struct sockaddr_in sin;
	struct hostent *hp = gethostbyname(server);

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
		perror("ii: cannot connect to server");
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

/* creates directories top-down, if necessary */
static void create_lastdir(const char *dir)
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

static int get_filepath(char *filepath, size_t len, char *channel,
							char *file)
{
	if(channel) {
		if(!snprintf(filepath, len, "%s/%s", path, channel))
			return 0;
		create_lastdir(filepath);
		return snprintf(filepath, len, "%s/%s/%s", path, channel, file);
	}
	return snprintf(filepath, len, "%s/%s", path, file);
}

static void create_filepath(char *filepath, size_t len, char *channel,
							char *suffix)
{
	if(!get_filepath(filepath, len, channel, suffix)) {
		fprintf(stderr, "%s", "ii: path to irc directory too long\n");
		exit(EXIT_FAILURE);
	}
}

static void print_out(char *channel, char *buffer)
{
	static char outfile[256];
	FILE *out;
	static char buft[8];
	time_t t = time(0);

	create_filepath(outfile, sizeof(outfile), channel, "out");
	out = fopen(outfile, "a");
	strftime(buft, sizeof(buft), "%R", localtime(&t));
	fprintf(out, "%s %s\n", buft, buffer);
	fclose(out);
}

static void proc_fifo_privmsg(char *channel, char *buffer)
{
	snprintf(buf, PIPE_BUF, "<%s> %s", nick, buffer);
	print_out(channel, buf);
	snprintf(buf, PIPE_BUF, "PRIVMSG %s :%s\r\n", channel, buffer);
	write(irc, buf, strlen(buf));
}

static void proc_fifo_input(int fd, char *buffer)
{
	static char infile[256];
	char *p;
	/*int ret = 1; */
	if(buffer[0] != '/') {
		if(fifo[fd][0] != 0)
			proc_fifo_privmsg(fifo[fd], buffer);
		return;
	}
	switch (buffer[1]) {
	case 'j':
		p = strchr(&buffer[3], ' ');
		if(p)
			*p = 0;
		snprintf(buf, PIPE_BUF, "JOIN %s\r\n", &buffer[3]);
		add_channel(&buffer[3]);
		break;
	case 't':
		snprintf(buf, PIPE_BUF, "TOPIC %s :%s\r\n", fifo[fd], &buffer[3]);
		break;
	case 'a':
		snprintf(buf, PIPE_BUF, "-!- %s is away \"%s\"", nick, &buffer[3]);
		print_out(fifo[fd], buf);
		if(&buffer[3] == nil)
			snprintf(buf, PIPE_BUF, "AWAY\r\n");
		else
			snprintf(buf, PIPE_BUF, "AWAY :%s\r\n", &buffer[3]);
		break;
	case 'm':
		p = strchr(&buffer[3], ' ');
		if(p) {
			*p = 0;
			add_channel(&buffer[3]);
			proc_fifo_privmsg(&buffer[3], p + 1);
		}
		return;
		break;
	case 'n':
		snprintf(nick, sizeof(nick),"%s", buffer);
		snprintf(buf, PIPE_BUF, "NICK %s\r\n", &buffer[3]);
		break;
	case 'l':
		if(fifo[fd][0] == 0)
			return;
		if(buffer[2] == ' ')
			snprintf(buf, PIPE_BUF, "PART %s :%s\r\n", fifo[fd],
					 &buffer[3]);
		else
			snprintf(buf, PIPE_BUF,
					 "PART %s :ii - 500SLOC are too much\r\n", fifo[fd]);
		write(irc, buf, strlen(buf));
		close(fd);
		create_filepath(infile, sizeof(infile), fifo[fd], "in");
		unlink(infile);
		free(fifo[fd]);
		fifo[fd] = 0;
		return;
		break;
	default:
		snprintf(buf, PIPE_BUF, "%s\r\n", &buffer[1]);
		break;
	}
	write(irc, buf, strlen(buf));
}

static void proc_server_cmd(char *buffer)
{
	char *argv[TOK_LAST], *cmd, *p;
	int i;
	if(!buffer)
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
	if(buffer[0] == ':') {		/* check prefix */
		p = strchr(buffer, ' ');
		*p = 0;
		for(++p; *p == ' '; p++);
		cmd = p;
		argv[TOK_NICKSRV] = &buffer[1];
		if((p = strchr(buffer, '!'))) {
			*p = 0;
			argv[TOK_USER] = ++p;
		}
	} else
		cmd = buffer;

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
		snprintf(buf, PIPE_BUF, "PONG %s\r\n", argv[TOK_TEXT]);
		write(irc, buf, strlen(buf));
		return;
	} else if(!argv[TOK_NICKSRV] || !argv[TOK_USER]) {	/* server command */
		snprintf(buf, PIPE_BUF, "%s", argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
		print_out(0, buf);
		return;
	} else if(!strncmp("ERROR", argv[TOK_CMD], 6))
		snprintf(buf, PIPE_BUF, "-!- error %s", argv[TOK_TEXT] ? argv[TOK_TEXT] : "unknown");
	else if(!strncmp("JOIN", argv[TOK_CMD], 5)) {
		if(argv[TOK_TEXT]!=nil){
			p = strchr(argv[TOK_TEXT], ' ');
		if(p)
			*p = 0;
		}
		argv[TOK_CHAN] = argv[TOK_TEXT];
		snprintf(buf, PIPE_BUF, "-!- %s(%s) has joined %s", argv[TOK_NICKSRV], argv[TOK_USER], argv[TOK_TEXT]);
	} else if(!strncmp("PART", argv[TOK_CMD], 5)) {
		snprintf(buf, PIPE_BUF, "-!- %s(%s) has left %s", argv[TOK_NICKSRV], argv[TOK_USER], argv[TOK_CHAN]);
	} else if(!strncmp("MODE", argv[TOK_CMD], 5))
		snprintf(buf, PIPE_BUF, "-!- %s changed mode/%s -> %s %s", argv[TOK_NICKSRV], argv[TOK_CMD + 1], argv[TOK_CMD + 2], argv[TOK_CMD + 3]);
	else if(!strncmp("QUIT", argv[TOK_CMD], 5))
		snprintf(buf, PIPE_BUF, "-!- %s(%s) has quit %s \"%s\"", argv[TOK_NICKSRV], argv[TOK_USER], argv[TOK_CHAN], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	else if(!strncmp("NICK", argv[TOK_CMD], 5))
		snprintf(buf, PIPE_BUF, "-!- %s changed nick to %s", argv[TOK_NICKSRV], argv[TOK_TEXT]);
	else if(!strncmp("TOPIC", argv[TOK_CMD], 6))
		snprintf(buf, PIPE_BUF, "-!- %s changed topic to \"%s\"", argv[TOK_NICKSRV], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	else if(!strncmp("KICK", argv[TOK_CMD], 5))
		snprintf(buf, PIPE_BUF, "-!- %s kicked %s (\"%s\")", argv[TOK_NICKSRV], argv[TOK_ARG], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	else if(!strncmp("NOTICE", argv[TOK_CMD], 7))
		snprintf(buf, PIPE_BUF, "-!- \"%s\")", argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	else if(!strncmp("PRIVMSG", argv[TOK_CMD], 8))
		snprintf(buf, PIPE_BUF, "<%s> %s", argv[TOK_NICKSRV], argv[TOK_TEXT] ? argv[TOK_TEXT] : "");
	if(!argv[TOK_CHAN] || !strncmp(argv[TOK_CHAN], nick, strlen(nick)))
		print_out(argv[TOK_NICKSRV], buf);
	else
		print_out(argv[TOK_CHAN], buf);
}

static int open_fifo(char *channel)
{
	static char infile[256];
	create_filepath(infile, sizeof(infile), channel, "in");
	if(access(infile, F_OK) == -1)
		mkfifo(infile, S_IRWXU);
	return open(infile, O_RDONLY | O_NONBLOCK, 0);
}

static int add_channel(char *channel)
{
	int i;
	char *new;

	if(channel && channel[0] != 0) {
		for(i = 0; i < 256; i++)
			if(fifo[i] && !strncmp(channel, fifo[i], strlen(channel)))
				return 1;
	}
	new = strdup(channel);
	if((i = open_fifo(new)) >= 0)
		fifo[i] = new;
	else {
		perror("ii: cannot create in fifo");
		return 0;
	}
	return 1;
}

static int readl_fd(int fd)
{
	int i = 0;
	char c;
	do {
		if(read(fd, &c, sizeof(char)) != sizeof(char))
			return 0;
		_buf[i++] = c;
	}
	while(c != '\n' && i<PIPE_BUF);
	_buf[i - 1] = 0;			/* eliminates '\n' */
	return 1;
}

static void handle_fifo_input(int fd)
{
	if(!readl_fd(fd)) {
		int i;
		if((i = open_fifo(fifo[fd]))) {
			fifo[i] = fifo[fd];
			fifo[fd] = 0;
		}
		return;
	}
	proc_fifo_input(fd, _buf);
}

static void handle_server_output()
{
	if(!readl_fd(irc)) {
		perror("ii: remote host closed connection");
		exit(EXIT_FAILURE);
	}
	proc_server_cmd(_buf);
}

static void run()
{
	int i, r, maxfd;
	fd_set rd;

	for(;;) {
		/* prepare */
		FD_ZERO(&rd);
		maxfd = irc;
		FD_SET(irc, &rd);
		for(i = 0; i < 256; i++) {
			if(fifo[i]) {
				if(maxfd < i)
					maxfd = i;
				FD_SET(i, &rd);
			}
		}

		r = select(maxfd + 1, &rd, 0, 0, 0);
		if(r == -1 && errno == EINTR)
			continue;
		if(r < 0) {
			perror("ii: error on select()");
			exit(EXIT_FAILURE);
		} else if(r > 0) {
			for(i = 0; i < 256; i++) {
				if(FD_ISSET(i, &rd)) {
					if(i == irc)
						handle_server_output();
					else
						handle_fifo_input(i);
				}
			}
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
		case 'i':
			snprintf(prefix,sizeof(prefix),"%s", argv[++i]);
			break;
		case 's':
			server = argv[++i];
			break;
		case 'p':
			port = atoi(argv[++i]);
			break;
		case 'n':
			snprintf(nick,sizeof(nick),"%s", argv[++i]);
			break;
		case 'k':
			key = argv[++i];
			break;
		case 'f':
			fullname = argv[++i];
			break;
		default:
			usage();
			break;
		}
	}
	irc = tcpopen(port);
	if(!snprintf(path, sizeof(path), "%s/irc/%s", prefix, server)) {
		fprintf(stderr, "%s", "ii: path to irc directory too long\n");
		exit(EXIT_FAILURE);
	}
	create_lastdir(path);

	for(i = 0; i < 256; i++)
		fifo[i] = 0;

	if(!add_channel(""))
		exit(EXIT_FAILURE);
	login(key, fullname);
	run();

	return 0;
}
