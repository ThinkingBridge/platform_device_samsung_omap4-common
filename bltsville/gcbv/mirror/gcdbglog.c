/*
 * Copyright(c) 2012,
 * Texas Instruments, Inc. and Vivante Corporation.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Vivante Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <gcx.h>
#include "gcmain.h"

#if ANDROID
#include <cutils/log.h>
#endif

#if GCDEBUG_ENABLE

/*******************************************************************************
 * Debug switches.
 */

/* Dumping enable default state. */
#define GC_DUMP_ENABLE		0

/* Ignore all zones as if they were all enabled in all modules. */
#define GC_IGNORE_ZONES		0

/* When enabled, all output is collected into a buffer with a predefined size.
 * GC_DUMP_BUFFER_SIZE determines the size of the buffer and GC_ENABLE_OVERFLOW
 * controls what happens when the buffer gets full. */
#define GC_BUFFERED_OUTPUT	0

/* Debug output buffer size. */
#define GC_DUMP_BUFFER_SIZE	(200 * 1024)

/* If disabled, the contents of the buffer will be dumped to the console when
 * the buffer gets full.
 * If enabled, wrap around mode is enabled where when the buffer gets full,
 * the oldest entries are overwritten with the new entrie. To dump the buffer
 * to the console gc_dump_flush must be called explicitly. */
#define GC_ENABLE_OVERFLOW	1

/* Specifies how many prints are accumulated in the buffer before the buffer is
 * flushed. Set to zero to disable auto dumping mode. */
#define GC_FLUSH_COUNT		0

/* Specifies the maximum number of threads that will be tracked in an attempt
 * to visually separate messages from different threads. To disable thread
 * tracking, set to 0 or 1. */
#define GC_THREAD_COUNT		20

/* Specifies spacing for thread messages. */
#define GC_THREAD_INDENT	0

/* When set to non-zero, specifies how many prints are accumulated in the
 * buffer before the buffer is flushed. */
#define GC_SHOW_DUMP_LINE	1

/* If enabled, each print statement will be preceeded with the current
 * process ID. */
#define GC_SHOW_PID		1

/* If enabled, internal logging validation code is turned on. */
#define GC_DEBUG_SELF		0

/* Maximum length of a dump string. */
#define GC_MAXSTR_LENGTH	256

/* Print buffers like C arrays. */
#define GC_C_BUFFER		0


/*******************************************************************************
 * Miscellaneous macros.
 */

#define GC_PTR2INT(p) \
( \
	(unsigned int) (p) \
)

#define GC_ALIGN(n, align) \
( \
	((n) + ((align) - 1)) & ~((align) - 1) \
)

#define GC_PTRALIGNMENT(p, alignment) \
( \
	GC_ALIGN(GC_PTR2INT(p), alignment) - GC_PTR2INT(p) \
)

#define GC_VARARG_ALIGNMENT sizeof(unsigned long long)

#if defined(GCDBGFILTER)
#undef GCDBGFILTER
#endif

#define GCDBGFILTER \
	(*filter)

#if GC_IGNORE_ZONES
#define GC_VERIFY_ENABLE(filter, zone) \
	(g_initdone)
#else
#define GC_VERIFY_ENABLE(filter, zone) \
	(g_initdone && ((filter == NULL) || ((filter->zone & zone) != 0)))
#endif

#if GC_SHOW_DUMP_LINE
#define GC_DUMPLINE_FORMAT "[%12d] "
#endif

#if GC_SHOW_PID
#define GC_PID_FORMAT "[pid=%04X] "
#endif

#define GC_EOL_RESERVE 1


/*******************************************************************************
 * Dump item header definition.
 */

enum itemtype {
	GC_BUFITEM_NONE,
	GC_BUFITEM_STRING,
	GC_BUFITEM_BUFFER
};

/* Common item head/buffer terminator. */
struct itemhead {
	enum itemtype type;
};


/*******************************************************************************
 * Supported dump items.
 */

/* GC_BUFITEM_STRING: buffered string. */
struct itemstring {
	enum itemtype itemtype;
	int indent;

#if GC_SHOW_PID
	pid_t pid;
#endif

#if GC_SHOW_DUMP_LINE
	unsigned int dumpline;
#endif

	const char *message;
	va_list messagedata;
	unsigned int datasize;
};

/* GC_BUFITEM_BUFFER: buffered memory. */
enum buffertype {
	GC_BUFTYPE_GENERIC,
	GC_BUFTYPE_COMMAND,
	GC_BUFTYPE_SURFACE
};

#define GC_GENERIC_DATA_COUNT 8
#define GC_SURFACE_DATA_COUNT 64

struct itembuffer {
	enum itemtype itemtype;
	enum buffertype buffertype;
	int indent;

	unsigned int surfwidth;
	unsigned int surfheight;
	unsigned int surfbpp;
	unsigned int x1, y1;
	unsigned int x2, y2;

	unsigned int datasize;
	unsigned int gpuaddr;
};


/*******************************************************************************
 * Debug output buffer.
 */

struct threadinfo {
	pid_t pid;
	int msgindent;
	int threadindent;
};

struct buffout {
	int enable;

#if GC_THREAD_COUNT > 1
	unsigned int threadcount;
	struct threadinfo threadinfo[1 + GC_THREAD_COUNT];
#else
	struct threadinfo threadinfo[1];
#endif

#if GC_SHOW_DUMP_LINE
	unsigned int dumpline;
#endif

#if GC_BUFFERED_OUTPUT
	int start;
	int index;
	int count;
	unsigned char *buffer;
#endif
};

static struct buffout g_outputbuffer = {
	.enable = GC_DUMP_ENABLE
};


/*******************************************************************************
 * Globals.
 */

static unsigned int g_initdone;
static GCDEFINE_LOCK(g_lockmutex);
static struct list_head gc_filterlist = LIST_HEAD_INIT(gc_filterlist);


/*******************************************************************************
 * Item size functions.
 */

#if GC_BUFFERED_OUTPUT
static int get_item_size_terminator(struct itemhead *item)
{
	return sizeof(struct itemhead);
}

static int get_item_size_string(struct itemhead *item)
{
	struct itemstring *itemstring = (struct itemstring *) item;
	unsigned int vlen = *((unsigned char **) &itemstring->messagedata)
					- ((unsigned char *) itemstring);
	return vlen + itemstring->datasize;
}

static int get_item_size_buffer(struct itemhead *item)
{
	struct itembuffer *itembuffer = (struct itembuffer *) item;
	return sizeof(struct itembuffer) + itembuffer->datasize;
}

#if GC_ENABLE_OVERFLOW
typedef int (*getitemsize) (struct itemhead *item);

