/**
 * Copyright (c) 2013 Anup Patel.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * @file fat_node.c
 * @author Anup Patel. (anup@brainfault.org)
 * @brief source file for FAT node functions
 */

#include <vmm_error.h>
#include <vmm_heap.h>
#include <libs/stringlib.h>
#include <libs/mathlib.h>

#include "fat_control.h"
#include "fat_node.h"

u32 fatfs_node_read(struct fatfs_node *node, u64 pos, u32 len, char *buf)
{
	int rc;
	u32 i, j;
	u32 cl_pos, cl_off, cl_num, cl_len;
	u64 rlen, roff;
	struct fatfs_control *ctrl = node->ctrl;

	if (!node->parent && ctrl->type != FAT_TYPE_32) {
		rlen = (u64)ctrl->bytes_per_sector * ctrl->fat_sectors;
		if (pos >= rlen) {
			return 0;
		}
		if ((pos + len) > rlen) {
			rlen = rlen - pos;
		} else {
			rlen = len;
		}
		roff = (u64)ctrl->first_root_sector * ctrl->bytes_per_sector
		       + pos;
		return vmm_blockdev_read(ctrl->bdev, (u8 *)buf, roff, rlen);
	}

	i = 0;
	while (i < len) {
		/* Get the next cluster */
		if (i == 0) {
			cl_pos = udiv64(pos, ctrl->bytes_per_cluster); 
			cl_off = pos - cl_pos * ctrl->bytes_per_cluster;
			cl_num = node->first_cluster;
			for (j = 0; j < cl_pos; j++) {
				rc = fatfs_control_next_cluster(ctrl, cl_num, &cl_num);
				if (rc) {
					return 0;
				}
			}
			cl_len = ctrl->bytes_per_cluster - cl_off;
			cl_len = (cl_len < len) ? cl_len : len;
		} else {
			cl_pos++;
			cl_off = 0;
			rc = fatfs_control_next_cluster(ctrl, cl_num, &cl_num);
			if (rc) {
				return i;
			}
			cl_len = (ctrl->bytes_per_cluster < len) ? 
						ctrl->bytes_per_cluster : len;
		}

		/* Make sure cached cluster is updated */
		if (!node->cached_data) {
			node->cached_data = 
					vmm_malloc(ctrl->bytes_per_cluster);
			if (!node->cached_data) {
				return 0;
			}
		}
		if (node->cached_cluster != cl_num) {
			if (node->cached_dirty) {
				/* FIXME: Write back dirty cached cluster */

				node->cached_dirty = FALSE;
			}

			node->cached_cluster = cl_num;

			roff = ((u64)ctrl->first_data_sector
			        * ctrl->bytes_per_sector) +
				((cl_num - 2) * ctrl->bytes_per_cluster);
			rlen = vmm_blockdev_read(ctrl->bdev, 
						node->cached_data, 
						roff, ctrl->bytes_per_cluster);
			if (rlen != ctrl->bytes_per_cluster) {
				return i;
			}
		}

		/* Read from cached cluster */
		memcpy(buf, &node->cached_data[cl_off], cl_len);

		/* Update iteration */
		i += cl_len;
		buf += cl_len;
	}

	return i;
}

u64 fatfs_node_get_size(struct fatfs_node *node)
{
	if (!node->parent) {
		return 0;
	}

	return __le32(node->dirent.file_size);
}

int fatfs_node_sync(struct fatfs_node *node)
{
	int i;

	for (i = 0; i < FAT_NODE_CACHE_SIZE; i++) {
		if (node->cached_dirty) {
			/* FIXME: Write back dirty node cluster */

			node->cached_dirty = FALSE;
		}
	}

	return VMM_OK;
}

