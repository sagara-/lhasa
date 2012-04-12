/*

Copyright (c) 2011, 2012, Simon Howard

Permission to use, copy, modify, and/or distribute this software
for any purpose with or without fee is hereby granted, provided
that the above copyright notice and this permission notice appear
in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "lha_endian.h"
#include "lha_file_header.h"
#include "ext_header.h"

#define COMMON_HEADER_LEN 22 /* bytes */
#define LEVEL_0_MIN_HEADER_LEN 22 /* bytes */
#define LEVEL_1_MIN_HEADER_LEN 25 /* bytes */

#define RAW_DATA(hdr_ptr, off)  ((*hdr_ptr)->raw_data[off])
#define RAW_DATA_LEN(hdr_ptr)   ((*hdr_ptr)->raw_data_len)

// Perform checksum of header contents.

static int checksum_header(uint8_t *header, size_t header_len, size_t csum)
{
	unsigned int result;
	unsigned int i;

	result = 0;

	for (i = 0; i < header_len; ++i) {
		result += header[i];
	}

	return (result & 0xff) == csum;
}

// Decode MS-DOS timestamp.

static unsigned int decode_ftime(uint8_t *buf)
{
	int raw;
	struct tm datetime;

	raw = (int) lha_decode_uint32(buf);

	if (raw == 0) {
		return 0;
	}

	datetime.tm_sec = (raw << 1) & 0x3e;
	datetime.tm_min = (raw >> 5) & 0x3f;
	datetime.tm_hour = (raw >> 11) & 0x1f;
	datetime.tm_mday = (raw >> 16) & 0x1f;
	datetime.tm_mon = ((raw >> 21) & 0xf) - 1;
	datetime.tm_year = 80 + ((raw >> 25) & 0x7f);
	datetime.tm_wday = 0;
	datetime.tm_yday = 0;
	datetime.tm_isdst = -1;

	return (unsigned int) mktime(&datetime);
}

// Fix up MS-DOS path: translate all-caps to lower case and replace
// DOS-style \ directory separator with /

static void fixup_msdos_path(char *path, int lowercase)
{
	unsigned int i;

	for (i = 0; path[i] != '\0'; ++i) {
		if (path[i] == '\\') {
			path[i] = '/';
		}

		if (lowercase) {
			path[i] = (char) tolower(path[i]);
		}
	}
}

// Decode the path field in the header.

static int process_level0_path(LHAFileHeader *header, uint8_t *data,
                               size_t data_len)
{
	uint8_t *filename;
	size_t filename_len;
	unsigned int i;
	int is_allcaps;
	uint8_t *sep;

	// Is the path an all-caps filename?  If so, it is a DOS path that
	// should be translated to lower case.

	if (header->os_type == LHA_OS_TYPE_UNKNOWN
	 || header->os_type == LHA_OS_TYPE_MSDOS
	 || header->os_type == LHA_OS_TYPE_ATARI
	 || header->os_type == LHA_OS_TYPE_OS2) {
		is_allcaps = 1;

		for (i = 0; i < data_len; ++i) {
			if (islower(data[i])) {
				is_allcaps = 0;
				break;
			}
		}
	} else {
		is_allcaps = 0;
	}

	// Is there a directory separator in the path?  If so, we need to
	// split into directory name and filename.

	sep = NULL;

	for (i = 0; i < data_len; ++i) {
		if (data[i] == '\\') {
			sep = data + i;
		}
	}

	if (sep != NULL) {
		header->path = malloc((size_t) (sep - data) + 2);

		if (header->path == NULL) {
			return 0;
		}

		memcpy(header->path, data, (size_t) (sep - data) + 1);
		header->path[sep - data + 1] = '\0';
		fixup_msdos_path(header->path, is_allcaps);

		filename = sep + 1;
		filename_len = data_len - 1 - (size_t) (sep - data);
	} else {
		filename = data;
		filename_len = data_len;
	}

	// Allocate filename buffer:

	header->filename = malloc(filename_len + 1);

	if (header->filename == NULL) {
		return 0;
	}

	memcpy(header->filename, filename, filename_len);
	header->filename[filename_len] = '\0';
	fixup_msdos_path(header->filename, is_allcaps);

	return 1;
}

// Read some more data from the input stream, extending the raw_data
// array (and the size of the header).

static uint8_t *extend_raw_data(LHAFileHeader **header,
                                LHAInputStream *stream,
                                size_t nbytes)
{
	LHAFileHeader *new_header;
	size_t new_raw_len;
	uint8_t *result;

	// Reallocate the header and raw_data area to be larger.

	new_raw_len = RAW_DATA_LEN(header) + nbytes;
	new_header = realloc(*header, sizeof(LHAFileHeader) + new_raw_len);

	if (new_header == NULL) {
		return NULL;
	}

	// Update the header pointer to point to the new area.

	*header = new_header;
	new_header->raw_data = (uint8_t *) (new_header + 1);
	result = new_header->raw_data + new_header->raw_data_len;

	// Read data from stream into new area.

	if (!lha_input_stream_read(stream, result, nbytes)) {
		return NULL;
	}

	new_header->raw_data_len = new_raw_len;

	return result;
}

static int read_next_ext_header(LHAFileHeader **header,
                                LHAInputStream *stream,
                                uint8_t **ext_header,
                                size_t *ext_header_len)
{
	// Last two bytes of the header raw data contain the size
	// of the next header.

	*ext_header_len
	    = lha_decode_uint16(&RAW_DATA(header, RAW_DATA_LEN(header) - 2));

	// No more headers?

	if (*ext_header_len == 0) {
		*ext_header = NULL;
		return 1;
	}

	*ext_header = extend_raw_data(header, stream, *ext_header_len);

	return *ext_header != NULL;
}

