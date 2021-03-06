/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2014 Varnish Software AS
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

#include "cache/cache.h"
#include "cache/cache_filter.h"
#include "cache_http1.h"

/*--------------------------------------------------------------------*/

static int __match_proto__(vdp_bytes)
v1d_bytes(struct req *req, enum vdp_action act, void **priv,
    const void *ptr, ssize_t len)
{
	ssize_t wl = 0;

	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	(void)priv;
	if (act == VDP_INIT || act == VDP_FINI)
		return (0);

	AZ(req->vdp_nxt);		/* always at the bottom of the pile */

	if (len > 0)
		wl = V1L_Write(req->wrk, ptr, len);
	req->acct.resp_bodybytes += len;
	if (act > VDP_NULL && V1L_Flush(req->wrk))
		return (-1);
	if (len != wl)
		return (-1);
	return (0);
}

/*--------------------------------------------------------------------
 */

void __match_proto__(vtr_deliver_f)
V1D_Deliver(struct req *req, struct busyobj *bo, int wantbody)
{
	enum objiter_status ois;

	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_ORNULL(bo, BUSYOBJ_MAGIC);
	CHECK_OBJ_NOTNULL(req->objcore, OBJCORE_MAGIC);

	if (wantbody) {
		http_Unset(req->resp, H_Content_Length);
		if (req->resp_len >= 0)
			http_PrintfHeader(req->resp,
			    "Content-Length: %jd", req->resp_len);
	}

	if (req->resp_len == 0)
		wantbody = 0;
	else if (http_GetHdr(req->resp, H_Content_Length, NULL))
		req->res_mode |= RES_LEN;
	else if (wantbody) {
		if (req->http->protover == 11) {
			req->res_mode |= RES_CHUNKED;
			http_SetHeader(req->resp, "Transfer-Encoding: chunked");
		} else {
			req->res_mode |= RES_EOF;
			req->doclose = SC_TX_EOF;
		}
	}

	VSLb(req->vsl, SLT_Debug, "RES_MODE %x", req->res_mode);

	if (req->doclose) {
		if (!http_HdrIs(req->resp, H_Connection, "close")) {
			http_Unset(req->resp, H_Connection);
			http_SetHeader(req->resp, "Connection: close");
		}
	} else if (!http_GetHdr(req->resp, H_Connection, NULL))
		http_SetHeader(req->resp, "Connection: keep-alive");

	VDP_push(req, v1d_bytes, NULL, 1);

	V1L_Reserve(req->wrk, req->ws, &req->sp->fd, req->vsl, req->t_prev);

	req->acct.resp_hdrbytes += HTTP1_Write(req->wrk, req->resp, HTTP1_Resp);
	if (DO_DEBUG(DBG_FLUSH_HEAD))
		(void)V1L_Flush(req->wrk);

	ois = OIS_DONE;
	if (wantbody) {
		if (req->res_mode & RES_CHUNKED)
			V1L_Chunked(req->wrk);
		ois = VDP_DeliverObj(req);
		if (ois == OIS_DONE && (req->res_mode & RES_CHUNKED))
			V1L_EndChunk(req->wrk);
	}

	if ((V1L_FlushRelease(req->wrk) || ois != OIS_DONE) && req->sp->fd >= 0)
		SES_Close(req->sp, SC_REM_CLOSE);
	VDP_close(req);
}