int fatfs_node_init(struct fatfs_node *node)
{
	int idx;

	node->ctrl = NULL;
	node->parent = NULL;
	node->parent_dirent_off = 0;
	node->parent_dirent_len = 0;
	memset(&node->dirent, 0, sizeof(struct fat_dirent));
	node->first_cluster = 0;

	node->cached_cluster = 0;
	node->cached_data = NULL;
	node->cached_dirty = FALSE;

	node->lookup_victim = 0;
	for (idx = 0; idx < FAT_NODE_LOOKUP_SIZE; idx++) {
		node->lookup_name[idx][0] = '\0';
		node->lookup_off[idx] = 0;
		node->lookup_len[idx] = 0;
	}

	return VMM_OK;
}

int fatfs_node_exit(struct fatfs_node *node)
{
	if (node->cached_data) {
		vmm_free(node->cached_data);
		node->cached_cluster = 0;
		node->cached_data = NULL;
		node->cached_dirty = FALSE;
	}

	return VMM_OK;
}

static int fatfs_node_find_lookup_dirent(struct fatfs_node *dnode, 
					 const char *name, 
					 struct fat_dirent *dent,
					 u32 *off, u32 *len)
{
	int idx;

	if (name[0] == '\0') {
		return -1;
	}

	for (idx = 0; idx < FAT_NODE_LOOKUP_SIZE; idx++) {
		if (!strcmp(dnode->lookup_name[idx], name)) {
			memcpy(dent, &dnode->lookup_dent[idx], sizeof(*dent));
			*off = dnode->lookup_off[idx];
			*len = dnode->lookup_len[idx];
			return idx;
		}
	}

	return -1;
}

static void fatfs_node_add_lookup_dirent(struct fatfs_node *dnode, 
					 const char *name, 
					 struct fat_dirent *dent, 
					 u32 off, u32 len)
{
	int idx;
	bool found = FALSE;

	if (name[0] == '\0') {
		return;
	}

	for (idx = 0; idx < FAT_NODE_LOOKUP_SIZE; idx++) {
		if (!strcmp(dnode->lookup_name[idx], name)) {
			found = TRUE;
			break;
		}
	}

	if (!found) {
		idx = dnode->lookup_victim;
		if (strlcpy(&dnode->lookup_name[idx][0], name,
		    sizeof(dnode->lookup_name[idx])) >=
		    sizeof(dnode->lookup_name[idx])) {
			return;
		}
		dnode->lookup_victim++;
		if (dnode->lookup_victim == FAT_NODE_LOOKUP_SIZE) {
			dnode->lookup_victim = 0;
		}
		memcpy(&dnode->lookup_dent[idx], dent, sizeof(*dent));
		dnode->lookup_off[idx] = off;
		dnode->lookup_len[idx] = len;
	}
}

#if 0 /* TODO: To be used later. */
static void fatfs_node_del_lookup_dirent(struct fatfs_node *dnode, 
					  const char *name)
{
	int idx;

	if (name[0] == '\0') {
		return;
	}

	for (idx = 0; idx < FAT_NODE_LOOKUP_SIZE; idx++) {
		if (!strcmp(dnode->lookup_name[idx], name)) {
			dnode->lookup_name[idx][0] = '\0';
			dnode->lookup_off[idx] = 0;
			dnode->lookup_len[idx] = 0;
			break;
		}
	}

}
#endif

