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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "namespace.h"
#include "reentrant.h"
#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <nsswitch.h>
#include <arpa/nameser.h>		/* XXX hack for _res */
#include <resolv.h>			/* XXX hack for _res */
#include "un-namespace.h"
#include "netdb_private.h"

extern int _ht_gethostbyname(void *, void *, va_list);
extern int _dns_gethostbyname(void *, void *, va_list);
extern int _nis_gethostbyname(void *, void *, va_list);
extern int _ht_gethostbyaddr(void *, void *, va_list);
extern int _dns_gethostbyaddr(void *, void *, va_list);
extern int _nis_gethostbyaddr(void *, void *, va_list);

static int gethostbyname_internal(const char *, int, struct hostent *, char *,
    size_t, struct hostent **, int *, res_state);

/* Host lookup order if nsswitch.conf is broken or nonexistant */
static const ns_src default_src[] = {
	{ NSSRC_FILES, NS_SUCCESS },
	{ NSSRC_DNS, NS_SUCCESS },
	{ 0 }
};

NETDB_THREAD_ALLOC(hostent)
NETDB_THREAD_ALLOC(hostent_data)
NETDB_THREAD_ALLOC(hostdata)

static void
hostent_free(void *ptr)
{
	free(ptr);
}

static void
hostent_data_free(void *ptr)
{
	struct hostent_data *hed = ptr;

	if (hed == NULL)
		return;
	hed->stayopen = 0;
	_endhosthtent(hed);
	free(hed);
}

static void
hostdata_free(void *ptr)
{
	free(ptr);
}

int
__copy_hostent(struct hostent *he, struct hostent *hptr, char *buf,
    size_t buflen)
{
	char *cp;
	char **ptr;
	int i, n;
	int nptr, len;

	/* Find out the amount of space required to store the answer. */
	nptr = 2; /* NULL ptrs */
	len = (char *)ALIGN(buf) - buf;
	for (i = 0; he->h_addr_list[i]; i++, nptr++) {
		len += he->h_length;
	}
	for (i = 0; he->h_aliases[i]; i++, nptr++) {
		len += strlen(he->h_aliases[i]) + 1;
	}
	len += strlen(he->h_name) + 1;
	len += nptr * sizeof(char*);

	if (len > buflen) {
		errno = ERANGE;
		return (-1);
	}

	/* copy address size and type */
	hptr->h_addrtype = he->h_addrtype;
	n = hptr->h_length = he->h_length;

	ptr = (char **)ALIGN(buf);
	cp = (char *)ALIGN(buf) + nptr * sizeof(char *);

	/* copy address list */
	hptr->h_addr_list = ptr;
	for (i = 0; he->h_addr_list[i]; i++ , ptr++) {
		memcpy(cp, he->h_addr_list[i], n);
		hptr->h_addr_list[i] = cp;
		cp += n;
	}
	hptr->h_addr_list[i] = NULL;
	ptr++;

	/* copy official name */
	n = strlen(he->h_name) + 1;
	strcpy(cp, he->h_name);
	hptr->h_name = cp;
	cp += n;

	/* copy aliases */
	hptr->h_aliases = ptr;
	for (i = 0 ; he->h_aliases[i]; i++) {
		n = strlen(he->h_aliases[i]) + 1;
		strcpy(cp, he->h_aliases[i]);
		hptr->h_aliases[i] = cp;
		cp += n;
	}
	hptr->h_aliases[i] = NULL;

	return (0);
}

