/* parser.c
 *
 * Copyright ? 2010 - 2013 UNISYS CORPORATION
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 */

#include "parser.h"
#include "memregion.h"
#include "controlvmchannel.h"
#include <linux/ctype.h>
#include <linux/mm.h>

#define MYDRVNAME "visorchipset_parser"
#define CURRENT_FILE_PC VISOR_CHIPSET_PC_parser_c

/* We will refuse to allocate more than this many bytes to copy data from
 * incoming payloads.  This serves as a throttling mechanism.
 */
#define MAX_CONTROLVM_PAYLOAD_BYTES (1024*128)
static ulong Controlvm_Payload_Bytes_Buffered;

struct PARSER_CONTEXT_Tag {
	ulong allocbytes;
	ulong param_bytes;
	u8 *curr;
	ulong bytes_remaining;
	BOOL byte_stream;
	char data[0];
};

static PARSER_CONTEXT *
parser_init_guts(U64 addr, U32 bytes, BOOL isLocal,
		 BOOL hasStandardPayloadHeader, BOOL *tryAgain)
{
	int allocbytes = sizeof(PARSER_CONTEXT) + bytes;
	PARSER_CONTEXT *rc = NULL;
	PARSER_CONTEXT *ctx = NULL;
	MEMREGION *rgn = NULL;
	ULTRA_CONTROLVM_PARAMETERS_HEADER *phdr = NULL;

	if (tryAgain)
		*tryAgain = FALSE;
	if (!hasStandardPayloadHeader)
		/* alloc and 0 extra byte to ensure payload is
		 * '\0'-terminated
		 */
		allocbytes++;
	if ((Controlvm_Payload_Bytes_Buffered + bytes)
	    > MAX_CONTROLVM_PAYLOAD_BYTES) {
		ERRDRV("%s (%s:%d) - prevented allocation of %d bytes to prevent exceeding throttling max (%d)",
		       __func__, __FILE__, __LINE__, allocbytes,
		       MAX_CONTROLVM_PAYLOAD_BYTES);
		if (tryAgain)
			*tryAgain = TRUE;
		rc = NULL;
		goto Away;
	}
	ctx = kzalloc(allocbytes, GFP_KERNEL|__GFP_NORETRY);
	if (ctx == NULL) {
		ERRDRV("%s (%s:%d) - failed to allocate %d bytes",
		       __func__, __FILE__, __LINE__, allocbytes);
		if (tryAgain)
			*tryAgain = TRUE;
		rc = NULL;
		goto Away;
	}

	ctx->allocbytes = allocbytes;
	ctx->param_bytes = bytes;
	ctx->curr = NULL;
	ctx->bytes_remaining = 0;
	ctx->byte_stream = FALSE;
	if (isLocal) {
		void *p;
		if (addr > virt_to_phys(high_memory - 1)) {
			ERRDRV("%s - bad local address (0x%-16.16Lx for %lu)",
			       __func__,
			       (unsigned long long) addr, (ulong) bytes);
			rc = NULL;
			goto Away;
		}
		p = __va((ulong) (addr));
		memcpy(ctx->data, p, bytes);
	} else {
		rgn = visor_memregion_create(addr, bytes);
		if (!rgn) {
			rc = NULL;
			goto Away;
		}
		if (visor_memregion_read(rgn, 0, ctx->data, bytes) < 0) {
			rc = NULL;
			goto Away;
		}
	}
	if (!hasStandardPayloadHeader) {
		ctx->byte_stream = TRUE;
		rc = ctx;
		goto Away;
	}
	phdr = (ULTRA_CONTROLVM_PARAMETERS_HEADER *) (ctx->data);
	if (phdr->TotalLength != bytes) {
		ERRDRV("%s - bad total length %lu (should be %lu)",
		       __func__,
		       (ulong) (phdr->TotalLength), (ulong) (bytes));
		rc = NULL;
		goto Away;
	}
	if (phdr->TotalLength < phdr->HeaderLength) {
		ERRDRV("%s - total length < header length (%lu < %lu)",
		       __func__,
		       (ulong) (phdr->TotalLength),
		       (ulong) (phdr->HeaderLength));
		rc = NULL;
		goto Away;
	}
	if (phdr->HeaderLength < sizeof(ULTRA_CONTROLVM_PARAMETERS_HEADER)) {
		ERRDRV("%s - header is too small (%lu < %lu)",
		       __func__,
		       (ulong) (phdr->HeaderLength),
		       (ulong) (sizeof(ULTRA_CONTROLVM_PARAMETERS_HEADER)));
		rc = NULL;
		goto Away;
	}

	rc = ctx;
Away:
	if (rgn) {
		visor_memregion_destroy(rgn);
		rgn = NULL;
	}
	if (rc)
		Controlvm_Payload_Bytes_Buffered += ctx->param_bytes;
	else {
		if (ctx) {
			parser_done(ctx);
			ctx = NULL;
		}
	}
	return rc;
}

