/* mload.c (for PPC) (c) 2009, Hermes 

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HW_RVL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogcsys.h>
#include <gccore.h>
#include <ogc/ipc.h>
#include "unistd.h"
#include "ehcmodule_elf.h"

#define MLOAD_MLOAD_THREAD_ID	0x4D4C4400
#define MLOAD_LOAD_MODULE		0x4D4C4480
#define MLOAD_RUN_MODULE		0x4D4C4481
#define MLOAD_RUN_THREAD        0x4D4C4482

#define MLOAD_STOP_THREAD		0x4D4C4484
#define MLOAD_CONTINUE_THREAD   0x4D4C4485

#define MLOAD_GET_LOAD_BASE     0x4D4C4490
#define MLOAD_MEMSET			0x4D4C4491

#define MLOAD_GET_EHCI_DATA		0x4D4C44A0

#define MLOAD_SET_ES_IOCTLV		0x4D4C44B0

#define getbe32(x) ((adr[x]<<24) | (adr[x+1]<<16) | (adr[x+2]<<8) | (adr[x+3]))

extern void iosDestroyHeap(int hid);

typedef struct
{
	u32 ident0;
	u32 ident1;
	u32 ident2;
	u32 ident3;
	u32 machinetype;
	u32 version;
	u32 entry;
	u32 phoff;
	u32 shoff;
	u32 flags;
	u16 ehsize;
	u16 phentsize;
	u16 phnum;
	u16 shentsize;
	u16 shnum;
	u16 shtrndx;
} elfheader;

typedef struct
{
	u32 type;
	u32 offset;
	u32 vaddr;
	u32 paddr;
	u32 filesz;
	u32 memsz;
	u32 flags;
	u32 align;
} elfphentry;

typedef struct
{
	void *start;
	int prio;
	void *stack;
	int size_stack;
} data_elf;

static const char mload_fs[] ATTRIBUTE_ALIGN(32) = "/dev/mload";
static s32 mload_fd = -1;

// to init/test if the device is running
int mload_init()
{
	int n;

	if (mload_fd >= 0)
		return 0;

	for (n = 0; n < 10; n++) // try 2.5 seconds
	{
		mload_fd = IOS_Open(mload_fs, 0);

		if (mload_fd >= 0)
			break;

		usleep(250 * 1000);
	}

	return mload_fd;
}

// to close the device (remember call it when rebooting the IOS!)
int mload_close()
{
	int ret;

	if (mload_fd < 0)
		return -1;

	ret = IOS_Close(mload_fd);

	mload_fd = -1;

	return ret;
}

// fix starlet address to read/write (uses SEEK_SET, etc as mode)
static int mload_seek(int offset, int mode)
{
	if (mload_init() < 0)
		return -1;
	return IOS_Seek(mload_fd, offset, mode);
}

// write bytes from starlet (it update the offset)
static int mload_write(const void * buf, u32 size)
{
	if (mload_init() < 0)
		return -1;
	return IOS_Write(mload_fd, buf, size);
}

// fill a block (similar to memset)
static int mload_memset(void *starlet_addr, int set, int len)
{
	int ret;
	s32 hid = -1;

	if (mload_init() < 0)
		return -1;

	hid = iosCreateHeap(0x800);

	if (hid < 0)
		return hid;

	ret = IOS_IoctlvFormat(hid, mload_fd, MLOAD_MEMSET, "iii:", starlet_addr,
			set, len);

	iosDestroyHeap(hid);

	return ret;
}

// load a module from the PPC
// the module must be a elf made with stripios
static int mload_elf(void *my_elf, data_elf *data_elf)
{
	int n, m;
	int p;
	u8 *adr;
	u32 elf = (u32) my_elf;

	if (elf & 3)
		return -1; // aligned to 4 please!

	elfheader *head = (void *) elf;
	elfphentry *entries;

	if (head->ident0 != 0x7F454C46)
		return -1;
	if (head->ident1 != 0x01020161)
		return -1;
	if (head->ident2 != 0x01000000)
		return -1;

	p = head->phoff;

	data_elf->start = (void *) head->entry;

	for (n = 0; n < head->phnum; n++)
	{
		entries = (void *) (elf + p);
		p += sizeof(elfphentry);

		if (entries->type == 4)
		{
			adr = (void *) (elf + entries->offset);

			if (getbe32(0) != 0)
				return -2; // bad info (sure)

			for (m = 4; m < entries->memsz; m += 8)
			{
				switch (getbe32(m))
				{
				case 0x9:
					data_elf->start = (void *) getbe32(m+4);
					break;
				case 0x7D:
					data_elf->prio = getbe32(m+4);
					break;
				case 0x7E:
					data_elf->size_stack = getbe32(m+4);
					break;
				case 0x7F:
					data_elf->stack = (void *) (getbe32(m+4));
					break;

				}
			}
		}
		else if (entries->type == 1 && entries->memsz != 0 && entries->vaddr != 0)
		{

			if (mload_memset((void *) entries->vaddr, 0, entries->memsz) < 0)
				return -1;
			if (mload_seek(entries->vaddr, SEEK_SET) < 0)
				return -1;
			if (mload_write((void *) (elf + entries->offset), entries->filesz) < 0)
				return -1;
		}
	}

	return 0;
}

// run one thread (you can use to load modules or binary files)
static int mload_run_thread(void *starlet_addr, void *starlet_top_stack,
		int stack_size, int priority)
{
	int ret;
	s32 hid = -1;

	if (mload_init() < 0)
		return -1;

	hid = iosCreateHeap(0x800);

	if (hid < 0)
		return hid;

	ret = IOS_IoctlvFormat(hid, mload_fd, MLOAD_RUN_THREAD, "iiii:",
			starlet_addr, starlet_top_stack, stack_size, priority);

	iosDestroyHeap(hid);

	return ret;
}

bool load_ehci_module()
{
	data_elf my_data_elf;
	my_data_elf.start = NULL;
	my_data_elf.prio = 0;
	my_data_elf.stack = NULL;
	my_data_elf.size_stack = 0;

	if(mload_elf((void *) ehcmodule_elf, &my_data_elf) != 0)
		return false;

	if (mload_run_thread(my_data_elf.start, my_data_elf.stack,
			my_data_elf.size_stack, my_data_elf.prio) < 0)
	{
		usleep(1000);
		if (mload_run_thread(my_data_elf.start, my_data_elf.stack,
				my_data_elf.size_stack, 0x47) < 0)
			return false;
	}
	return true;
}

#endif
