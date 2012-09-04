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
 *
 * Handle backend connections and backend request structures.
 *
 */

#include "config.h"

#include "cache.h"

#include "cache_backend.h"
#include "vtcp.h"

/* Close a connection ------------------------------------------------*/

void
VDI_CloseFd(struct vbc **vbp)
{
	struct backend *bp;
	struct vbc *vc;

	AN(vbp);
	vc = *vbp;
	*vbp = NULL;
	CHECK_OBJ_NOTNULL(vc, VBC_MAGIC);
	CHECK_OBJ_NOTNULL(vc->backend, BACKEND_MAGIC);
	assert(vc->fd >= 0);

	bp = vc->backend;

	VSLb(vc->vsl, SLT_BackendClose, "%s", bp->display_name);

	/*
	 * Checkpoint log to flush all info related to this connection
	 * before the OS reuses the FD
	 */
	VSL_Flush(vc->vsl, 0);
	vc->vsl = NULL;

	VTCP_close(&vc->fd);
	VBE_DropRefConn(bp);
	vc->backend = NULL;
	VBE_ReleaseConn(vc);
}

/* Recycle a connection ----------------------------------------------*/

void
VDI_RecycleFd(struct vbc **vbp)
{
	struct backend *bp;
	struct vbc *vc;

	AN(vbp);
	vc = *vbp;
	*vbp = NULL;
	CHECK_OBJ_NOTNULL(vc, VBC_MAGIC);
	CHECK_OBJ_NOTNULL(vc->backend, BACKEND_MAGIC);
	assert(vc->fd >= 0);

	bp = vc->backend;

	VSLb(vc->vsl, SLT_BackendReuse, "%s", bp->display_name);

	/* XXX: revisit this hack */
	VSL_Flush(vc->vsl, 0);
	vc->vsl = NULL;

	Lck_Lock(&bp->mtx);
	VSC_C_main->backend_recycle++;
	VTAILQ_INSERT_HEAD(&bp->connlist, vc, list);
	VBE_DropRefLocked(bp);
}

/* Get a connection --------------------------------------------------*/

struct vbc *
VDI_GetFd(const struct director *d, struct req *req)
{
	struct vbc *vc;

	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	if (d == NULL)
		d = req->director;
	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	vc = d->getfd(d, req);
	if (vc != NULL)
		vc->vsl = req->busyobj->vsl;
	return (vc);
}

/* Check health ------------------------------------------------------
 *
 * The target is really an objhead pointer, but since it can not be
 * dereferenced during health-checks, we pass it as uintptr_t, which
 * hopefully will make people investigate, before mucking about with it.
 */

int
VDI_Healthy(const struct director *d, const struct req *req)
{

	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	return (d->healthy(d, req));
}