static int
fakeaddr(const char *name, int af, struct hostent *hp, char *buf,
size_t buflen, res_state statp)
{
	struct hostent_data *hed;
	struct hostent he;

	if ((hed = __hostent_data_init()) == NULL) {
		errno = ENOMEM;
		RES_SET_H_ERRNO(statp, NETDB_INTERNAL);
		return (-1);
	}

	if ((af != AF_INET ||
	    inet_aton(name, (struct in_addr *)hed->host_addr) != 1) &&
	    inet_pton(af, name, hed->host_addr) != 1) {
		RES_SET_H_ERRNO(statp, HOST_NOT_FOUND);
		return (-1);
	}
	strncpy(hed->hostbuf, name, MAXDNAME);
	hed->hostbuf[MAXDNAME] = '\0';
	if (af == AF_INET && (statp->options & RES_USE_INET6) != 0U) {
		_map_v4v6_address((char *)hed->host_addr,
		    (char *)hed->host_addr);
		af = AF_INET6;
	}
	he.h_addrtype = af;
	switch(af) {
	case AF_INET:
		he.h_length = NS_INADDRSZ;
		break;
	case AF_INET6:
		he.h_length = NS_IN6ADDRSZ;
		break;
	default:
		errno = EAFNOSUPPORT;
		RES_SET_H_ERRNO(statp, NETDB_INTERNAL);
		return (-1);
	}
	he.h_name = hed->hostbuf;
	he.h_aliases = hed->host_aliases;
	hed->host_aliases[0] = NULL;
	hed->h_addr_ptrs[0] = (char *)hed->host_addr;
	hed->h_addr_ptrs[1] = NULL;
	he.h_addr_list = hed->h_addr_ptrs;
	RES_SET_H_ERRNO(statp, NETDB_SUCCESS);
	return (__copy_hostent(&he, hp, buf, buflen));
}

int
gethostbyname_r(const char *name, struct hostent *he, char *buffer,
    size_t buflen, struct hostent **result, int *h_errnop)
{
	res_state statp;

	statp = __res_state();
	if ((statp->options & RES_INIT) == 0 && res_ninit(statp) == -1) {
		RES_SET_H_ERRNO(statp, NETDB_INTERNAL);
		return (-1);
	}
	if (statp->options & RES_USE_INET6) {
		if (fakeaddr(name, AF_INET, he, buffer, buflen, statp) == 0) {
			*result = he;
			return (0);
		}
		if (gethostbyname_internal(name, AF_INET6, he, buffer, buflen,
		    result, h_errnop, statp) == 0)
			return (0);
	}
	return (gethostbyname_internal(name, AF_INET, he, buffer, buflen,
	    result, h_errnop, statp));
}

int
gethostbyname2_r(const char *name, int af, struct hostent *he, char *buffer,
    size_t buflen, struct hostent **result, int *h_errnop)
{
	res_state statp;

	statp = __res_state();
	if ((statp->options & RES_INIT) == 0 && res_ninit(statp) == -1) {
		RES_SET_H_ERRNO(statp, NETDB_INTERNAL);
		return (-1);
	}
	return (gethostbyname_internal(name, af, he, buffer, buflen, result,
	    h_errnop, statp));
}

int
gethostbyname_internal(const char *name, int af, struct hostent *hp, char *buf,
    size_t buflen, struct hostent **result, int *h_errnop, res_state statp)
{
	const char *cp;
	int rval, ret_errno;
	char abuf[MAXDNAME];

	static const ns_dtab dtab[] = {
		NS_FILES_CB(_ht_gethostbyname, NULL)
		{ NSSRC_DNS, _dns_gethostbyname, NULL },
		NS_NIS_CB(_nis_gethostbyname, NULL) /* force -DHESIOD */
		{ 0 }
	};

	switch (af) {
	case AF_INET:
	case AF_INET6:
		break;
	default:
		RES_SET_H_ERRNO(statp, NETDB_INTERNAL);
		*h_errnop = statp->res_h_errno;
		errno = EAFNOSUPPORT;
		return (-1);
	}

	/*
	 * if there aren't any dots, it could be a user-level alias.
	 * this is also done in res_query() since we are not the only
	 * function that looks up host names.
	 */
	if (!strchr(name, '.') &&
	    (cp = res_hostalias(statp, name, abuf, sizeof abuf)))
		name = cp;

	if (fakeaddr(name, af, hp, buf, buflen, statp) == 0) {
		*result = hp;
		return (0);
	}

	rval = _nsdispatch((void *)result, dtab, NSDB_HOSTS,
	    "gethostbyname2_r", default_src, name, af, hp, buf, buflen,
	    &ret_errno, h_errnop);

	return ((rval == NS_SUCCESS) ? 0 : -1);
}

