/*-
 * Copyright (c) 1994, Garrett Wollman
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)$Id$";
static char rcsid[] = "$Id$";
#endif /* LIBC_SCCS and not lint */

#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>

extern struct hostent * _gethostbyhtname  __P((const char *));
extern struct hostent * _gethostbydnsname __P((const char *));
extern struct hostent * _gethostbynisname __P((const char *));
extern struct hostent * _gethostbyhtaddr  __P((const char *, int, int));
extern struct hostent * _gethostbydnsaddr __P((const char *, int, int));
extern struct hostent * _gethostbynisaddr __P((const char *, int, int));

#define _PATH_HOSTCONF	"/etc/host.conf"

enum service_type { 
  SERVICE_NONE = 0,
  SERVICE_BIND,
  SERVICE_HOSTS,
  SERVICE_NIS };
#define SERVICE_MAX	SERVICE_NIS

static struct {
  const char *name;
  enum service_type type;
} service_names[] = {
  { "hosts", SERVICE_HOSTS },
  { "/etc/hosts", SERVICE_HOSTS },
  { "hosttable", SERVICE_HOSTS },
  { "htable", SERVICE_HOSTS },
  { "bind", SERVICE_BIND },
  { "dns", SERVICE_BIND },
  { "domain", SERVICE_BIND },
  { "yp", SERVICE_NIS },
  { "yellowpages", SERVICE_NIS },
  { "nis", SERVICE_NIS },
  { 0, SERVICE_NONE }
};

static enum service_type service_order[SERVICE_MAX + 1];
static int service_done = 0;

static enum service_type
get_service_name(const char *name) {
	int i;
	for(i = 0; service_names[i].type != SERVICE_NONE; i++) {
		if(!strcasecmp(name, service_names[i].name)) {
			return service_names[i].type;
		}
	}
	return SERVICE_NONE;
}

static void
init_services()
{
	char *cp, buf[BUFSIZ];
	register int cc = 0;
	FILE *fd;

	if ((fd = (FILE *)fopen(_PATH_HOSTCONF, "r")) == NULL) {
				/* make some assumptions */
		service_order[0] = SERVICE_BIND;
		service_order[1] = SERVICE_HOSTS;
		service_order[2] = SERVICE_NONE;
	} else {
		while (fgets(buf, BUFSIZ, fd) != NULL && cc < SERVICE_MAX) {
			if(buf[0] == '#')
				continue;

			cp = strtok(buf, "\n \t,:;");
			do {
				if(!isalpha(cp[0])) continue;
				service_order[cc] = get_service_name(buf);
				if(service_order[cc] != SERVICE_NONE)
					cc++;
			} while((cp = strtok((char *)0, "\n \t,:;"))
				&& (cc < SERVICE_MAX));
		}
		service_order[cc] = SERVICE_NONE;
		fclose(fd);
	}
	service_done = 1;
}

struct hostent *
gethostbyname(const char *name)
{
	struct hostent *hp = 0;
	int nserv = 0;

	if (!service_done)
		init_services();

	while (!hp) {
		switch (service_order[nserv]) {
		      case SERVICE_NONE:
			return NULL;
		      case SERVICE_HOSTS:
			hp = _gethostbyhtname(name);
			break;
		      case SERVICE_BIND:
			hp = _gethostbydnsname(name);
			break;
		      case SERVICE_NIS:
			hp = _gethostbynisname(name);
			break;
		}
		nserv++;
	}
	return hp;
}

struct hostent *
gethostbyaddr(const char *addr, int len, int type)
{
	struct hostent *hp = 0;
	int nserv = 0;

	if (!service_done)
		init_services();

	while (!hp) {
		switch (service_order[nserv]) {
		      case SERVICE_NONE:
			return 0;
		      case SERVICE_HOSTS:
			hp = _gethostbyhtaddr(addr, len, type);
			break;
		      case SERVICE_BIND:
			hp = _gethostbydnsaddr(addr, len, type);
			break;
		      case SERVICE_NIS:
			hp = _gethostbynisaddr(addr, len, type);
			break;
		}
		nserv++;
	}
	return hp;
}

void
sethostent(stayopen)
	int stayopen;
{
	_sethosthtent(stayopen);
	_sethostdnsent(stayopen);
}

void
endhostent()
{
	_endhosthtent();
	_endhostdnsent();
}
