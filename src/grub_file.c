/* grubberize.c - The elastic binding between grub and standalone EFI */
/*
 *  Copyright © 2014 Pete Batard <pete@akeo.ie>
 *  Based on GRUB  --  GRand Unified Bootloader
 *  Copyright © 2001-2014 Free Software Foundation, Inc.
 *  Path sanitation code by Ludwig Nussel <ludwig.nussel@suse.de>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <efi.h>
#include <efilib.h>

#include <grub/err.h>
#include <grub/misc.h>
#include <grub/disk.h>
#include <grub/fs.h>
#include <grub/dl.h>
#include <grub/file.h>

#include "driver.h"

/* The file system list should only ever contain one element */
grub_fs_t grub_fs_list = NULL;

extern EFI_STATUS GrubErrToEFIStatus(grub_err_t err);

/* Don't care about refcounts for a standalone EFI FS driver */
int
grub_dl_ref(grub_dl_t mod) {
	return 0;
}

int
grub_dl_unref(grub_dl_t mod) {
	return 0;
};

grub_disk_read_hook_t grub_file_progress_hook = NULL;

grub_err_t
grub_disk_read(grub_disk_t disk, grub_disk_addr_t sector,
		grub_off_t offset, grub_size_t size, void *buf)
{
	EFI_STATUS Status;
	EFI_FS* FileSystem = (EFI_FS *) disk->data;

	if ((FileSystem == NULL) || (FileSystem->DiskIo == NULL) || (FileSystem->BlockIo == NULL))
		return GRUB_ERR_READ_ERROR;

	/* NB: We could get the actual blocksize through FileSystem->BlockIo->Media->BlockSize
	 * but GRUB uses the fixed GRUB_DISK_SECTOR_SIZE, so we follow suit
	 */
	Status = FileSystem->DiskIo->ReadDisk(FileSystem->DiskIo, FileSystem->BlockIo->Media->MediaId,
			sector * GRUB_DISK_SECTOR_SIZE + offset, size, buf);

	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not read block at address %08x", sector);
		return GRUB_ERR_READ_ERROR;
	}

	return 0;
}

// TODO: btrfs DOES call on grub_device_open() with an actual name => we'll have to handle that!
grub_device_t 
grub_device_open(const char *name)
{
	struct grub_device* device;

	device = grub_zalloc(sizeof(struct grub_device));
	if (device == NULL)
		return NULL;
	device->disk = grub_zalloc(sizeof(struct grub_disk));
	if (device->disk == NULL) {
		grub_free(device);
		return NULL;
	}
	/* The private disk data is a pointer back to our EFI_FS */
	device->disk->data = (void *) name;
	/* Ideally, we'd fill the other disk data, such as total_sectors, name
	 * and so on, but since we're doing the actual disk access through EFI
	 * DiskIO rather than GRUB's disk.c, this doesn't seem to be needed.
	 */

	return device;
}

grub_err_t 
grub_device_close(grub_device_t device)
{
	grub_free(device->disk);
	grub_free(device);
	return 0;
}

EFI_STATUS 
GrubDeviceInit(EFI_FS *This)
{
	// TODO: We hijack the name parameter to pass an EFI_FS, but some filesystems
	// such as btrfs have their own calls to grub_device_open() using a name.
	// => Use a name <-> EFI_FS table, with the DevicePath string as our ID.
	This->GrubDevice = (VOID *) grub_device_open((const char *) This);
	if (This->GrubDevice == NULL)
		return EFI_OUT_OF_RESOURCES;

	return EFI_SUCCESS;
}

EFI_STATUS
GrubDeviceExit(EFI_FS *This)
{
	grub_device_close((grub_device_t) This->GrubDevice);

	return EFI_SUCCESS;
}

EFI_STATUS
GrubCreateFile(EFI_GRUB_FILE **File, EFI_FS *This)
{
	EFI_GRUB_FILE *NewFile;
	grub_file_t f;

	NewFile = AllocateZeroPool(sizeof(*NewFile));
	if (NewFile == NULL)
		return EFI_OUT_OF_RESOURCES;
	NewFile->GrubFile = AllocateZeroPool(sizeof(*f));
	if (NewFile->GrubFile == NULL) {
		FreePool(NewFile);
		return EFI_OUT_OF_RESOURCES;
	}

	/* Initialize the attributes */
	NewFile->FileSystem = This;
	CopyMem(&NewFile->EfiFile, &This->RootFile->EfiFile, sizeof(EFI_FILE));

	f = (grub_file_t) NewFile->GrubFile;
	f->device = (grub_device_t) This->GrubDevice;
	f->fs = grub_fs_list;

	*File = NewFile;
	return EFI_SUCCESS;
}

