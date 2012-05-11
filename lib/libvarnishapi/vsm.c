/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "miniobj.h"
#include "vas.h"
#include "vdef.h"

#include "vapi/vsm.h"
#include "vapi/vsm_int.h"
#include "vin.h"
#include "vsb.h"
#include "vsm_api.h"

#ifndef MAP_HASSEMAPHORE
#define MAP_HASSEMAPHORE 0 /* XXX Linux */
#endif


/*--------------------------------------------------------------------*/

struct VSM_data *
VSM_New(void)
{
	struct VSM_data *vd;

	ALLOC_OBJ(vd, VSM_MAGIC);
	if (vd == NULL)
		return (vd);

	vd->vsm_fd = -1;

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);
	return (vd);
}

/*--------------------------------------------------------------------*/

int
vsm_diag(struct VSM_data *vd, const char *fmt, ...)
{
	va_list ap;

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);
	AN(fmt);

	if (vd->diag == NULL)
		vd->diag = VSB_new_auto();
	AN(vd->diag);
	VSB_clear(vd->diag);
	va_start(ap, fmt);
	VSB_vprintf(vd->diag, fmt, ap);
	va_end(ap);
	AZ(VSB_finish(vd->diag));
	return (-1);
}
/*--------------------------------------------------------------------*/

const char *
VSM_Error(const struct VSM_data *vd)
{

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);

	if (vd->diag == NULL)
		return (NULL);
	else
		return (VSB_data(vd->diag));
}

/*--------------------------------------------------------------------*/

int
VSM_n_Arg(struct VSM_data *vd, const char *opt)
{

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);
	AN(opt);

	REPLACE(vd->n_opt, opt);
	if (VIN_N_Arg(vd->n_opt, NULL, NULL, &vd->fname))
		return (vsm_diag(vd, "Invalid instance name: %s\n",
		    strerror(errno)));
	return (1);
}

/*--------------------------------------------------------------------*/

const char *
VSM_Name(const struct VSM_data *vd)
{

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);

	return (vd->n_opt);
}

/*--------------------------------------------------------------------*/

void
VSM_Delete(struct VSM_data *vd)
{

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);

	VSM_Close(vd);
	free(vd->n_opt);
	free(vd->fname);
	if (vd->vsc != NULL)
		VSC_Delete(vd);
	if (vd->vsl != NULL)
		VSL_Delete(vd);
	FREE_OBJ(vd);
}

/*--------------------------------------------------------------------
 * The internal VSM open function
 *
 * Return:
 *	0 = success
 *	<0 = failure
 *
 */

/*--------------------------------------------------------------------*/

int
VSM_Open(struct VSM_data *vd)
{
	int i;
	struct VSM_head slh;
	void *v;

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);

	AZ(vd->head);
	if (!vd->n_opt)
		(void)VSM_n_Arg(vd, "");

	AZ(vd->head);
	AN(vd->fname);

	vd->vsm_fd = open(vd->fname, O_RDONLY);
	if (vd->vsm_fd < 0)
		return (vsm_diag(vd, "Cannot open %s: %s\n",
		    vd->fname, strerror(errno)));

	AZ(fstat(vd->vsm_fd, &vd->fstat));
	if (!S_ISREG(vd->fstat.st_mode)) {
		AZ(close(vd->vsm_fd));
		vd->vsm_fd = -1;
		return (vsm_diag(vd, "%s is not a regular file\n",
		    vd->fname));
	}

	i = read(vd->vsm_fd, &slh, sizeof slh);
	if (i != sizeof slh) {
		AZ(close(vd->vsm_fd));
		vd->vsm_fd = -1;
		return(vsm_diag(vd, "Cannot read %s: %s\n",
		    vd->fname, strerror(errno)));
	}

	if (memcmp(slh.marker, VSM_HEAD_MARKER, sizeof slh.marker) ||
	    slh.alloc_seq == 0) {
		AZ(close(vd->vsm_fd));
		vd->vsm_fd = -1;
		return (vsm_diag(vd, "Not a VSM file %s\n", vd->fname));
	}

	v = mmap(NULL, slh.shm_size,
	    PROT_READ, MAP_SHARED|MAP_HASSEMAPHORE, vd->vsm_fd, 0);
	if (v == MAP_FAILED) {
		AZ(close(vd->vsm_fd));
		vd->vsm_fd = -1;
		return (vsm_diag(vd, "Cannot mmap %s: %s\n",
		    vd->fname, strerror(errno)));
	}
	vd->head = v;
	vd->b = v;
	vd->e = vd->b + slh.shm_size;

	return (0);
}