int fatfs_node_read_dirent(struct fatfs_node *dnode, 
			    loff_t off, struct dirent *d)
{
	u32 i, rlen, len;
	char lname[VFS_MAX_NAME];
	struct fat_dirent dent;
	struct fat_longname lfn;
	u64 fileoff = off;

	if (umod64(fileoff, sizeof(struct fat_dirent))) {
		return VMM_EINVALID;
	}

	memset(lname, 0, sizeof(lname));
	d->d_off = off;
	d->d_reclen = 0;

	do {
		rlen = fatfs_node_read(dnode, fileoff, 
				sizeof(struct fat_dirent), (char *)&dent);
		if (rlen != sizeof(struct fat_dirent)) {
			return VMM_EIO;
		}

		if (dent.dos_file_name[0] == 0x0) {
			return VMM_ENOENT;
		}

		d->d_reclen += sizeof(struct fat_dirent);
		fileoff += sizeof(struct fat_dirent);

		if ((dent.dos_file_name[0] == 0xE5) ||
		    (dent.dos_file_name[0] == 0x2E)) {
			continue;
		}

		if (dent.file_attributes == FAT_LONGNAME_ATTRIBUTE) {
			memcpy(&lfn, &dent, sizeof(struct fat_longname));
			if (FAT_LONGNAME_LASTSEQ(lfn.seqno)) {
				memset(lname, 0, sizeof(lname));
				lfn.seqno = FAT_LONGNAME_SEQNO(lfn.seqno);
			}
			if ((lfn.seqno < FAT_LONGNAME_MINSEQ) ||
			    (FAT_LONGNAME_MAXSEQ < lfn.seqno)) {
				continue;
			}
			len = (lfn.seqno - 1) * 13;
			lname[len + 0] = (char)__le16(lfn.name_utf16_1[0]);
			lname[len + 1] = (char)__le16(lfn.name_utf16_1[1]);
			lname[len + 2] = (char)__le16(lfn.name_utf16_1[2]);
			lname[len + 3] = (char)__le16(lfn.name_utf16_1[3]);
			lname[len + 4] = (char)__le16(lfn.name_utf16_1[4]);
			lname[len + 5] = (char)__le16(lfn.name_utf16_2[0]);
			lname[len + 6] = (char)__le16(lfn.name_utf16_2[1]);
			lname[len + 7] = (char)__le16(lfn.name_utf16_2[2]);
			lname[len + 8] = (char)__le16(lfn.name_utf16_2[3]);
			lname[len + 9] = (char)__le16(lfn.name_utf16_2[4]);
			lname[len + 10] = (char)__le16(lfn.name_utf16_2[5]);
			lname[len + 11] = (char)__le16(lfn.name_utf16_3[0]);
			lname[len + 12] = (char)__le16(lfn.name_utf16_3[1]);
			continue;
		}

		if (dent.file_attributes & FAT_DIRENT_VOLLABLE) {
			continue;
		}

		if (!strlen(lname)) {
			i = 8;
			while (i && (dent.dos_file_name[i-1] == ' ')) {
				dent.dos_file_name[i-1] = '\0';
				i--;
			}
			i = 3;
			while (i && (dent.dos_extension[i-1] == ' ')) {
				dent.dos_extension[i-1] = '\0';
				i--;
			}
			memcpy(lname, dent.dos_file_name, 8);
			if (dent.dos_extension[0] != '\0') {
				len = strlen(lname);
				lname[len] = '.';
				lname[len + 1] = dent.dos_extension[0];
				lname[len + 2] = dent.dos_extension[1];
				lname[len + 3] = dent.dos_extension[2];
				lname[len + 4] = '\0';
			}
		}

		if (strlcpy(d->d_name, lname, sizeof(d->d_name)) >=
		    sizeof(d->d_name)) {
			return VMM_EOVERFLOW;
		}

		break;
	} while (1);

	if (dent.file_attributes & FAT_DIRENT_SUBDIR) {
		d->d_type = DT_DIR;
	} else {
		d->d_type = DT_REG;
	}

	/* Add dent to lookup table */
	fatfs_node_add_lookup_dirent(dnode, d->d_name, 
				     &dent, d->d_off, d->d_reclen);

	return VMM_OK;
}

