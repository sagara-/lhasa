/*

Copyright (c) 2011, Simon Howard

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

#include "lha_input_stream.h"

// Maximum length of the self-extractor header.
// If we don't find an LHA file header after this many bytes, give up.

#define MAX_SFX_HEADER_LEN 8192

// Size of the lead-in buffer used to skip the self-extractor.

#define LEADIN_BUFFER_LEN 24

// Magic string to detect an Amiga LhASFX self-extracting file.
// This type of self-extractor is special because the program itself
// contains a mini-LHA file that must be skipped over to get to
// the real one.

#define AMIGA_LHASFX_ID "LhASFX V1.2,"

typedef enum {
	LHA_INPUT_STREAM_INIT,
	LHA_INPUT_STREAM_READING,
	LHA_INPUT_STREAM_FAIL
} LHAInputStreamState;

struct _LHAInputStream {
	const LHAInputStreamType *type;
	void *handle;
	LHAInputStreamState state;
	uint8_t leadin[LEADIN_BUFFER_LEN];
	size_t leadin_len;
};

LHAInputStream *lha_input_stream_new(const LHAInputStreamType *type,
                                     void *handle)
{
	LHAInputStream *result;

	result = malloc(sizeof(LHAInputStream));

	if (result == NULL) {
		return NULL;
	}

	result->type = type;
	result->handle = handle;
	result->leadin_len = 0;
	result->state = LHA_INPUT_STREAM_INIT;

	return result;
}

void lha_input_stream_free(LHAInputStream *stream)
{
	// Close the input stream.

	if (stream->type->close != NULL) {
		stream->type->close(stream->handle);
	}

	free(stream);
}

// Check if the specified buffer is the start of a file header.

static int file_header_match(uint8_t *buf)
{
	if (buf[2] != '-' || buf[6] != '-') {
		return 0;
	}

	// LHA algorithm?

	if (!strncmp((char *) buf + 3, "lh", 2)) {
		return 1;
	}

	// LArc algorithm (lz4, lz5, lzs)?

	if (!strncmp((char *) buf + 3, "lz", 2)
	 && (buf[5] == '4' || buf[5] == '5' || buf[5] == 's')) {
		return 1;
	}

	// PMarc algorithm? (pm0, pm2)
	// Note: PMarc SFX archives have a -pms- string in them that must
	// be ignored.

	if (!strncmp((char *) buf + 3, "pm", 2) && buf[5] != 's') {
		return 1;
	}

	return 0;
}

// Empty some of the bytes from the start of the lead-in buffer.

static void empty_leadin(LHAInputStream *stream, size_t bytes)
{
	memmove(stream->leadin, stream->leadin + bytes,
	        stream->leadin_len - bytes);
	stream->leadin_len -= bytes;
}

// Read bytes from the input stream into the specified buffer.

static int do_read(LHAInputStream *stream, void *buf, size_t buf_len)
{
	return stream->type->read(stream->handle, buf, buf_len);
}

// Skip the self-extractor header at the start of the file.
// Returns non-zero if a header was found.

static int skip_sfx(LHAInputStream *stream)
{
	size_t filepos;
	unsigned int i;
	int skip_files;
	int read;

	filepos = 0;
	skip_files = 0;

	while (filepos < MAX_SFX_HEADER_LEN) {

		// Add some more bytes to the lead-in buffer:

		read = do_read(stream, stream->leadin + stream->leadin_len,
		               LEADIN_BUFFER_LEN - stream->leadin_len);

		if (read <= 0) {
			break;
		}

		stream->leadin_len += (unsigned int) read;

		// Check the lead-in buffer for a file header.

		for (i = 0; i + 12 < stream->leadin_len; ++i) {
			if (file_header_match(stream->leadin + i)) {
				if (skip_files == 0) {
					empty_leadin(stream, i);
					return 1;
				} else {
					--skip_files;
				}
			}

			// Detect Amiga self-extractor.

			if (!memcmp(stream->leadin + i, AMIGA_LHASFX_ID,
			            strlen(AMIGA_LHASFX_ID))) {
				skip_files = 1;
			}
		}

		empty_leadin(stream, i);
		filepos += i;
	}

	return 0;
}

int lha_input_stream_read(LHAInputStream *stream, void *buf, size_t buf_len)
{
	size_t total_bytes, n;
	int result;

	// Start of the stream?  Skip self-extract header, if there is one.

	if (stream->state == LHA_INPUT_STREAM_INIT) {
		if (skip_sfx(stream)) {
			stream->state = LHA_INPUT_STREAM_READING;
		} else {
			stream->state = LHA_INPUT_STREAM_FAIL;
		}
	}

	if (stream->state == LHA_INPUT_STREAM_FAIL) {
		return 0;
	}

	// Now fill the result buffer. Start by emptying the lead-in buffer.

	total_bytes = 0;

	if (stream->leadin_len > 0) {
		if (buf_len < stream->leadin_len) {
			n = buf_len;
		} else {
			n = stream->leadin_len;
		}

		memcpy(buf, stream->leadin, n);
		empty_leadin(stream, n);
		total_bytes += n;
	}

	// Read from the input stream.

	if (total_bytes < buf_len) {
		result = do_read(stream, (uint8_t *) buf + total_bytes,
		                 buf_len - total_bytes);

		if (result > 0) {
			total_bytes += (unsigned int) result;
		}
	}

	// Only successful if the complete buffer is filled.

	return total_bytes == buf_len;
}

int lha_input_stream_read_byte(LHAInputStream *stream, uint8_t *result)
{
	return lha_input_stream_read(stream, result, 1);
}

int lha_input_stream_read_short(LHAInputStream *stream, uint16_t *result)
{
	uint8_t buf[2];

	if (!lha_input_stream_read(stream, buf, 2)) {
		return 0;
	}

	*result = (uint16_t) (buf[0] | (buf[1] << 8));

	return 1;
}

int lha_input_stream_read_long(LHAInputStream *stream, uint32_t *result)
{
	uint8_t buf[4];

	if (!lha_input_stream_read(stream, buf, 4)) {
		return 0;
	}

	*result = (uint32_t) (buf[0] | (buf[1] << 8)
	                    | (buf[2] << 16) | (buf[3] << 24));

	return 1;
}

int lha_input_stream_skip(LHAInputStream *stream, size_t bytes)
{
	// If we have a dedicated skip function, use it; otherwise,
	// the read function can be used to perform a skip.

	if (stream->type->skip != NULL) {
		return stream->type->skip(stream->handle, bytes);
	} else {
		uint8_t data[32];
		unsigned int len;
		int result;

		while (bytes > 0) {

			// Read as many bytes left as possible to fit in
			// the buffer:

			if (bytes > sizeof(data)) {
				len = sizeof(data);
			} else {
				len = bytes;
			}

			result = do_read(stream, data, len);

			if (result < 0) {
				return 0;
			}

			bytes -= (unsigned int) result;
		}

		return 1;
	}
}

// Read data from a FILE * source.

static int file_source_read(void *handle, void *buf, size_t buf_len)
{
	size_t bytes_read;
	FILE *fh = handle;

	bytes_read = fread(buf, 1, buf_len, fh);

	// If an error occurs, zero is returned; however, it may also
	// indicate end of file.

	if (bytes_read == 0 && !feof(fh)) {
		return -1;
	}

	return (int) bytes_read;
}

// Seek forward in a FILE * input stream.

static int file_source_seek(void *handle, size_t bytes)
{
	return fseek(handle, (long) bytes, SEEK_CUR);
}

// Close a FILE * input stream.

static void file_source_close(void *handle)
{
	fclose(handle);
}

// "Owned" file source - the stream will be closed when the input
// stream is freed.

static const LHAInputStreamType file_source_owned = {
	file_source_read,
	file_source_seek,
	file_source_close
};

// "Unowned" file source - the stream is owned by the calling code.

static const LHAInputStreamType file_source_unowned = {
	file_source_read,
	file_source_seek,
	NULL
};

LHAInputStream *lha_input_stream_from(char *filename)
{
	FILE *fstream;

	fstream = fopen(filename, "rb");

	if (fstream == NULL) {
		return NULL;
	}

	return lha_input_stream_new(&file_source_owned, fstream);
}

LHAInputStream *lha_input_stream_from_FILE(FILE *stream)
{
	return lha_input_stream_new(&file_source_unowned, stream);
}