PARSER_CONTEXT *
parser_init(U64 addr, U32 bytes, BOOL isLocal, BOOL *tryAgain)
{
	return parser_init_guts(addr, bytes, isLocal, TRUE, tryAgain);
}

/* Call this instead of parser_init() if the payload area consists of just
 * a sequence of bytes, rather than a ULTRA_CONTROLVM_PARAMETERS_HEADER
 * structures.  Afterwards, you can call parser_simpleString_get() or
 * parser_byteStream_get() to obtain the data.
 */
PARSER_CONTEXT *
parser_init_byteStream(U64 addr, U32 bytes, BOOL isLocal, BOOL *tryAgain)
{
	return parser_init_guts(addr, bytes, isLocal, FALSE, tryAgain);
}

/* Obtain '\0'-terminated copy of string in payload area.
 */
char *
parser_simpleString_get(PARSER_CONTEXT *ctx)
{
	if (!ctx->byte_stream)
		return NULL;
	return ctx->data;	/* note this IS '\0'-terminated, because of
				 * the num of bytes we alloc+clear in
				 * parser_init_byteStream() */
}

/* Obtain a copy of the buffer in the payload area.
 */
void *
parser_byteStream_get(PARSER_CONTEXT *ctx, ulong *nbytes)
{
	if (!ctx->byte_stream)
		return NULL;
	if (nbytes)
		*nbytes = ctx->param_bytes;
	return (void *) ctx->data;
}

GUID
parser_id_get(PARSER_CONTEXT *ctx)
{
	ULTRA_CONTROLVM_PARAMETERS_HEADER *phdr = NULL;

	if (ctx == NULL) {
		ERRDRV("%s (%s:%d) - no context",
		       __func__, __FILE__, __LINE__);
		return Guid0;
	}
	phdr = (ULTRA_CONTROLVM_PARAMETERS_HEADER *) (ctx->data);
	return phdr->Id;
}

void
parser_param_start(PARSER_CONTEXT *ctx, PARSER_WHICH_STRING which_string)
{
	ULTRA_CONTROLVM_PARAMETERS_HEADER *phdr = NULL;

	if (ctx == NULL) {
		ERRDRV("%s (%s:%d) - no context",
		       __func__, __FILE__, __LINE__);
		goto Away;
	}
	phdr = (ULTRA_CONTROLVM_PARAMETERS_HEADER *) (ctx->data);
	switch (which_string) {
	case PARSERSTRING_INITIATOR:
		ctx->curr = ctx->data + phdr->InitiatorOffset;
		ctx->bytes_remaining = phdr->InitiatorLength;
		break;
	case PARSERSTRING_TARGET:
		ctx->curr = ctx->data + phdr->TargetOffset;
		ctx->bytes_remaining = phdr->TargetLength;
		break;
	case PARSERSTRING_CONNECTION:
		ctx->curr = ctx->data + phdr->ConnectionOffset;
		ctx->bytes_remaining = phdr->ConnectionLength;
		break;
	case PARSERSTRING_NAME:
		ctx->curr = ctx->data + phdr->NameOffset;
		ctx->bytes_remaining = phdr->NameLength;
		break;
	default:
		ERRDRV("%s - bad which_string %d", __func__, which_string);
		break;
	}

Away:
	return;
}

void
parser_done(PARSER_CONTEXT *ctx)
{
	if (!ctx)
		return;
	Controlvm_Payload_Bytes_Buffered -= ctx->param_bytes;
	kfree(ctx);
}

/** Return length of string not counting trailing spaces. */
static int
string_length_no_trail(char *s, int len)
{
	int i = len - 1;
	while (i >= 0) {
		if (!isspace(s[i]))
			return i + 1;
		i--;
	}
	return 0;
}

/** Grab the next name and value out of the parameter buffer.
 *  The entire parameter buffer looks like this:
 *      <name>=<value>\0
 *      <name>=<value>\0
 *      ...
 *      \0
 *  If successful, the next <name> value is returned within the supplied
 *  <nam> buffer (the value is always upper-cased), and the corresponding
 *  <value> is returned within a kmalloc()ed buffer, whose pointer is
 *  provided as the return value of this function.
 *  (The total number of bytes allocated is strlen(<value>)+1.)
 *
 *  NULL is returned to indicate failure, which can occur for several reasons:
 *  - all <name>=<value> pairs have already been processed
 *  - bad parameter
 *  - parameter buffer ends prematurely (couldn't find an '=' or '\0' within
 *    the confines of the parameter buffer)
 *  - the <nam> buffer is not large enough to hold the <name> of the next
 *    parameter
 */
