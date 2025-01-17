#include <cassert>
#include <vector>
#include <string>
using namespace std;

#include "unrar/rar.hpp"
#include "vfs_rar.hpp"

//-----------------------------------------------------------------------------

//#define trace(...) { fprintf(stderr, __VA_ARGS__); }
#define trace(fmt,...)

#define min(x,y) ((x)<(y)?(x):(y))

static DB_functions_t *deadbeef;
static DB_vfs_t plugin;

typedef struct {
	DB_FILE file;

	Archive *arc;
	ComprDataIO *dataIO;
	Unpack *unp;
	byte *buffer;
	size_t buffer_size;

	int64_t offset;
	int64_t size;
} rar_file_t;

static const char *scheme_names[] = { "rar://", NULL };

void unstore_file(ComprDataIO *dataIO, int64_t size)
{
	byte buffer[0x10000];
	while (1) {
		uint code = dataIO->UnpRead(buffer, 0x10000);
		if (code == 0 || (int)code == -1)
			break;
		code = min(code, size);
		dataIO->UnpWrite(buffer, code);
		if (size >= 0)
			size -= code;
	}
}
//-----------------------------------------------------------------------------

const char **
vfs_rar_get_schemes (void)
{
	return scheme_names;
}

int
vfs_rar_is_streaming (void)
{
	return 0;
}

// fname must have form of zip://full_filepath.zip:full_filepath_in_zip
DB_FILE*
vfs_rar_open (const char *fname)
{
	trace("[vfs_rar_open] %s\n", fname);
	if (strncasecmp (fname, "rar://", 6))
		return NULL;

	// get the full path of .rar file
	fname += 6;
	const char *colon = strchr (fname, ':');
	if (!colon) {
		return NULL;
	}

	char rarname[colon-fname+1];
	memcpy(rarname, fname, colon-fname);
	rarname[colon-fname] = '\0';

	// get the compressed file in this archive
	fname = colon+1;

	Archive *arc = new Archive();
	trace("opening rar file: %s\n", rarname);
	if (!arc->Open(rarname))
		return NULL;

	if (!arc->IsArchive(true))
		return NULL;

	// find the desired file from archive
	trace("searching file %s\n", fname);
	bool found_file = false;
	while (arc->ReadHeader() > 0) {
		int hdr_type = arc->GetHeaderType();
		if (hdr_type == ENDARC_HEAD)
			break;
	
		switch (hdr_type) {
			case FILE_HEAD:
				if (!arc->IsArcDir() && !strcmp(arc->NewLhd.FileName, fname)) {
					trace("file %s found\n", fname);
					found_file = true;
				}
				break;

			default:
				break;
		}

		if (found_file)
			break;
		else
			arc->SeekToNext();
	}
	if (!found_file)
		return NULL;

	// Seek to head of the file
	arc->Seek(arc->NextBlockPos - arc->NewLhd.FullPackSize, SEEK_SET);

	// Initialize the ComprDataIO and Unpack
	ComprDataIO *dataIO = new ComprDataIO();
	Unpack *unp = new Unpack(dataIO);
	unp->Init(NULL);

	dataIO->CurUnpRead = 0;
	dataIO->CurUnpWrite = 0;
	dataIO->UnpFileCRC = arc->OldFormat ? 0 : 0xFFFFFFFF;
	dataIO->PackedCRC = 0xFFFFFFFF;

	dataIO->SetEncryption(
		(arc->NewLhd.Flags & LHD_PASSWORD) ? arc->NewLhd.UnpVer : 0,
		L"",
		(arc->NewLhd.Flags & LHD_SALT) ? arc->NewLhd.Salt : NULL,
		false,
		arc->NewLhd.UnpVer >= 36
	);
	dataIO->SetPackedSizeToRead(arc->NewLhd.FullPackSize);
	dataIO->SetFiles(arc, NULL); // we unpack data to memory

	// Unpack the full file into memory
	byte *buffer = (byte *)malloc(arc->NewLhd.FullUnpSize);
	dataIO->SetUnpackToMemory(buffer, arc->NewLhd.FullUnpSize);

	if (arc->NewLhd.Method == 0x30)
		unstore_file(dataIO, arc->NewLhd.FullUnpSize);
	else {
		unp->SetDestSize(arc->NewLhd.FullUnpSize);
		unp->DoUnpack(arc->NewLhd.UnpVer, (arc->NewLhd.Flags & LHD_SOLID));
	}

	rar_file_t *f = (rar_file_t *)malloc (sizeof (rar_file_t));
	memset (f, 0, sizeof (rar_file_t));
	f->file.vfs = &plugin;
	f->arc = arc;
	f->dataIO = dataIO;
	f->unp = unp;
	f->buffer = buffer;
	f->offset = 0;
	f->size = arc->NewLhd.FullUnpSize;

	return (DB_FILE*)f;
}

void
vfs_rar_close (DB_FILE *f)
{
	rar_file_t *rf = (rar_file_t *)f;

	if(rf->buffer)
		free(rf->buffer);
	if (rf->unp)
		delete rf->unp;
	if (rf->dataIO)
		delete rf->dataIO;
	if (rf->arc)
		delete rf->arc;

	free (rf);
}