int
gethostbyaddr_r(const void *addr, socklen_t len, int af, struct hostent *hp,
    char *buf, size_t buflen, struct hostent **result, int *h_errnop)
{
	const u_char *uaddr = (const u_char *)addr;
	const struct in6_addr *addr6;
	socklen_t size;
	int rval, ret_errno;
	res_state statp;

	static const ns_dtab dtab[] = {
		NS_FILES_CB(_ht_gethostbyaddr, NULL)
		{ NSSRC_DNS, _dns_gethostbyaddr, NULL },
		NS_NIS_CB(_nis_gethostbyaddr, NULL) /* force -DHESIOD */
		{ 0 }
	};

	statp = __res_state();
	if ((statp->options & RES_INIT) == 0 && res_ninit(statp) == -1) {
		RES_SET_H_ERRNO(statp, NETDB_INTERNAL);
		*h_errnop = statp->res_h_errno;
		return (-1);
	}

	if (af == AF_INET6 && len == NS_IN6ADDRSZ) {
		addr6 = (const struct in6_addr *)addr;
		if (IN6_IS_ADDR_LINKLOCAL(addr6)) {
			RES_SET_H_ERRNO(statp, HOST_NOT_FOUND);
			*h_errnop = statp->res_h_errno;
			return (-1);
		}
		if (IN6_IS_ADDR_V4MAPPED(addr6) ||
		    IN6_IS_ADDR_V4COMPAT(addr6)) {
			/* Unmap. */
			uaddr += NS_IN6ADDRSZ - NS_INADDRSZ;
			af = AF_INET;
			len = NS_INADDRSZ;
		}
	}
	switch (af) {
	case AF_INET:
		size = NS_INADDRSZ;
		break;
	case AF_INET6:
		size = NS_IN6ADDRSZ;
		break;
	default:
		errno = EAFNOSUPPORT;
		RES_SET_H_ERRNO(statp, NETDB_INTERNAL);
		*h_errnop = statp->res_h_errno;
		return (-1);
	}
	if (size != len) {
		errno = EINVAL;
		RES_SET_H_ERRNO(statp, NETDB_INTERNAL);
		*h_errnop = statp->res_h_errno;
		return (-1);
	}

	rval = _nsdispatch((void *)result, dtab, NSDB_HOSTS,
	    "gethostbyaddr_r", default_src, uaddr, len, af, hp, buf, buflen,
	    &ret_errno, h_errnop);

	return ((rval == NS_SUCCESS) ? 0 : -1);
}

struct hostent *
gethostbyname(const char *name)
{
	struct hostdata *hd;
	struct hostent *rval;
	int ret_h_errno;

	if ((hd = __hostdata_init()) == NULL)
		return (NULL);
	if (gethostbyname_r(name, &hd->host, hd->data, sizeof(hd->data), &rval,
	    &ret_h_errno) != 0)
		return (NULL);
	return (rval);
}

struct hostent *
gethostbyname2(const char *name, int af)
{
	struct hostdata *hd;
	struct hostent *rval;
	int ret_h_errno;

	if ((hd = __hostdata_init()) == NULL)
		return (NULL);
	if (gethostbyname2_r(name, af, &hd->host, hd->data, sizeof(hd->data),
	    &rval, &ret_h_errno) != 0)
		return (NULL);
	return (rval);
}

struct hostent *
gethostbyaddr(const void *addr, int len, int af)
{
	struct hostdata *hd;
	struct hostent *rval;
	int ret_h_errno;

	if ((hd = __hostdata_init()) == NULL)
		return (NULL);
	if (gethostbyaddr_r(addr, (socklen_t)len, af, &hd->host, hd->data,
	    sizeof(hd->data), &rval, &ret_h_errno) != 0)
		return (NULL);
	return (rval);
}

void
sethostent(int stayopen)
{
	struct hostent_data *hed;

	if ((hed = __hostent_data_init()) == NULL)
		return;
	_sethosthtent(stayopen, hed);
	_sethostdnsent(stayopen);
}

void
endhostent(void)
{
	struct hostent_data *hed;

	if ((hed = __hostent_data_init()) == NULL)
		return;
	_endhosthtent(hed);
	_endhostdnsent();
}
