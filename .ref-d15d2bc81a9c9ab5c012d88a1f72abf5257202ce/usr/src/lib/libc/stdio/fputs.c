/* @(#)fputs.c	4.1 (Berkeley) %G% */
#include	<stdio.h>

fputs(s, iop)
register char *s;
register FILE *iop;
{
	register r;
	register c;

	while (c = *s++)
		r = putc(c, iop);
	return(r);
}