size_t
vfs_rar_read (void *ptr, size_t size, size_t nmemb, DB_FILE *f)
{
	trace("[vfs_rar_read]\n");
	rar_file_t *rf = (rar_file_t *)f;

	size_t rb = min(size * nmemb, rf->size - rf->offset);
	if (rb) {
		memcpy(ptr, rf->buffer + rf->offset, rb);
		rf->offset += rb;
	}

	return rb / size;
}

int
vfs_rar_seek (DB_FILE *f, int64_t offset, int whence)
{
	trace("[vfs_rar_seek]");
	rar_file_t *rf = (rar_file_t *)f;

	if (whence == SEEK_CUR) {
		offset = rf->offset + offset;
	}
	else if (whence == SEEK_END) {
		offset = rf->size + offset;
	}

	rf->offset = offset;
	return 0;
#if 0
	// reopen when seeking back
	if (offset < rf->offset) {
		rf->arc->Seek(
			rf->arc->NextBlockPos - rf->arc->NewLhd.FullPackSize,
			SEEK_SET
		);
		rf->offset = 0;
	}

	unsigned char buf[4096];
	int64_t n = offset - rf->offset;
	while (n > 0) {
		int sz = min (n, sizeof (buf));
		size_t rb = read_unpacked_data(rf, buf, sz);
		n -= rb;
		assert (n >= 0);
		rf->offset += rb;
		if (rb != sz) {
			break;
		}
	}

	return n > 0 ? -1 : 0;
#endif
}

int64_t
vfs_rar_tell (DB_FILE *f)
{
	rar_file_t *rf = (rar_file_t *)f;
	return rf->offset;
}

void
vfs_rar_rewind (DB_FILE *f)
{
	rar_file_t *rf = (rar_file_t *)f;
	rf->offset = 0;
}

int64_t
vfs_rar_getlength (DB_FILE *f)
{
	rar_file_t *rf = (rar_file_t *)f;
	return rf->size;
}

int
vfs_rar_scandir (
	const char *dir,
	struct dirent ***namelist,
	int (*selector) (const struct dirent *),
	int (*cmp) (const struct dirent **, const struct dirent **)
)
{
	vector<string> fname_list;
	Archive arc;

	if (!arc.Open(dir))
		return -1;

	if (!arc.IsArchive(true))
		return -1;

	// read files from archive
	while (arc.ReadHeader() > 0) {
		int hdr_type = arc.GetHeaderType();
		if (hdr_type == ENDARC_HEAD)
			break;
	
		switch (hdr_type) {
			case FILE_HEAD:
				if (!arc.IsArcDir())
					fname_list.push_back(string(arc.NewLhd.FileName));
				break;

			default:
				break;
		}

		arc.SeekToNext();
	}

	// transmit files to player
	int n = fname_list.size();
	*namelist = (dirent **)malloc (sizeof(void *) * n);
	for (int i = 0; i < n; i++) {
		(*namelist)[i] = (dirent *)malloc (sizeof(struct dirent));
		memset ((*namelist)[i], 0, sizeof(struct dirent));
		snprintf(
			(*namelist)[i]->d_name, sizeof((*namelist)[i]->d_name),
			"rar://%s:%s", dir, fname_list[i].c_str()
		);
	}

	return n;
}

int
vfs_rar_is_container (const char *fname)
{
	const char *ext = strrchr (fname, '.');
	if (ext && !strcasecmp (ext, ".rar"))
		return 1;
	return 0;
}

extern "C"
DB_plugin_t *
vfs_rar_load (DB_functions_t *api)
{
	deadbeef = api;

	plugin.plugin.api_vmajor = 1;
	plugin.plugin.api_vminor = 0;
	plugin.plugin.version_major = 1;
	plugin.plugin.version_minor = 0;
	plugin.plugin.type = DB_PLUGIN_VFS;
	plugin.plugin.id = "vfs_rar";
	plugin.plugin.name = "RAR vfs";
	plugin.plugin.descr = "play files directly from rar files";
	plugin.plugin.copyright =
		"Copyright (C) 2011 Shao Hao <shaohao@users.sourceforge.net>\n"
		"\n"
		"This program is free software; you can redistribute it and/or\n"
		"modify it under the terms of the GNU General Public License\n"
		"as published by the Free Software Foundation; either version 2\n"
		"of the License, or (at your option) any later version.\n"
		"\n"
		"This program is distributed in the hope that it will be useful,\n"
		"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		"GNU General Public License for more details.\n"
		"\n"
		"You should have received a copy of the GNU General Public License\n"
		"along with this program; if not, write to the Free Software\n"
		"Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n"
		"\n"
		"\n"
		"UnRAR source (C) Alexander RoshalUnRAR";
	plugin.plugin.website = "http://github.com/shaohao/vfs_rar";
	plugin.open = vfs_rar_open;
	plugin.close = vfs_rar_close;
	plugin.read = vfs_rar_read;
	plugin.seek = vfs_rar_seek;
	plugin.tell = vfs_rar_tell;
	plugin.rewind = vfs_rar_rewind;
	plugin.getlength = vfs_rar_getlength;
	plugin.get_schemes = vfs_rar_get_schemes;
	plugin.is_streaming = vfs_rar_is_streaming;
	plugin.is_container = vfs_rar_is_container;
	plugin.scandir = vfs_rar_scandir;

	return DB_PLUGIN(&plugin);
}