/*--------------------------------------------------------------------*/

void
VSM_Close(struct VSM_data *vd)
{

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);

	if (vd->head == NULL)
		return;

	assert(vd->vsm_fd >= 0);
	AZ(munmap((void*)vd->b, vd->e - vd->b));
	vd->b = NULL;
	vd->e = NULL;
	vd->head = NULL;
	AZ(close(vd->vsm_fd));
	vd->vsm_fd = -1;
}

/*--------------------------------------------------------------------*/

int
VSM_Abandoned(const struct VSM_data *vd)
{
	struct stat st;

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);

	if (vd->head == NULL)
		return (1);

	if (!vd->head->alloc_seq)
		return (1);
	if (!stat(vd->fname, &st))
		return (1);
	if (st.st_dev != vd->fstat.st_dev)
		return (1);
	if (st.st_ino != vd->fstat.st_ino)
		return (1);
	return (0);
}

/*--------------------------------------------------------------------*/

void
VSM__iter0(const struct VSM_data *vd, struct VSM_fantom *vf)
{

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);
	AN(vf);

	memset(vf, 0, sizeof *vf);
}

/* XXX: revisit, logic is unclear */
int
VSM__itern(const struct VSM_data *vd, struct VSM_fantom *vf)
{
	void *p;

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);
	AN(vf);

	if (vd->head->alloc_seq == 0)
		return (0);	/* abandoned VSM */
	else if (vf->priv != 0) {
		if (vf->priv != vd->head->alloc_seq)
			return (0);
		if (vf->chunk->len == 0)
			return (0);
		if (vf->chunk->next == 0)
			return (0);
		p = (void*)(vd->b + vf->chunk->next);
		assert(p != vf->chunk);
		vf->chunk = p;
	} else if (vd->head->first == 0) {
		return (0);
	} else {
		AZ(vf->chunk);
		vf->chunk = (void*)(vd->b + vd->head->first);
	}
	if (memcmp(vf->chunk->marker, VSM_CHUNK_MARKER,
	    sizeof vf->chunk->marker))
		return (0);
	vf->priv = vd->head->alloc_seq;
	vf->b = (void*)(vf->chunk + 1);
	vf->e = (char*)vf->b + vf->chunk->len;

	if (vf->priv == 0)
		return (0);	/* abandoned VSM */
	if (vf->b == vf->e)
		return (0);	/* freed chunk */
	AN(vf->priv);
	AN(vf->chunk);
	return (1);
}

/*--------------------------------------------------------------------*/

int
VSM_StillValid(const struct VSM_data *vd, struct VSM_fantom *vf)
{
	struct VSM_fantom f2;

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);
	AN(vf);
	if (!vd->head)
		return (0);
	if (!vd->head->alloc_seq)
		return (0);
	if (vf->priv == vd->head->alloc_seq)
		return (1);
	VSM_FOREACH_SAFE(&f2, vd) {
		if (f2.chunk == vf->chunk && f2.b == vf->b && f2.e == vf->e) {
			vf->priv = vd->head->alloc_seq;
			return (2);
		}
	}
	return (0);
}

int
VSM_Get(const struct VSM_data *vd, struct VSM_fantom *vf,
    const char *class, const char *type, const char *ident)
{

	CHECK_OBJ_NOTNULL(vd, VSM_MAGIC);
	VSM_FOREACH_SAFE(vf, vd) {
		if (strcmp(vf->chunk->class, class))
			continue;
		if (type != NULL && strcmp(vf->chunk->type, type))
			continue;
		if (ident != NULL && strcmp(vf->chunk->ident, ident))
			continue;
		return (1);
	}
	memset(vf, 0, sizeof *vf);
	return (0);
}