static getitemsize g_itemsize[] = {
	get_item_size_terminator,
	get_item_size_string,
	get_item_size_buffer
};
#endif
#endif


/*******************************************************************************
 * Printing functions.
 */

#if ANDROID
#define GC_PRINTK(s, fmt, ...) \
	LOGI(fmt, ##__VA_ARGS__);
#else
#define GC_PRINTK(s, fmt, ...) \
	printf(fmt, ##__VA_ARGS__);
#endif

#if GC_DEBUG_SELF
#	define GC_DEBUGMSG(fmt, ...) \
		GC_PRINTK(NULL, "[%s:%d] " fmt, __func__, __LINE__, \
			  ##__VA_ARGS__)
#else
#	define GC_DEBUGMSG(...) {}
#endif

static struct threadinfo *get_threadinfo(struct buffout *buffout)
{
#if GC_THREAD_COUNT > 1
	struct threadinfo *threadinfo;
	unsigned int i, count;
	pid_t pid;

	/* Get current pid. */
	pid = gettid();

	/* Try to locate thread record. */
	count = buffout->threadcount + 1;
	for (i = 1; i < count; i += 1)
		if (buffout->threadinfo[i].pid == pid)
			return &buffout->threadinfo[i];

	/* Not found, still have room? */
	if (buffout->threadcount < GC_THREAD_COUNT) {
		threadinfo = &buffout->threadinfo[count];
		threadinfo->pid = pid;
		threadinfo->msgindent = 0;
		threadinfo->threadindent = buffout->threadcount
					 * GC_THREAD_INDENT;
		buffout->threadcount += 1;
		return threadinfo;
	}

	/* Too many threads, use the common entry. */
	GC_PRINTK(NULL, "%s(%d) [ERROR] reached the maximum thread number.\n",
		  __func__, __LINE__);
	threadinfo = buffout->threadinfo;
	threadinfo->pid = pid;
	return threadinfo;
#else
	struct threadinfo *threadinfo;
	threadinfo = buffout->threadinfo;

#if GC_SHOW_PID
	threadinfo->pid = pid;
#else
	threadinfo->pid = 0;
#endif

	return threadinfo;
#endif
}

static int gc_get_indent(int indent, char *buffer, int buffersize)
{
	static const int MAX_INDENT = 80;
	int len, _indent;

	_indent = indent % MAX_INDENT;
	if (_indent > buffersize)
		_indent = buffersize - 1;

	for (len = 0; len < _indent; len += 1)
		buffer[len] = ' ';

	buffer[len] = '\0';
	return len;
}

static void gc_print_string(struct seq_file *s, struct itemstring *str)
{
	int len = 0;
	char buffer[GC_MAXSTR_LENGTH];

#if GC_SHOW_DUMP_LINE
	len += snprintf(buffer + len, sizeof(buffer) - len - GC_EOL_RESERVE,
			GC_DUMPLINE_FORMAT, str->dumpline);
#endif

#if GC_SHOW_PID
	len += snprintf(buffer + len, sizeof(buffer) - len - GC_EOL_RESERVE,
			GC_PID_FORMAT, str->pid);
#endif

	/* Append the indent string. */
	len += gc_get_indent(str->indent, buffer + len,
			     sizeof(buffer) - len - GC_EOL_RESERVE);

	/* Format the string. */
	len += vsnprintf(buffer + len, sizeof(buffer) - len - GC_EOL_RESERVE,
			 str->message, str->messagedata);

	/* Add end-of-line if missing. */
	if (buffer[len - 1] != '\n')
		buffer[len++] = '\n';
	buffer[len] = '\0';

	/* Print the string. */
	GC_PRINTK(s, "%s", buffer);
}

static void gc_print_generic(struct seq_file *s, struct itembuffer *item,
			     unsigned char *data)
{
	char buffer[GC_MAXSTR_LENGTH];
	unsigned int i, indent, len;

	/* Append the indent string. */
	indent = gc_get_indent(item->indent, buffer, sizeof(buffer));

	/* Print the title. */
	GC_PRINTK(s, "%sBUFFER @ 0x%08X\n",
		  buffer, item->gpuaddr);

	/* Print the buffer. */
	for (i = 0, len = indent; i < item->datasize; i += 4) {
		if ((i % GC_GENERIC_DATA_COUNT) == 0) {
			if (i != 0) {
				/* Print the string. */
				GC_PRINTK(s, "%s\n", buffer);

				/* Reset the line. */
				len = indent;
			}

			len += snprintf(buffer + len, sizeof(buffer) - len,
					"0x%08X: ", item->gpuaddr + i);
		}

		/* Append the data value. */
		len += snprintf(buffer + len, sizeof(buffer) - len,
				" 0x%08X", *(unsigned int *) (data + i));
	}

	/* Print the last partial string. */
	if ((i % GC_SURFACE_DATA_COUNT) != 0)
		GC_PRINTK(s, "%s\n", buffer);
}

static char *gc_module_name(unsigned int index)
{
	switch (index) {
	case GCREG_COMMAND_STALL_STALL_SOURCE_FRONT_END:
		return "FE";

	case GCREG_COMMAND_STALL_STALL_SOURCE_PIXEL_ENGINE:
		return "PE";

	case GCREG_COMMAND_STALL_STALL_SOURCE_DRAWING_ENGINE:
		return "DE";

	default:
		return "*INVALID*";
	}
}

static void gc_print_command(struct seq_file *s, struct itembuffer *item,
			     unsigned char *data)
{
	char buffer[GC_MAXSTR_LENGTH];
	unsigned int *data32;
	unsigned int i, j, datacount;
	unsigned int command, count, addr;
	unsigned int delay, src, dst;
	unsigned int x1, y1, x2, y2;

	/* Append the indent string. */
	gc_get_indent(item->indent, buffer, sizeof(buffer));

	/* Print the title. */
	GC_PRINTK(s, "%sCOMMAND BUFFER @ 0x%08X\n", buffer, item->gpuaddr);
	GC_PRINTK(s, "%s  size = %d\n", buffer, item->datasize);

	datacount = (item->datasize + 3) / 4;
	data32 = (unsigned int *) data;
	for (i = 0; i < datacount;) {
#if GC_C_BUFFER
		GC_PRINTK(s, "%s\t0x%08X,\n", buffer, data32[i++]);
#else
		command = (data32[i] >> 27) & 0x1F;

		switch (command) {
		case GCREG_COMMAND_OPCODE_LOAD_STATE:
			count = (data32[i] >> 16) & 0x3F;
			addr = data32[i] & 0xFFFF;
			GC_PRINTK(s, "%s  0x%08X: 0x%08X  STATE(0x%04X, %d)\n",
				  buffer, item->gpuaddr + (i << 2),
				  data32[i], addr, count);
			i += 1;

			count |= 1;
			for (j = 0; j < count; i += 1, j += 1)
				GC_PRINTK(s, "%s%14c0x%08X\n",
					  buffer, ' ', data32[i]);
			break;

		case GCREG_COMMAND_OPCODE_END:
			GC_PRINTK(s, "%s  0x%08X: 0x%08X  END()\n",
				  buffer, item->gpuaddr + (i << 2),
				  data32[i]);
			i += 1;

			GC_PRINTK(s, "%s%14c0x%08X\n",
				  buffer, ' ', data32[i]);
			i += 1;
			break;

		case GCREG_COMMAND_OPCODE_NOP:
			GC_PRINTK(s, "%s  0x%08X: 0x%08X  NOP()\n",
				  buffer, item->gpuaddr + (i << 2),
				  data32[i]);
			i += 1;

			GC_PRINTK(s, "%s"  "%14c0x%08X\n",
				buffer, ' ', data32[i]);
			i += 1;
			break;

		case GCREG_COMMAND_OPCODE_STARTDE:
			count = (data32[i] >> 8) & 0xFF;
			GC_PRINTK(s, "%s  0x%08X: 0x%08X  STARTDE(%d)\n",
				  buffer, item->gpuaddr + (i << 2),
				  data32[i], count);
			i += 1;

			GC_PRINTK(s, "%s"  "%14c0x%08X\n",
				buffer, ' ', data32[i]);
			i += 1;

			for (j = 0; j < count; j += 1) {
				x1 =  data32[i]	       & 0xFFFF;
				y1 = (data32[i] >> 16) & 0xFFFF;
				GC_PRINTK(s, "%s%14c0x%08X	 LT(%d,%d)\n",
					  buffer, ' ', data32[i], x1, y1);
				i += 1;

				x2 =  data32[i]	       & 0xFFFF;
				y2 = (data32[i] >> 16) & 0xFFFF;
				GC_PRINTK(s, "%s%14c0x%08X	 RB(%d,%d)\n",
					  buffer, ' ', data32[i], x2, y2);
				i += 1;
			}
			break;

		case GCREG_COMMAND_OPCODE_WAIT:
			delay = data32[i] & 0xFFFF;
			GC_PRINTK(s, "%s  0x%08X: 0x%08X  WAIT(%d)\n",
				  buffer, item->gpuaddr + (i << 2),
				  data32[i], delay);
			i += 1;

			GC_PRINTK(s, "%s%14c0x%08X\n", buffer, ' ', data32[i]);
			i += 1;
			break;

		case GCREG_COMMAND_OPCODE_LINK:
			count = data32[i] & 0xFFFF;
			addr = data32[i + 1];
			GC_PRINTK(s, "%s  0x%08X: 0x%08X  "
				  "LINK(0x%08X-0x%08X, %d)\n",
				  buffer, item->gpuaddr + (i << 2),
				  data32[i], addr, addr + count * 8,
				  count);
			i += 1;

			GC_PRINTK(s, "%s%14c0x%08X\n", buffer, ' ', data32[i]);
			i += 1;
			break;

		case GCREG_COMMAND_OPCODE_STALL:
			src =  data32[i + 1]	   & 0x1F;
			dst = (data32[i + 1] >> 8) & 0x1F;

			GC_PRINTK(s, "%s  0x%08X: 0x%08X  STALL(%s-%s)\n",
				  buffer, item->gpuaddr + (i << 2),
				  data32[i],
				  gc_module_name(src),
				  gc_module_name(dst));
			i += 1;

			GC_PRINTK(s, "%s"  "%14c0x%08X\n",
				  buffer, ' ', data32[i]);
			i += 1;
			break;

		default:
			GC_PRINTK(s, "%s  0x%08X: 0x%08X  UNKNOWN COMMAND\n",
				  buffer, item->gpuaddr + (i << 2),
				  data32[i]);
			i += 2;
		}
#endif
	}
}

static void gc_flush_line(struct seq_file *s, char *buffer,
			  unsigned int indent, unsigned int *len,
			  unsigned int count, unsigned char checksum)
{
	unsigned int _len;
	char countstr[10];

	/* Correct data count. */
	count %= GC_SURFACE_DATA_COUNT;
	if (count == 0)
		count = GC_SURFACE_DATA_COUNT;

	snprintf(countstr, sizeof(countstr), "%02X", count);
	buffer[indent + 1] = countstr[0];
	buffer[indent + 2] = countstr[1];

	/* Append the checksum. */
	_len = *len;
	_len += snprintf(buffer + _len, sizeof(buffer) - _len,
			 "%02X", checksum);

	/* Print the string. */
	GC_PRINTK(s, "%s\n", buffer);

	/* Reset the length. */
	*len = indent;
}

static void gc_print_surface(struct seq_file *s, struct itembuffer *itembuffer,
			     unsigned char *data)
{
	char buffer[GC_MAXSTR_LENGTH];
	unsigned int i, indent, len;
	unsigned int prevupper32 = ~0U;
	unsigned int currupper32;
	unsigned int offset, address;
	unsigned int width, height;

	/* Append the indent string. */
	indent = gc_get_indent(itembuffer->indent, buffer, sizeof(buffer));

	/* Print the title. */
	GC_PRINTK(s, "%sIMAGE SURFACE @ 0x%08X\n",
		  buffer, itembuffer->gpuaddr);

	GC_PRINTK(s, "%s  surface size = %dx%d\n",
		  buffer, itembuffer->surfwidth, itembuffer->surfheight);

	GC_PRINTK(s, "%s  surface colordepth = %d\n",
		  buffer, itembuffer->surfbpp);

	GC_PRINTK(s, "%s  dumping rectangle = (%d,%d)-(%d,%d)\n",
		  buffer, itembuffer->x1, itembuffer->y1,
		  itembuffer->x2, itembuffer->y2);

	/* Add TGA header. */
	width  = itembuffer->x2 - itembuffer->x1;
	height = itembuffer->y2 - itembuffer->y1;
	GC_PRINTK(s, ":12000000000002000000000000000000"
		  "%02X%02X%02X%02X%02X2000\n",
		  (width  & 0xFF), ((width  >> 8) & 0xFF),
		  (height & 0xFF), ((height >> 8) & 0xFF),
		  itembuffer->surfbpp * 8);

	/* TGA skip header. */
	offset = 18;

	/* Print the buffer. */
	for (i = 0, len = indent; i < itembuffer->datasize; i += 1) {
		/* Determine the current address. */
		address = offset + i;

		/* Determine the current higher 16 bits of the address. */
		currupper32 = address >> 16;

		/* Did it change? */
		if (currupper32 != prevupper32) {
			/* Print the previous data if any. */
			if ((i % GC_SURFACE_DATA_COUNT) != 0)
				gc_flush_line(s, buffer, indent, &len, i, 0);

			/* Set new upper address. */
			prevupper32 = currupper32;
			GC_PRINTK(s, ":02000004%04X00\n", prevupper32);

			/* Add the line prefix. */
			len += snprintf(buffer + len,
					sizeof(buffer) - len - 2,
					":xx%04X00", address & 0xFFFF);
		} else if ((i % GC_SURFACE_DATA_COUNT) == 0) {
			len += snprintf(buffer + len,
					sizeof(buffer) - len - 2,
					":xx%04X00", address & 0xFFFF);
		}

		/* Append the data value. */
		len += snprintf(buffer + len,
				sizeof(buffer) - len - 2,
				"%02X", data[i]);

		/* End of line? */
		if (((i + 1) % GC_SURFACE_DATA_COUNT) == 0)
			gc_flush_line(s, buffer, indent, &len, i + 1, 0);
	}

	/* Print the last partial string. */
	if ((i % GC_SURFACE_DATA_COUNT) != 0)
		gc_flush_line(s, buffer, indent, &len, i, 0);

	/* End of dump. */
	GC_PRINTK(s, ":00000001FF\n");
}

typedef void (*printbuffer) (struct seq_file *s, struct itembuffer *itembuffer,
			     unsigned char *data);

static printbuffer g_printbuffer[] = {
	gc_print_generic,
	gc_print_command,
	gc_print_surface
};

static void gc_print_buffer(struct seq_file *s, struct itembuffer *itembuffer,
			    unsigned char *data)
{
	if (((int) itembuffer->buffertype < 0) ||
		(itembuffer->buffertype >= countof(g_printbuffer))) {
		GC_PRINTK(s,  "BUFFER ENTRY 0x%08X\n",
			  (unsigned int) itembuffer);
		GC_PRINTK(s,  "INVALID BUFFER TYPE %d\n",
			  itembuffer->buffertype);
	} else {
		g_printbuffer[itembuffer->buffertype](s, itembuffer, data);
	}
}


/*******************************************************************************
 * Print function wrappers.
 */

#if GC_BUFFERED_OUTPUT
static unsigned int gc_print_none(struct seq_file *s, struct buffout *buffout,
				  struct itemhead *item)
{
	/* Return the size of the node. */
	return get_item_size_terminator(item);
}

static unsigned int gc_print_string_wrapper(struct seq_file *s,
					    struct buffout *buffout,
					    struct itemhead *item)
{
	/* Print the message. */
	gc_print_string(s, (struct itemstring *) item);

	/* Return the size of the node. */
	return get_item_size_string(item);
}

static unsigned int gc_print_buffer_wrapper(struct seq_file *s,
					    struct buffout *buffout,
					    struct itemhead *item)
{
	unsigned char *data;
	struct itembuffer *itembuffer = (struct itembuffer *) item;

	/* Compute data address. */
	data = (unsigned char *) (itembuffer + 1);

	/* Print the message. */
	gc_print_buffer(s, (struct itembuffer *) item, data);

	/* Return the size of the node. */
	return get_item_size_buffer(item);
}

typedef unsigned int (*printitem) (struct seq_file *s, struct buffout *buffout,
				   struct itemhead *item);

static printitem g_printarray[] = {
	gc_print_none,
	gc_print_string_wrapper,
	gc_print_buffer_wrapper
};
#endif


/*******************************************************************************
 * Private functions.
 */

unsigned int gc_get_bpp(unsigned int format)
{
	unsigned int bpp;

	switch (format) {
	case GCREG_DE_FORMAT_INDEX8:
	case GCREG_DE_FORMAT_A8:
		bpp = 1;
		break;

	case GCREG_DE_FORMAT_X4R4G4B4:
	case GCREG_DE_FORMAT_A4R4G4B4:
	case GCREG_DE_FORMAT_X1R5G5B5:
	case GCREG_DE_FORMAT_A1R5G5B5:
	case GCREG_DE_FORMAT_R5G6B5:
	case GCREG_DE_FORMAT_YUY2:
	case GCREG_DE_FORMAT_UYVY:
	case GCREG_DE_FORMAT_RG16:
		bpp = 2;
		break;

	case GCREG_DE_FORMAT_X8R8G8B8:
	case GCREG_DE_FORMAT_A8R8G8B8:
		bpp = 4;
		break;

	default:
		bpp = 0;
	}

	return bpp;
}

#if GC_BUFFERED_OUTPUT
static void gc_buffer_flush(struct seq_file *s, struct buffout *buffout)
{
	int i, skip;
	struct itemhead *item;

	if (buffout->count == 0)
		return;

	GC_PRINTK(s,  "****************************************"
				"****************************************\n");
	GC_PRINTK(s,  "FLUSHING DEBUG OUTPUT BUFFER (%d elements).\n",
				buffout->count);

#if !GC_ENABLE_OVERFLOW
	{
		int occupied = (100 * (buffout->index - buffout->start))
				/ GC_DUMP_BUFFER_SIZE;
		if (buffout->start != 0)
			GC_PRINTK(s,  "	 START = %d\n", buffout->start);
		GC_PRINTK(s,  "	 INDEX = %d\n", buffout->index);
		GC_PRINTK(s,  "	 BUFFER USE = %d%%\n", occupied);
	}
#endif

	GC_PRINTK(s,  "****************************************"
			   "****************************************\n");

	item = (struct itemhead *) &buffout->buffer[buffout->start];
	GC_DEBUGMSG("start=%d.\n", buffout->start);

	for (i = 0; i < buffout->count; i += 1) {
		GC_DEBUGMSG("printing item %d of type %d @ 0x%08X.\n",
			    i, item->type, (unsigned int) item);
		skip = (*g_printarray[item->type]) (s, buffout, item);

		item = (struct itemhead *) ((unsigned char *) item + skip);
		GC_DEBUGMSG("next item @ 0x%08X.\n", (unsigned int) item);

		if (item->type == GC_BUFITEM_NONE) {
			GC_DEBUGMSG("reached the end of buffer.\n");
			item = (struct itemhead *) buffout->buffer;
		}
	}

	GC_DEBUGMSG("resetting the buffer.\n");
	buffout->start = 0;
	buffout->index = 0;
	buffout->count = 0;
}

static struct itemhead *gc_allocate_item(struct buffout *buffout, int size)
{
	struct itemhead *item, *next;
	int endofbuffer = (buffout->index + size
			>= GC_DUMP_BUFFER_SIZE - sizeof(struct itemhead));

#if GC_ENABLE_OVERFLOW
	int skip, bufferoverflow;

	bufferoverflow = (buffout->index < buffout->start) &&
				(buffout->index + size >= buffout->start);

	if (endofbuffer || bufferoverflow) {
		if (endofbuffer) {
			if (buffout->index < buffout->start) {
				item = (struct itemhead *)
					&buffout->buffer[buffout->start];

				while (item->type != GC_BUFITEM_NONE) {
					skip = (*g_itemsize[item->type]) (item);

					buffout->start += skip;
					buffout->count -= 1;

					item->type = GC_BUFITEM_NONE;
					item = (struct itemhead *)
						((unsigned char *) item + skip);
				}

				buffout->start = 0;
			}

			buffout->index = 0;
		}

		item = (struct itemhead *) &buffout->buffer[buffout->start];

		while (buffout->start - buffout->index <= size) {
			skip = (*g_itemsize[item->type]) (item);

			buffout->start += skip;
			buffout->count -= 1;

			item->type = GC_BUFITEM_NONE;
			item = (struct itemhead *)
				((unsigned char *) item + skip);

			if (item->type == GC_BUFITEM_NONE) {
				buffout->start = 0;
				break;
			}
		}
	}
#else
	if (endofbuffer) {
		GC_PRINTK(NULL, "message buffer full; "
			"forcing message flush.\n\n");
		gc_buffer_flush(NULL, buffout);
	}
#endif

	item = (struct itemhead *) &buffout->buffer[buffout->index];

	buffout->index += size;
	buffout->count += 1;

	next = (struct itemhead *) ((unsigned char *) item + size);
	next->type = GC_BUFITEM_NONE;

	return item;
}

static void gc_append_string(struct buffout *buffout,
			     struct itemstring *itemstring)
{
	unsigned char *messagedata;
	struct itemstring *item;
	unsigned int alignment;
	int size, freesize;
	int allocsize;

	/* Determine the maximum item size. */
	allocsize = sizeof(struct itemstring) + itemstring->datasize
			+ GC_VARARG_ALIGNMENT;

	/* Allocate the item. */
	item = (struct itemstring *) gc_allocate_item(buffout, allocsize);
	GC_DEBUGMSG("allocated %d bytes @ 0x%08X.\n",
		    allocsize, (unsigned int) item);

	/* Compute the initial message data pointer. */
	messagedata = (unsigned char *) (item + 1);

	/* Align the data pointer as necessary. */
	alignment = GC_PTRALIGNMENT(messagedata, GC_VARARG_ALIGNMENT);
	messagedata += alignment;
	GC_DEBUGMSG("messagedata @ 0x%08X.\n", (unsigned int) messagedata);

	/* Set item data. */
	item->itemtype = GC_BUFITEM_STRING;
	item->indent = itemstring->indent;
	item->message = itemstring->message;
	item->messagedata = *(va_list *) &messagedata;
	item->datasize = itemstring->datasize;

#if GC_SHOW_PID
	item->pid = itemstring->pid;
#endif

#if GC_SHOW_DUMP_LINE
	item->dumpline = itemstring->dumpline;
#endif

	/* Copy argument value. */
	if (itemstring->datasize != 0) {
		GC_DEBUGMSG("copying %d bytes of messagedata.\n",
			    itemstring->datasize);
		memcpy(messagedata,
		       *(unsigned char **) &itemstring->messagedata,
		       itemstring->datasize);
	}

	/* Compute the actual node size. */
	size = sizeof(struct itemstring) + itemstring->datasize + alignment;
	GC_DEBUGMSG("adjusted item size=%d.\n", size);

	/* Free extra memory if any. */
	freesize = allocsize - size;
	GC_DEBUGMSG("freesize=%d.\n", freesize);

	if (freesize != 0) {
		struct itemhead *next;
		buffout->index -= freesize;
		next = (struct itemhead *) ((unsigned char *) item + size);
		next->type = GC_BUFITEM_NONE;
	}

#if GC_BUFFERED_OUTPUT && GC_FLUSH_COUNT
	if (buffout->count >= GC_FLUSH_COUNT) {
		GC_PRINTK(NULL, "reached %d message count; "
			"forcing message flush.\n\n", buffout->count);
		gc_buffer_flush(NULL, buffout);
	}
#endif
}

static void gc_append_buffer(struct buffout *buffout,
				struct itembuffer *itembuffer,
				unsigned int *data)
{
	struct itembuffer *item;
	int allocsize;

	/* Determine the item size. */
	allocsize = sizeof(struct itembuffer) + itembuffer->datasize;

	/* Allocate the item. */
	item = (struct itembuffer *) gc_allocate_item(buffout, allocsize);
	GC_DEBUGMSG("allocated %d bytes @ 0x%08X.\n",
		    allocsize, (unsigned int) item);

	/* Set item data. */
	*item = *itembuffer;

	/* Copy data. */
	memcpy(item + 1, data, itembuffer->datasize);

#if GC_BUFFERED_OUTPUT && GC_FLUSH_COUNT
	if (buffout->count >= GC_FLUSH_COUNT) {
		GC_PRINTK(NULL, "reached %d message count; "
			"forcing message flush.\n\n", buffout->count);
		gc_buffer_flush(NULL, buffout);
	}
#endif
}
#endif

static void gc_print(struct buffout *buffout, unsigned int argsize,
		     const char *message, va_list args)
{
	struct itemstring itemstring;
	struct threadinfo *threadinfo;

	GCLOCK(&g_lockmutex);

	/* Locate thead entry. */
	threadinfo = get_threadinfo(buffout);

	/* Form the indent string. */
	if (strncmp(message, "--", 2) == 0)
		threadinfo->msgindent -= 2;

	/* Fill in the sructure. */
	itemstring.itemtype = GC_BUFITEM_STRING;
	itemstring.indent = threadinfo->msgindent
			  + threadinfo->threadindent;
	itemstring.message = message;
	itemstring.messagedata = args;
	itemstring.datasize = argsize;

#if GC_SHOW_PID
	itemstring.pid = threadinfo->pid;
#endif

#if GC_SHOW_DUMP_LINE
	itemstring.dumpline = ++buffout->dumpline;
#endif

	/* Print the message. */
#if GC_BUFFERED_OUTPUT
	gc_append_string(buffout, &itemstring);
#else
	gc_print_string(NULL, &itemstring);
#endif

	/* Check increasing indent. */
	if (strncmp(message, "++", 2) == 0)
		threadinfo->msgindent += 2;

	GCUNLOCK(&g_lockmutex);
}


/*******************************************************************************
 * Dumping functions.
 */

void gc_dump_string(struct gcdbgfilter *filter, unsigned int zone,
			const char *message, ...)
{
	va_list args;
	unsigned int i, count, argsize;

	if (!g_outputbuffer.enable)
		return;

	if (message == NULL)
		GC_DEBUGMSG("message is NULL.\n");

	if (GC_VERIFY_ENABLE(filter, zone)) {
		for (i = 0, count = 0; message[i]; i += 1)
			if (message[i] == '%')
				count += 1;

		argsize = count * sizeof(unsigned int);
		GC_DEBUGMSG("argsize=%d.\n", argsize);

		va_start(args, message);
		gc_print(&g_outputbuffer, argsize, message, args);
		va_end(args);
	}
}
EXPORT_SYMBOL(gc_dump_string);

void gc_dump_string_sized(struct gcdbgfilter *filter, unsigned int zone,
				unsigned int argsize, const char *message, ...)
{
	va_list args;

	if (!g_outputbuffer.enable)
		return;

	if (GC_VERIFY_ENABLE(filter, zone)) {
		va_start(args, message);
		gc_print(&g_outputbuffer, argsize, message, args);
		va_end(args);
	}
}
EXPORT_SYMBOL(gc_dump_string_sized);

void gc_dump_cmd_buffer(struct gcdbgfilter *filter, unsigned int zone,
			void *ptr, unsigned int gpuaddr, unsigned int datasize)
{
	struct itembuffer itembuffer;
	struct threadinfo *threadinfo;

	if (!g_outputbuffer.enable)
		return;

	if (GC_VERIFY_ENABLE(filter, zone)) {
		GCLOCK(&g_lockmutex);

		/* Locate thead entry. */
		threadinfo = get_threadinfo(&g_outputbuffer);

		/* Fill in the sructure. */
		itembuffer.itemtype = GC_BUFITEM_BUFFER;
		itembuffer.buffertype = GC_BUFTYPE_COMMAND;
		itembuffer.indent = threadinfo->msgindent
				  + threadinfo->threadindent;
		itembuffer.datasize = datasize;
		itembuffer.gpuaddr = gpuaddr;

		/* Print the message. */
#if GC_BUFFERED_OUTPUT
		gc_append_buffer(NULL, &g_outputbuffer, &itembuffer,
					(unsigned int *) ptr);
#else
		gc_print_buffer(NULL, &itembuffer,
					(unsigned char *) ptr);
#endif

		GCUNLOCK(&g_lockmutex);
	}
}
EXPORT_SYMBOL(gc_dump_cmd_buffer);

void gc_dump_buffer(struct gcdbgfilter *filter, unsigned int zone,
			void *ptr, unsigned int gpuaddr,
			unsigned int datasize)
{
	struct itembuffer itembuffer;
	struct threadinfo *threadinfo;

	if (!g_outputbuffer.enable)
		return;

	if (GC_VERIFY_ENABLE(filter, zone)) {
		GCLOCK(&g_lockmutex);

		/* Locate thead entry. */
		threadinfo = get_threadinfo(&g_outputbuffer);

		/* Fill in the sructure. */
		itembuffer.itemtype = GC_BUFITEM_BUFFER;
		itembuffer.buffertype = GC_BUFTYPE_GENERIC;
		itembuffer.indent = threadinfo->msgindent
				  + threadinfo->threadindent;
		itembuffer.datasize = datasize;
		itembuffer.gpuaddr = gpuaddr;

		/* Print the message. */
#if GC_BUFFERED_OUTPUT
		gc_append_buffer(NULL, &g_outputbuffer, &itembuffer,
					(unsigned int *) ptr);
#else
		gc_print_buffer(NULL, &itembuffer,
					(unsigned char *) ptr);
#endif

		GCUNLOCK(&g_lockmutex);
	}
}
EXPORT_SYMBOL(gc_dump_buffer);


/*******************************************************************************
 * Dumping control functions.
 */

void gc_dump_enable(void)
{
	GCLOCK(&g_lockmutex);

	g_outputbuffer.enable = 1;
	GC_PRINTK(NULL, "gcx dumping is enabled.\n");

	GCUNLOCK(&g_lockmutex);
}
EXPORT_SYMBOL(gc_dump_enable);

void gc_dump_disable(void)
{
	GCLOCK(&g_lockmutex);

	g_outputbuffer.enable = 0;
	GC_PRINTK(NULL, "gcx dumping is disabled.\n");

	GCUNLOCK(&g_lockmutex);
}
EXPORT_SYMBOL(gc_dump_disable);

void gc_dump_show_enabled(struct seq_file *s)
{
	struct list_head *filterhead;
	struct gcdbgfilter *filter;
	unsigned int i, zone;

	GCLOCK(&g_lockmutex);

	GC_PRINTK(s, "gcx logging is %s\n", g_outputbuffer.enable
			? "enabled" : "disabled");

	list_for_each(filterhead, &gc_filterlist) {
		filter = list_entry(filterhead, struct gcdbgfilter,
					link);

		GC_PRINTK(s, "gcx filter '%s':\n", filter->filtername);
		GC_PRINTK(s, "    zone mask = 0x%08X%s\n", filter->zone,
				(filter->zone == 0)
					? " (all disabled)" : "");

		for (i = 0; filter->zonename[i] != NULL; i++) {
			zone = 1 << i;
			GC_PRINTK(s, "    0x%08X: %10s%s\n",
					zone, filter->zonename[i],
					((filter->zone & zone) != 0)
						? " (enabled)" : "");
		}
	}

	GCUNLOCK(&g_lockmutex);
}
EXPORT_SYMBOL(gc_dump_show_enabled);

void gc_dump_filter_enable(const char *filtername, int zone)
{
	struct list_head *filterhead;
	struct gcdbgfilter *filter;
	unsigned int filterfound = 0;
	unsigned int havesetzones = 0;

	GCLOCK(&g_lockmutex);

	GC_PRINTK(NULL, "modifying zone mask for filter %s:\n", filtername);

	list_for_each(filterhead, &gc_filterlist) {
		filter = list_entry(filterhead, struct gcdbgfilter,
					link);

		if (strcasecmp(filtername, filter->filtername) == 0) {
			GC_PRINTK(NULL, "  0x%08X --> 0x%08X\n",
				filter->zone, zone);
			filter->zone = zone;
			filterfound = 1;
		}

		if (filter->zone != 0)
			havesetzones = 1;
	}

	GCUNLOCK(&g_lockmutex);

	if (!filterfound)
		GC_PRINTK(NULL, "  couldn't find filter %s.\n", filtername);

	if (havesetzones && !g_outputbuffer.enable)
		gc_dump_enable();
	else if (!havesetzones && g_outputbuffer.enable)
		gc_dump_disable();
}
EXPORT_SYMBOL(gc_dump_filter_enable);

void gc_dbg_add_client(struct gcdbgfilter *filter)
{
	list_add(&filter->link, &gc_filterlist);
}
EXPORT_SYMBOL(gc_dbg_add_client);

void gc_dump_flush(struct seq_file *s)
{
#if GC_BUFFERED_OUTPUT
	GCLOCK(&g_lockmutex);

	/*
	 * Not dumping through debugfs for now because we have
	 * too much data and it'd require us to implement the
	 * seq_file iterator interface.
	 */
	gc_buffer_flush(NULL, &g_outputbuffer);

	GCUNLOCK(&g_lockmutex);
#endif
}
EXPORT_SYMBOL(gc_dump_flush);

void gc_dump_reset(void)
{
#if GC_BUFFERED_OUTPUT
	GCLOCK(&g_lockmutex);

	g_outputbuffer.start = 0;
	g_outputbuffer.index = 0;
	g_outputbuffer.count = 0;

	GC_PRINTK(NULL, "gcx logging buffer is reset.\n");

	GCUNLOCK(&g_lockmutex);
#endif
}
EXPORT_SYMBOL(gc_dump_reset);


/*******************************************************************************
 * Command buffer parser.
 */

int gc_parse_command_buffer(unsigned int *buffer, unsigned int size,
				struct gccommandinfo *info)
{
	int res;
	unsigned int i, j, itemcount, index, oldsrc;
	unsigned int command, count, addr;

	memset(info, 0, sizeof(struct gccommandinfo));
	info->command = ~0U;

	oldsrc = 0;

	itemcount = (size + 3) / 4;
	for (i = 0; i < itemcount;) {
		command = (buffer[i] >> 27) & 0x1F;

		switch (command) {
		case GCREG_COMMAND_OPCODE_LOAD_STATE:
			count = (buffer[i] >> 16) & 0x3F;
			addr = buffer[i] & 0xFFFF;
			i += 1;

			for (j = 0; j < count; j += 1) {
				switch (addr) {
				case gcregDestAddressRegAddrs:
					info->dst.surf.address = buffer[i];
					break;

				case gcregDestStrideRegAddrs:
					info->dst.surf.stride = buffer[i];
					break;

				case gcregDestRotationConfigRegAddrs:
					info->dst.surf.width
						= buffer[i] & 0xFFFF;
					break;

				case gcregDstRotationHeightRegAddrs:
					info->dst.surf.height
						= buffer[i] & 0xFFFF;
					break;

				case gcregDestConfigRegAddrs:
					info->command
						= (buffer[i] >> 12) & 0xF;

					info->dst.surf.swizzle
						= (buffer[i] >> 16) & 0x3;

					info->dst.surf.format
						= buffer[i] & 0x1F;

					info->dst.surf.bpp = gc_get_bpp(
						info->dst.surf.format);
					break;

				case gcregSrcAddressRegAddrs:
					info->src[0].surf.address = buffer[i];
					oldsrc = 1;
					break;

				case gcregSrcStrideRegAddrs:
					info->src[0].surf.stride = buffer[i];
					break;

				case gcregSrcRotationConfigRegAddrs:
					info->src[0].surf.width
						= buffer[i] & 0xFFFF;
					break;

				case gcregSrcRotationHeightRegAddrs:
					info->src[0].surf.height
						= buffer[i] & 0xFFFF;
					break;

				case gcregSrcConfigRegAddrs:
					info->src[0].surf.swizzle
						= (buffer[i] >> 20) & 0x3;

					info->src[0].surf.format
						= (buffer[i] >> 24) & 0x1F;

					info->src[0].surf.bpp = gc_get_bpp(
						info->src[0].surf.format);
					break;

				case gcregSrcOriginRegAddrs:
					info->src[0].rect.left
						= buffer[i] & 0xFFFF;

					info->src[0].rect.top
						= (buffer[i] >> 16) & 0xFFFF;
					break;

				case gcregSrcSizeRegAddrs:
					info->src[0].rect.right
						= buffer[i] & 0xFFFF;

					info->src[0].rect.bottom
						= (buffer[i] >> 16) & 0xFFFF;
					break;

				case gcregBlock4SrcAddressRegAddrs:
				case gcregBlock4SrcAddressRegAddrs + 1:
				case gcregBlock4SrcAddressRegAddrs + 2:
				case gcregBlock4SrcAddressRegAddrs + 3:
					index = addr & 3;
					info->src[index].surf.address
						= buffer[i];
					break;

				case gcregBlock4SrcStrideRegAddrs:
				case gcregBlock4SrcStrideRegAddrs + 1:
				case gcregBlock4SrcStrideRegAddrs + 2:
				case gcregBlock4SrcStrideRegAddrs + 3:
					index = addr & 3;
					info->src[index].surf.stride
						= buffer[i];
					break;

				case gcregBlock4SrcRotationConfigRegAddrs:
				case gcregBlock4SrcRotationConfigRegAddrs + 1:
				case gcregBlock4SrcRotationConfigRegAddrs + 2:
				case gcregBlock4SrcRotationConfigRegAddrs + 3:
					index = addr & 3;
					info->src[index].surf.width
						= buffer[i] & 0xFFFF;
					break;

				case gcregBlock4SrcRotationHeightRegAddrs:
				case gcregBlock4SrcRotationHeightRegAddrs + 1:
				case gcregBlock4SrcRotationHeightRegAddrs + 2:
				case gcregBlock4SrcRotationHeightRegAddrs + 3:
					index = addr & 3;
					info->src[0].surf.height
						= buffer[i] & 0xFFFF;
					break;

				case gcregBlock4SrcConfigRegAddrs:
				case gcregBlock4SrcConfigRegAddrs + 1:
				case gcregBlock4SrcConfigRegAddrs + 2:
				case gcregBlock4SrcConfigRegAddrs + 3:
					index = addr & 3;
					info->src[index].surf.swizzle
						= (buffer[i] >> 20) & 0x3;

					info->src[index].surf.format
						= (buffer[i] >> 24) & 0x1F;

					info->src[index].surf.bpp = gc_get_bpp(
						info->src[index].surf.format);
					break;

				case gcregBlock4SrcOriginRegAddrs:
				case gcregBlock4SrcOriginRegAddrs + 1:
				case gcregBlock4SrcOriginRegAddrs + 2:
				case gcregBlock4SrcOriginRegAddrs + 3:
					index = addr & 3;
					info->src[index].rect.left
						= buffer[i] & 0xFFFF;

					info->src[index].rect.top
						= (buffer[i] >> 16) & 0xFFFF;
					break;

				case gcregBlock4SrcSizeRegAddrs:
				case gcregBlock4SrcSizeRegAddrs + 1:
				case gcregBlock4SrcSizeRegAddrs + 2:
				case gcregBlock4SrcSizeRegAddrs + 3:
					index = addr & 3;
					info->src[index].rect.right
						= buffer[i] & 0xFFFF;

					info->src[index].rect.bottom
						= (buffer[i] >> 16) & 0xFFFF;
					break;

				case gcregDEMultiSourceRegAddrs:
					info->srccount = (buffer[i] & 0x7) + 1;
					break;
				}

				addr += 1;
				i += 1;
			}

			i += ((~count) & 1);
			break;

		case GCREG_COMMAND_OPCODE_END:
		case GCREG_COMMAND_OPCODE_NOP:
		case GCREG_COMMAND_OPCODE_WAIT:
		case GCREG_COMMAND_OPCODE_LINK:
		case GCREG_COMMAND_OPCODE_STALL:
			i += 2;
			break;

		case GCREG_COMMAND_OPCODE_STARTDE:
			info->dst.rectcount = (buffer[i] >> 8) & 0xFF;
			i += 2;

			for (j = 0; j < info->dst.rectcount; j += 1) {
				info->dst.rect[j].left
						= buffer[i] & 0xFFFF;
				info->dst.rect[j].top
						= (buffer[i] >> 16) & 0xFFFF;
				i += 1;

				info->dst.rect[j].right
						= buffer[i] & 0xFFFF;
				info->dst.rect[j].bottom
						= (buffer[i] >> 16) & 0xFFFF;
				i += 1;
			}
			break;

		default:
			res = 0;
			gc_dump_string(NULL, 0,
					"bad command (%d) "
					"while parsing the command stream",
					command);
			goto exit;
		}

	}

	/* Enable old source. */
	if ((info->srccount == 0) && oldsrc)
		info->srccount = 1;

	/* Success. */
	res = 1;

exit:
	return res;
}
EXPORT_SYMBOL(gc_parse_command_buffer);


/*******************************************************************************
 * Bltsville debugging.
 */

char *gc_bvblend_name(enum bvblend blend)
{
	switch (blend) {
	case BVBLEND_CLEAR:		return "BVBLEND_CLEAR";
	case BVBLEND_SRC1:		return "BVBLEND_SRC1";
	case BVBLEND_SRC2:		return "BVBLEND_SRC2";
	case BVBLEND_SRC1OVER:		return "BVBLEND_SRC1OVER";
	case BVBLEND_SRC2OVER:		return "BVBLEND_SRC2OVER";
	case BVBLEND_SRC1IN:		return "BVBLEND_SRC1IN";
	case BVBLEND_SRC2IN:		return "BVBLEND_SRC2IN";
	case BVBLEND_SRC1OUT:		return "BVBLEND_SRC1OUT";
	case BVBLEND_SRC2OUT:		return "BVBLEND_SRC2OUT";
	case BVBLEND_SRC1ATOP:		return "BVBLEND_SRC1ATOP";
	case BVBLEND_SRC2ATOP:		return "BVBLEND_SRC2ATOP";
	case BVBLEND_XOR:		return "BVBLEND_XOR";
	case BVBLEND_PLUS:		return "BVBLEND_PLUS";
	case BVBLEND_NORMAL:		return "BVBLEND_NORMAL";
	case BVBLEND_LIGHTEN:		return "BVBLEND_LIGHTEN";
	case BVBLEND_DARKEN:		return "BVBLEND_DARKEN";
	case BVBLEND_MULTIPLY:		return "BVBLEND_MULTIPLY";
	case BVBLEND_AVERAGE:		return "BVBLEND_AVERAGE";
	case BVBLEND_ADD:		return "BVBLEND_ADD";
	case BVBLEND_SUBTRACT:		return "BVBLEND_SUBTRACT";
	case BVBLEND_DIFFERENCE:	return "BVBLEND_DIFFERENCE";
	case BVBLEND_NEGATE:		return "BVBLEND_NEGATE";
	case BVBLEND_SCREEN:		return "BVBLEND_SCREEN";
	case BVBLEND_EXCLUSION:		return "BVBLEND_EXCLUSION";
	case BVBLEND_OVERLAY:		return "BVBLEND_OVERLAY";
	case BVBLEND_SOFT_LIGHT:	return "BVBLEND_SOFT_LIGHT";
	case BVBLEND_HARD_LIGHT:	return "BVBLEND_HARD_LIGHT";
	case BVBLEND_COLOR_DODGE:	return "BVBLEND_COLOR_DODGE";
	case BVBLEND_COLOR_BURN:	return "BVBLEND_COLOR_BURN";
	case BVBLEND_LINEAR_LIGHT:	return "BVBLEND_LINEAR_LIGHT";
	case BVBLEND_VIVID_LIGHT:	return "BVBLEND_VIVID_LIGHT";
	case BVBLEND_PIN_LIGHT:		return "BVBLEND_PIN_LIGHT";
	case BVBLEND_HARD_MIX:		return "BVBLEND_HARD_MIX";
	case BVBLEND_REFLECT:		return "BVBLEND_REFLECT";
	case BVBLEND_GLOW:		return "BVBLEND_GLOW";
	case BVBLEND_PHOENIX:		return "BVBLEND_PHOENIX";
	default:			return "[UNKNOWN]";
	}
}
EXPORT_SYMBOL(gc_bvblend_name);


/*******************************************************************************
 * Initialization/cleanup.
 */

void gcdbg_init(void)
{
#if GC_BUFFERED_OUTPUT
	/* Allocate the debug buffer. */
	g_outputbuffer.buffer = kmalloc(GC_DUMP_BUFFER_SIZE, GFP_KERNEL);
	if (g_outputbuffer.buffer == NULL) {
		GC_PRINTK(NULL, "failed to allocate dump buffer.\n");
		return;
	}
#endif

	g_initdone = 1;
}

void gcdbg_exit(void)
{
#if GC_BUFFERED_OUTPUT
	if (g_outputbuffer.buffer != NULL) {
		kfree(g_outputbuffer.buffer);
		g_outputbuffer.buffer = NULL;
	}
#endif

	g_initdone = 0;
}

#endif /* GCDEBUG_ENABLE */