static int decode_extended_headers(LHAFileHeader **header,
                                   LHAInputStream *stream)
{
	uint8_t *ext_header;
	size_t ext_header_len;

	for (;;) {
		// Try to read the next header.

		if (!read_next_ext_header(header, stream,
		                          &ext_header, &ext_header_len)) {
			return 0;
		}

		// Last header?

		if (ext_header_len == 0) {
			break;
		}

		// In level 1 headers, the compressed length field is
		// actually "compressed length + length of all extended
		// headers":

		if ((*header)->header_level <= 1) {
			(*header)->compressed_length -= ext_header_len;
		}

		// Must be at least 3 bytes - 1 byte header type
		// + 2 bytes for next header length

		if (ext_header_len < 3) {
			return 0;
		}

		// Process header:

		lha_ext_header_decode(*header, ext_header[0],
		                      ext_header + 1, ext_header_len - 3);
	}

	return 1;
}

// Decode a level 0 or 1 header.

static int decode_level0_header(LHAFileHeader **header, LHAInputStream *stream)
{
	uint8_t header_len;
	uint8_t header_csum;
	size_t path_len;
	size_t min_len;

	header_len = RAW_DATA(header, 0);
	header_csum = RAW_DATA(header, 1);

	// Sanity check header length.  This is the minimum header length
	// for a header that has a zero-length path.

	switch ((*header)->header_level) {
		case 0:
			min_len = LEVEL_0_MIN_HEADER_LEN;
			break;
		case 1:
			min_len = LEVEL_1_MIN_HEADER_LEN;
			break;

		default:
			return 0;
	}

	if (header_len < min_len) {
		return 0;
	}

	// We only have a partial header so far. Read the full header.

	if (!extend_raw_data(header, stream,
	                     header_len + 2 - RAW_DATA_LEN(header))) {
		return 0;
	}

	// Checksum the header.

	if (!checksum_header(&RAW_DATA(header, 2),
	                     RAW_DATA_LEN(header) - 2,
	                     header_csum)) {
		return 0;
	}

	// Compression method:

	memcpy((*header)->compress_method, &RAW_DATA(header, 2), 5);
	(*header)->compress_method[5] = '\0';

	// File lengths:

	(*header)->compressed_length = lha_decode_uint32(&RAW_DATA(header, 7));
	(*header)->length = lha_decode_uint32(&RAW_DATA(header, 11));

	// Timestamp:

	(*header)->timestamp = decode_ftime(&RAW_DATA(header, 15));

	// Read path.  Check path length field - is the header long enough
	// to hold this full path?

	path_len = RAW_DATA(header, 21);

	if (min_len + path_len > header_len) {
		return 0;
	}

	// OS type?

	if ((*header)->header_level == 0) {
		(*header)->os_type = LHA_OS_TYPE_UNKNOWN;
	} else {
		(*header)->os_type = RAW_DATA(header, 24 + path_len);
	}

	// Read filename field:

	if (!process_level0_path(*header, &RAW_DATA(header, 22), path_len)) {
		return 0;
	}

	// CRC field.

	(*header)->crc = lha_decode_uint16(&RAW_DATA(header, 22 + path_len));

	return 1;
}

static int decode_level1_header(LHAFileHeader **header, LHAInputStream *stream)
{
	if (!decode_level0_header(header, stream)) {
		return 0;
	}

	// Level 1 headers can have extended headers, so parse them.

	if (!decode_extended_headers(header, stream)) {
		return 0;
	}

	return 1;
}

LHAFileHeader *lha_file_header_read(LHAInputStream *stream)
{
	LHAFileHeader *header;
	int success;

	// We cannot decode the file header until we identify the
	// header level (as different header level formats are
	// decoded in different ways. The header level field is
	// located at byte offset 20 within the header, so we
	// must read the first 21 bytes to read it (actually this
	// reads one byte more, so that we get the filename length
	// byte for level 1 headers as well).

	// Allocate result structure.

	header = malloc(sizeof(LHAFileHeader) + COMMON_HEADER_LEN);

	if (header == NULL) {
		return NULL;
	}

	memset(header, 0, sizeof(LHAFileHeader));

	header->_refcount = 1;

	// Read first 21 bytes of header.

	header->raw_data = (uint8_t *) (header + 1);
	header->raw_data_len = COMMON_HEADER_LEN;

	if (!lha_input_stream_read(stream, header->raw_data,
	                           header->raw_data_len)) {
		goto fail;
	}

	// Identify header level, and decode header depending on
	// the value encountered.

	header->header_level = header->raw_data[20];

	switch (header->header_level) {
		case 0:
			success = decode_level0_header(&header, stream);
			break;

		case 1:
			success = decode_level1_header(&header, stream);
			break;

		default:
			success = 0;
			break;
	}

	if (!success) {
		goto fail;
	}

	return header;
fail:
	lha_file_header_free(header);
	return NULL;
}

void lha_file_header_free(LHAFileHeader *header)
{
	// Sanity check:

	if (header->_refcount == 0) {
		return;
	}

	// Count down references and only free when all have been removed.

	--header->_refcount;

	if (header->_refcount > 0) {
		return;
	}

	free(header->filename);
	free(header->path);
	free(header->unix_username);
	free(header->unix_group);
	free(header);
}

void lha_file_header_add_ref(LHAFileHeader *header)
{
	++header->_refcount;
}