int fatfs_node_find_dirent(struct fatfs_node *dnode, 
			   const char *name,
			   struct fat_dirent *dent, 
			   u32 *dent_off, u32 *dent_len)
{
	u32 i, rlen, len, lfn_off, lfn_len;
	struct fat_longname lfn;
	char lname[VFS_MAX_NAME];
	u64 off;

	/* Try to find in lookup table */
	if (fatfs_node_find_lookup_dirent(dnode, name, 
					  dent, dent_off, dent_len) > -1) {
		return VMM_OK;
	}

	lfn_off = 0;
	lfn_len = 0;
	memset(lname, 0, sizeof(lname));

	off = 0;
	while (1) {
		rlen = fatfs_node_read(dnode, off, 
				sizeof(struct fat_dirent), (char *)dent);
		if (rlen != sizeof(struct fat_dirent)) {
			return VMM_EIO;
		}

		if (dent->dos_file_name[0] == 0x0) {
			return VMM_ENOENT;
		}

		off += sizeof(struct fat_dirent);

		if ((dent->dos_file_name[0] == 0xE5) ||
		    (dent->dos_file_name[0] == 0x2E)) {
			continue;
		}

		if (dent->file_attributes == FAT_LONGNAME_ATTRIBUTE) {
			memcpy(&lfn, dent, sizeof(struct fat_longname));
			if (FAT_LONGNAME_LASTSEQ(lfn.seqno)) {
				lfn.seqno = FAT_LONGNAME_SEQNO(lfn.seqno);
				lfn_off = off - sizeof(struct fat_dirent);
				lfn_len = lfn.seqno * sizeof(struct fat_longname);
				memset(lname, 0, sizeof(lname));
			}
			if ((lfn.seqno < FAT_LONGNAME_MINSEQ) ||
			    (FAT_LONGNAME_MAXSEQ < lfn.seqno)) {
				continue;
			}
			len = (lfn.seqno - 1) * 13;
			lname[len + 0] = (char)__le16(lfn.name_utf16_1[0]);
			lname[len + 1] = (char)__le16(lfn.name_utf16_1[1]);
			lname[len + 2] = (char)__le16(lfn.name_utf16_1[2]);
			lname[len + 3] = (char)__le16(lfn.name_utf16_1[3]);
			lname[len + 4] = (char)__le16(lfn.name_utf16_1[4]);
			lname[len + 5] = (char)__le16(lfn.name_utf16_2[0]);
			lname[len + 6] = (char)__le16(lfn.name_utf16_2[1]);
			lname[len + 7] = (char)__le16(lfn.name_utf16_2[2]);
			lname[len + 8] = (char)__le16(lfn.name_utf16_2[3]);
			lname[len + 9] = (char)__le16(lfn.name_utf16_2[4]);
			lname[len + 10] = (char)__le16(lfn.name_utf16_2[5]);
			lname[len + 11] = (char)__le16(lfn.name_utf16_3[0]);
			lname[len + 12] = (char)__le16(lfn.name_utf16_3[1]);
			continue;
		}

		if (dent->file_attributes & FAT_DIRENT_VOLLABLE) {
			continue;
		}

		if (!strlen(lname)) {
			lfn_off = off - sizeof(struct fat_dirent);
			lfn_len = 0;
			i = 8;
			while (i && (dent->dos_file_name[i-1] == ' ')) {
				dent->dos_file_name[i-1] = '\0';
				i--;
			}
			i = 3;
			while (i && (dent->dos_extension[i-1] == ' ')) {
				dent->dos_extension[i-1] = '\0';
				i--;
			}
			memcpy(lname, dent->dos_file_name, 8);
			if (dent->dos_extension[0] != '\0') {
				len = strlen(lname);
				lname[len] = '.';
				lname[len + 1] = dent->dos_extension[0];
				lname[len + 2] = dent->dos_extension[1];
				lname[len + 3] = dent->dos_extension[2];
				lname[len + 4] = '\0';
			}
		}

		if (!strncmp(lname, name, VFS_MAX_NAME)) {
			*dent_off = lfn_off;
			*dent_len = sizeof(struct fat_dirent) + lfn_len;
			break;
		}

		lfn_off = off;
		lfn_len = 0;
		memset(lname, 0, sizeof(lname));
	}

	/* Add dent to lookup table */
	fatfs_node_add_lookup_dirent(dnode, lname, 
				     dent, *dent_off, *dent_len);

	return VMM_OK;
}