VOID
GrubDestroyFile(EFI_GRUB_FILE *File)
{
	FreePool(File->GrubFile);
	FreePool(File);
}

UINT64
GrubGetFileSize(EFI_GRUB_FILE *File)
{
	grub_file_t f = (grub_file_t) File->GrubFile;
	return (UINT64) f->size;
}

UINT64
GrubGetFileOffset(EFI_GRUB_FILE *File)
{
	grub_file_t f = (grub_file_t) File->GrubFile;
	return (UINT64) f->offset;
}

VOID GrubSetFileOffset(EFI_GRUB_FILE *File, UINT64 Offset)
{
	grub_file_t f = (grub_file_t) File->GrubFile;
	f->offset = (grub_off_t) Offset;
}

/*
 * The following provides an EFI interface for each basic GRUB fs call
 */
EFI_STATUS
GrubDir(EFI_GRUB_FILE *File, const CHAR8 *path,
		GRUB_DIRHOOK Hook, VOID *HookData)
{
	grub_fs_t p = grub_fs_list;
	grub_file_t f = (grub_file_t) File->GrubFile;
	grub_err_t rc;

	grub_errno = 0;
	rc = p->dir(f->device, path, (grub_fs_dir_hook_t) Hook, HookData);
	return GrubErrToEFIStatus(rc);
}

EFI_STATUS
GrubOpen(EFI_GRUB_FILE *File)
{
	grub_fs_t p = grub_fs_list;
	grub_file_t f = (grub_file_t) File->GrubFile;
	grub_err_t rc;

	grub_errno = 0;
	rc = p->open(f, File->path);
	return GrubErrToEFIStatus(rc);
}

VOID
GrubClose(EFI_GRUB_FILE *File)
{
	grub_fs_t p = grub_fs_list;
	grub_file_t f = (grub_file_t) File->GrubFile;

	grub_errno = 0;
	p->close(f);
}

EFI_STATUS
GrubRead(EFI_GRUB_FILE *File, VOID *Data, UINTN *Len)
{
	grub_fs_t p = grub_fs_list;
	grub_file_t f = (grub_file_t) File->GrubFile;
	grub_ssize_t len;
	INTN Remaining;

	/* GRUB may return an error if we request more data than available */
	Remaining = f->size - f->offset;

	if (*Len > Remaining)
		*Len = Remaining;

	len = p->read(f, (char *) Data, *Len);

	if (len < 0) {
		*Len = 0;
		return GrubErrToEFIStatus(grub_errno);
	}

	/* You'd think that GRUB read() would increase the offset... */
	f->offset += len; 
	*Len = len;

	return EFI_SUCCESS;
}

EFI_STATUS
GrubLabel(EFI_GRUB_FILE *File, CHAR8 **label)
{
	grub_fs_t p = grub_fs_list;
	grub_file_t f = (grub_file_t) File->GrubFile;
	grub_err_t rc;
	
	grub_errno = 0;
	rc = p->label(f->device, (char **) label);
	return GrubErrToEFIStatus(rc);
}

/* Helper for GrubFSProbe.  */
static int
probe_dummy_iter (const char *filename __attribute__ ((unused)),
		const struct grub_dirhook_info *info __attribute__ ((unused)),
		void *data __attribute__ ((unused)))
{
	return 1;
}

BOOLEAN 
GrubFSProbe(EFI_FS *This)
{
	grub_fs_t p = grub_fs_list;
	grub_device_t device = (grub_device_t) This->GrubDevice;

	if ((p == NULL) || (device->disk == NULL)) {
		PrintError(L"GrubFSProbe: uninitialized variables\n"); 
		return FALSE;
	}

	grub_errno = 0;
	(p->dir)(device, "/", probe_dummy_iter, NULL);
	if (grub_errno != 0) {
		if (LogLevel >= FS_LOGLEVEL_INFO)
			grub_print_error();	/* NB: this call will reset grub_errno */
		return FALSE;
	}
	return TRUE;
}

CHAR16 *
GrubGetUuid(EFI_FS* This)
{
	EFI_STATUS Status;
	grub_fs_t p = grub_fs_list;
	grub_device_t device = (grub_device_t) This->GrubDevice;
	static CHAR16 Uuid[36];
	char* uuid;

	if (p->uuid(device, &uuid) || (uuid == NULL))
		return NULL;

	Status = Utf8ToUtf16NoAlloc(uuid, Uuid, ARRAYSIZE(Uuid));
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not convert UUID to UTF-16");
		return NULL;
	}

	return Uuid;
}