void *
parser_param_get(PARSER_CONTEXT *ctx, char *nam, int namesize)
{
	u8 *pscan, *pnam = nam;
	ulong nscan;
	int value_length = -1, orig_value_length = -1;
	void *value = NULL;
	int i;
	int closing_quote = 0;

	if (!ctx)
		return NULL;
	pscan = ctx->curr;
	nscan = ctx->bytes_remaining;
	if (nscan == 0)
		return NULL;
	if (*pscan == '\0')
		/*  This is the normal return point after you have processed
		 *  all of the <name>=<value> pairs in a syntactically-valid
		 *  parameter buffer.
		 */
		return NULL;

	/* skip whitespace */
	while (isspace(*pscan)) {
		pscan++;
		nscan--;
		if (nscan == 0)
			return NULL;
	}

	while (*pscan != ':') {
		if (namesize <= 0) {
			ERRDRV("%s - name too big", __func__);
			return NULL;
		}
		*pnam = toupper(*pscan);
		pnam++;
		namesize--;
		pscan++;
		nscan--;
		if (nscan == 0) {
			ERRDRV("%s - unexpected end of input parsing name",
			       __func__);
			return NULL;
		}
	}
	if (namesize <= 0) {
		ERRDRV("%s - name too big", __func__);
		return NULL;
	}
	*pnam = '\0';
	nam[string_length_no_trail(nam, strlen(nam))] = '\0';

	/* point to char immediately after ":" in "<name>:<value>" */
	pscan++;
	nscan--;
	/* skip whitespace */
	while (isspace(*pscan)) {
		pscan++;
		nscan--;
		if (nscan == 0) {
			ERRDRV("%s - unexpected end of input looking for value",
			       __func__);
			return NULL;
		}
	}
	if (nscan == 0) {
		ERRDRV("%s - unexpected end of input looking for value",
		       __func__);
		return NULL;
	}
	if (*pscan == '\'' || *pscan == '"') {
		closing_quote = *pscan;
		pscan++;
		nscan--;
		if (nscan == 0) {
			ERRDRV("%s - unexpected end of input after %c",
			       __func__, closing_quote);
			return NULL;
		}
	}

	/* look for a separator character, terminator character, or
	 * end of data
	 */
	for (i = 0, value_length = -1; i < nscan; i++) {
		if (closing_quote) {
			if (pscan[i] == '\0') {
				ERRDRV("%s - unexpected end of input parsing quoted value", __func__);
				return NULL;
			}
			if (pscan[i] == closing_quote) {
				value_length = i;
				break;
			}
		} else
		    if (pscan[i] == ',' || pscan[i] == ';'
			|| pscan[i] == '\0') {
			value_length = i;
			break;
		}
	}
	if (value_length < 0) {
		if (closing_quote) {
			ERRDRV("%s - unexpected end of input parsing quoted value", __func__);
			return NULL;
		}
		value_length = nscan;
	}
	orig_value_length = value_length;
	if (closing_quote == 0)
		value_length = string_length_no_trail(pscan, orig_value_length);
	value = kmalloc(value_length + 1, GFP_KERNEL|__GFP_NORETRY);
	if (value == NULL)
		return NULL;
	memcpy(value, pscan, value_length);
	((u8 *) (value))[value_length] = '\0';

	pscan += orig_value_length;
	nscan -= orig_value_length;

	/* skip past separator or closing quote */
	if (nscan > 0) {
		if (*pscan != '\0') {
			pscan++;
			nscan--;
		}
	}

	if (closing_quote && (nscan > 0)) {
		/* we still need to skip around the real separator if present */
		/* first, skip whitespace */
		while (isspace(*pscan)) {
			pscan++;
			nscan--;
			if (nscan == 0)
				break;
		}
		if (nscan > 0) {
			if (*pscan == ',' || *pscan == ';') {
				pscan++;
				nscan--;
			} else if (*pscan != '\0') {
				ERRDRV("%s - missing separator after quoted string", __func__);
				kfree(value);
				value = NULL;
				return NULL;
			}
		}
	}
	ctx->curr = pscan;
	ctx->bytes_remaining = nscan;
	return value;
}

void *
parser_string_get(PARSER_CONTEXT *ctx)
{
	u8 *pscan;
	ulong nscan;
	int value_length = -1;
	void *value = NULL;
	int i;

	if (!ctx)
		return NULL;
	pscan = ctx->curr;
	nscan = ctx->bytes_remaining;
	if (nscan == 0)
		return NULL;
	if (!pscan)
		return NULL;
	for (i = 0, value_length = -1; i < nscan; i++)
		if (pscan[i] == '\0') {
			value_length = i;
			break;
		}
	if (value_length < 0)	/* '\0' was not included in the length */
		value_length = nscan;
	value = kmalloc(value_length + 1, GFP_KERNEL|__GFP_NORETRY);
	if (value == NULL)
		return NULL;
	if (value_length > 0)
		memcpy(value, pscan, value_length);
	((u8 *) (value))[value_length] = '\0';
	return value;
}
