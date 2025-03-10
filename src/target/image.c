// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2007 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2007,2008 Øyvind Harboe                                 *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2009 by Franck Hereson                                  *
 *   franck.hereson@secad.fr                                               *
 *                                                                         *
 *   Copyright (C) 2018 by Advantest                                       *
 *   florian.meister@advantest.com                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "image.h"
#include "target.h"
#include <helper/log.h>
#include <server/server.h>

/* convert ELF header field to host endianness */
#define field16(elf, field) \
	((elf->endianness == ELFDATA2LSB) ? \
	le_to_h_u16((uint8_t *)&field) : be_to_h_u16((uint8_t *)&field))

#define field32(elf, field) \
	((elf->endianness == ELFDATA2LSB) ? \
	le_to_h_u32((uint8_t *)&field) : be_to_h_u32((uint8_t *)&field))

#define field64(elf, field) \
	((elf->endianness == ELFDATA2LSB) ? \
	le_to_h_u64((uint8_t *)&field) : be_to_h_u64((uint8_t *)&field))

static int autodetect_image_type(struct image *image, const char *url)
{
	int retval;
	struct fileio *fileio;
	size_t read_bytes;
	uint8_t buffer[9];

	/* read the first 9 bytes of image */
	retval = fileio_open(&fileio, url, FILEIO_READ, FILEIO_BINARY);
	if (retval != ERROR_OK)
		return retval;
	retval = fileio_read(fileio, 9, buffer, &read_bytes);
	fileio_close(fileio);

	/* If the file is smaller than 9 bytes, it can only be bin */
	if (retval == ERROR_OK && read_bytes != 9) {
		LOG_DEBUG("Less than 9 bytes in the image file found.");
		LOG_DEBUG("BIN image detected.");
		image->type = IMAGE_BINARY;
		return ERROR_OK;
	}

	if (retval != ERROR_OK)
		return retval;

	/* check header against known signatures */
	if (strncmp((char *)buffer, ELFMAG, SELFMAG) == 0) {
		LOG_DEBUG("ELF image detected.");
		image->type = IMAGE_ELF;
	} else if ((buffer[0] == ':')	/* record start byte */
		&& (isxdigit(buffer[1]))
		&& (isxdigit(buffer[2]))
		&& (isxdigit(buffer[3]))
		&& (isxdigit(buffer[4]))
		&& (isxdigit(buffer[5]))
		&& (isxdigit(buffer[6]))
		&& (buffer[7] == '0')	/* record type : 00 -> 05 */
		&& (buffer[8] >= '0') && (buffer[8] < '6')) {
		LOG_DEBUG("IHEX image detected.");
		image->type = IMAGE_IHEX;
	} else if ((buffer[0] == 'S')	/* record start byte */
		&& (isxdigit(buffer[1]))
		&& (isxdigit(buffer[2]))
		&& (isxdigit(buffer[3]))
		&& (buffer[1] >= '0') && (buffer[1] < '9')) {
		LOG_DEBUG("S19 image detected.");
		image->type = IMAGE_SRECORD;
	} else {
		LOG_DEBUG("BIN image detected.");
		image->type = IMAGE_BINARY;
	}

	return ERROR_OK;
}

static int identify_image_type(struct image *image, const char *type_string, const char *url)
{
	if (type_string) {
		if (!strcmp(type_string, "bin")) {
			image->type = IMAGE_BINARY;
		} else if (!strcmp(type_string, "ihex")) {
			image->type = IMAGE_IHEX;
		} else if (!strcmp(type_string, "elf")) {
			image->type = IMAGE_ELF;
		} else if (!strcmp(type_string, "mem")) {
			image->type = IMAGE_MEMORY;
		} else if (!strcmp(type_string, "s19")) {
			image->type = IMAGE_SRECORD;
		} else if (!strcmp(type_string, "build")) {
			image->type = IMAGE_BUILDER;
		} else {
			LOG_ERROR("Unknown image type: %s, use one of: bin, ihex, elf, mem, s19, build", type_string);
			return ERROR_IMAGE_TYPE_UNKNOWN;
		}
	} else
		return autodetect_image_type(image, url);

	return ERROR_OK;
}

static int image_ihex_buffer_complete_inner(struct image *image,
	char *lpsz_line,
	struct imagesection *section)
{
	struct image_ihex *ihex = image->type_private;
	struct fileio *fileio = ihex->fileio;
	uint32_t full_address;
	uint32_t cooked_bytes;
	bool end_rec = false;

	/* we can't determine the number of sections that we'll have to create ahead of time,
	 * so we locally hold them until parsing is finished */

	size_t filesize;
	int retval;
	retval = fileio_size(fileio, &filesize);
	if (retval != ERROR_OK)
		return retval;

	ihex->buffer = malloc(filesize >> 1);
	cooked_bytes = 0x0;
	image->num_sections = 0;

	while (!fileio_feof(fileio)) {
		full_address = 0x0;
		section[image->num_sections].private = &ihex->buffer[cooked_bytes];
		section[image->num_sections].base_address = 0x0;
		section[image->num_sections].size = 0x0;
		section[image->num_sections].flags = 0;

		while (fileio_fgets(fileio, 1023, lpsz_line) == ERROR_OK) {
			uint32_t count;
			uint32_t address;
			uint32_t record_type;
			uint32_t checksum;
			uint8_t cal_checksum = 0;
			size_t bytes_read = 0;

			/* skip comments and blank lines */
			if ((lpsz_line[0] == '#') || (strlen(lpsz_line + strspn(lpsz_line, "\n\t\r ")) == 0))
				continue;

			if (sscanf(&lpsz_line[bytes_read], ":%2" SCNx32 "%4" SCNx32 "%2" SCNx32, &count,
				&address, &record_type) != 3)
				return ERROR_IMAGE_FORMAT_ERROR;
			bytes_read += 9;

			cal_checksum += (uint8_t)count;
			cal_checksum += (uint8_t)(address >> 8);
			cal_checksum += (uint8_t)address;
			cal_checksum += (uint8_t)record_type;

			if (record_type == 0) {	/* Data Record */
				if ((full_address & 0xffff) != address) {
					/* we encountered a nonconsecutive location, create a new section,
					 * unless the current section has zero size, in which case this specifies
					 * the current section's base address
					 */
					if (section[image->num_sections].size != 0) {
						image->num_sections++;
						if (image->num_sections >= IMAGE_MAX_SECTIONS) {
							/* too many sections */
							LOG_ERROR("Too many sections found in IHEX file");
							return ERROR_IMAGE_FORMAT_ERROR;
						}
						section[image->num_sections].size = 0x0;
						section[image->num_sections].flags = 0;
						section[image->num_sections].private =
							&ihex->buffer[cooked_bytes];
					}
					section[image->num_sections].base_address =
						(full_address & 0xffff0000) | address;
					full_address = (full_address & 0xffff0000) | address;
				}

				while (count-- > 0) {
					unsigned int value;
					sscanf(&lpsz_line[bytes_read], "%2x", &value);
					ihex->buffer[cooked_bytes] = (uint8_t)value;
					cal_checksum += (uint8_t)ihex->buffer[cooked_bytes];
					bytes_read += 2;
					cooked_bytes += 1;
					section[image->num_sections].size += 1;
					full_address++;
				}
			} else if (record_type == 1) {	/* End of File Record */
				/* finish the current section */
				image->num_sections++;

				/* copy section information */
				image->sections = malloc(sizeof(struct imagesection) * image->num_sections);
				for (unsigned int i = 0; i < image->num_sections; i++) {
					image->sections[i].private = section[i].private;
					image->sections[i].base_address = section[i].base_address;
					image->sections[i].size = section[i].size;
					image->sections[i].flags = section[i].flags;
				}

				end_rec = true;
				break;
			} else if (record_type == 2) {	/* Linear Address Record */
				uint16_t upper_address;

				sscanf(&lpsz_line[bytes_read], "%4hx", &upper_address);
				cal_checksum += (uint8_t)(upper_address >> 8);
				cal_checksum += (uint8_t)upper_address;
				bytes_read += 4;

				if ((full_address >> 4) != upper_address) {
					/* we encountered a nonconsecutive location, create a new section,
					 * unless the current section has zero size, in which case this specifies
					 * the current section's base address
					 */
					if (section[image->num_sections].size != 0) {
						image->num_sections++;
						if (image->num_sections >= IMAGE_MAX_SECTIONS) {
							/* too many sections */
							LOG_ERROR("Too many sections found in IHEX file");
							return ERROR_IMAGE_FORMAT_ERROR;
						}
						section[image->num_sections].size = 0x0;
						section[image->num_sections].flags = 0;
						section[image->num_sections].private =
							&ihex->buffer[cooked_bytes];
					}
					section[image->num_sections].base_address =
						(full_address & 0xffff) | (upper_address << 4);
					full_address = (full_address & 0xffff) | (upper_address << 4);
				}
			} else if (record_type == 3) {	/* Start Segment Address Record */
				uint32_t dummy;

				/* "Start Segment Address Record" will not be supported
				 * but we must consume it, and do not create an error.  */
				while (count-- > 0) {
					sscanf(&lpsz_line[bytes_read], "%2" SCNx32, &dummy);
					cal_checksum += (uint8_t)dummy;
					bytes_read += 2;
				}
			} else if (record_type == 4) {	/* Extended Linear Address Record */
				uint16_t upper_address;

				sscanf(&lpsz_line[bytes_read], "%4hx", &upper_address);
				cal_checksum += (uint8_t)(upper_address >> 8);
				cal_checksum += (uint8_t)upper_address;
				bytes_read += 4;

				if ((full_address >> 16) != upper_address) {
					/* we encountered a nonconsecutive location, create a new section,
					 * unless the current section has zero size, in which case this specifies
					 * the current section's base address
					 */
					if (section[image->num_sections].size != 0) {
						image->num_sections++;
						if (image->num_sections >= IMAGE_MAX_SECTIONS) {
							/* too many sections */
							LOG_ERROR("Too many sections found in IHEX file");
							return ERROR_IMAGE_FORMAT_ERROR;
						}
						section[image->num_sections].size = 0x0;
						section[image->num_sections].flags = 0;
						section[image->num_sections].private =
							&ihex->buffer[cooked_bytes];
					}
					section[image->num_sections].base_address =
						(full_address & 0xffff) | (upper_address << 16);
					full_address = (full_address & 0xffff) | (upper_address << 16);
				}
			} else if (record_type == 5) {	/* Start Linear Address Record */
				uint32_t start_address;

				sscanf(&lpsz_line[bytes_read], "%8" SCNx32, &start_address);
				cal_checksum += (uint8_t)(start_address >> 24);
				cal_checksum += (uint8_t)(start_address >> 16);
				cal_checksum += (uint8_t)(start_address >> 8);
				cal_checksum += (uint8_t)start_address;
				bytes_read += 8;

				image->start_address_set = true;
				image->start_address = be_to_h_u32((uint8_t *)&start_address);
			} else {
				LOG_ERROR("unhandled IHEX record type: %i", (int)record_type);
				return ERROR_IMAGE_FORMAT_ERROR;
			}

			sscanf(&lpsz_line[bytes_read], "%2" SCNx32, &checksum);

			if ((uint8_t)checksum != (uint8_t)(~cal_checksum + 1)) {
				/* checksum failed */
				LOG_ERROR("incorrect record checksum found in IHEX file");
				return ERROR_IMAGE_CHECKSUM;
			}

			if (end_rec) {
				end_rec = false;
				LOG_WARNING("continuing after end-of-file record: %.40s", lpsz_line);
			}
		}
	}

	if (end_rec)
		return ERROR_OK;
	else {
		LOG_ERROR("premature end of IHEX file, no matching end-of-file record found");
		return ERROR_IMAGE_FORMAT_ERROR;
	}
}

/**
 * Allocate memory dynamically instead of on the stack. This
 * is important w/embedded hosts.
 */
static int image_ihex_buffer_complete(struct image *image)
{
	char *lpsz_line = malloc(1023);
	if (!lpsz_line) {
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}
	struct imagesection *section = malloc(sizeof(struct imagesection) * IMAGE_MAX_SECTIONS);
	if (!section) {
		free(lpsz_line);
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}
	int retval;

	retval = image_ihex_buffer_complete_inner(image, lpsz_line, section);

	free(section);
	free(lpsz_line);

	return retval;
}

static int image_elf32_read_headers(struct image *image)
{
	struct image_elf *elf = image->type_private;
	size_t read_bytes;
	uint32_t i, j;
	int retval;
	uint32_t nload;
	bool load_to_vaddr = false;

	retval = fileio_seek(elf->fileio, 0);
	if (retval != ERROR_OK) {
		LOG_ERROR("cannot seek to ELF file header, read failed");
		return retval;
	}

	elf->header32 = malloc(sizeof(Elf32_Ehdr));

	if (!elf->header32) {
		LOG_ERROR("insufficient memory to perform operation");
		return ERROR_FILEIO_OPERATION_FAILED;
	}

	retval = fileio_read(elf->fileio, sizeof(Elf32_Ehdr), (uint8_t *)elf->header32, &read_bytes);
	if (retval != ERROR_OK) {
		LOG_ERROR("cannot read ELF file header, read failed");
		return ERROR_FILEIO_OPERATION_FAILED;
	}
	if (read_bytes != sizeof(Elf32_Ehdr)) {
		LOG_ERROR("cannot read ELF file header, only partially read");
		return ERROR_FILEIO_OPERATION_FAILED;
	}

	elf->segment_count = field16(elf, elf->header32->e_phnum);
	if (elf->segment_count == 0) {
		LOG_ERROR("invalid ELF file, no program headers");
		return ERROR_IMAGE_FORMAT_ERROR;
	}

	retval = fileio_seek(elf->fileio, field32(elf, elf->header32->e_phoff));
	if (retval != ERROR_OK) {
		LOG_ERROR("cannot seek to ELF program header table, read failed");
		return retval;
	}

	elf->segments32 = malloc(elf->segment_count*sizeof(Elf32_Phdr));
	if (!elf->segments32) {
		LOG_ERROR("insufficient memory to perform operation");
		return ERROR_FILEIO_OPERATION_FAILED;
	}

	retval = fileio_read(elf->fileio, elf->segment_count*sizeof(Elf32_Phdr),
			(uint8_t *)elf->segments32, &read_bytes);
	if (retval != ERROR_OK) {
		LOG_ERROR("cannot read ELF segment headers, read failed");
		return retval;
	}
	if (read_bytes != elf->segment_count*sizeof(Elf32_Phdr)) {
		LOG_ERROR("cannot read ELF segment headers, only partially read");
		return ERROR_FILEIO_OPERATION_FAILED;
	}

	/* count useful segments (loadable), ignore BSS section */
	image->num_sections = 0;
	for (i = 0; i < elf->segment_count; i++)
		if ((field32(elf,
			elf->segments32[i].p_type) == PT_LOAD) &&
			(field32(elf, elf->segments32[i].p_filesz) != 0))
			image->num_sections++;

	if (image->num_sections == 0) {
		LOG_ERROR("invalid ELF file, no loadable segments");
		return ERROR_IMAGE_FORMAT_ERROR;
	}

	/**
	 * some ELF linkers produce binaries with *all* the program header
	 * p_paddr fields zero (there can be however one loadable segment
	 * that has valid physical address 0x0).
	 * If we have such a binary with more than
	 * one PT_LOAD header, then use p_vaddr instead of p_paddr
	 * (ARM ELF standard demands p_paddr = 0 anyway, and BFD
	 * library uses this approach to workaround zero-initialized p_paddrs
	 * when obtaining lma - look at elf.c of BDF)
	 */
	for (nload = 0, i = 0; i < elf->segment_count; i++)
		if (elf->segments32[i].p_paddr != 0)
			break;
		else if ((field32(elf,
			elf->segments32[i].p_type) == PT_LOAD) &&
			(field32(elf, elf->segments32[i].p_memsz) != 0))
			++nload;

	if (i >= elf->segment_count && nload > 1)
		load_to_vaddr = true;

	/* alloc and fill sections array with loadable segments */
	image->sections = malloc(image->num_sections * sizeof(struct imagesection));
	if (!image->sections) {
		LOG_ERROR("insufficient memory to perform operation");
		return ERROR_FILEIO_OPERATION_FAILED;
	}

	for (i = 0, j = 0; i < elf->segment_count; i++) {
		if ((field32(elf,
			elf->segments32[i].p_type) == PT_LOAD) &&
			(field32(elf, elf->segments32[i].p_filesz) != 0)) {
			image->sections[j].size = field32(elf, elf->segments32[i].p_filesz);
			if (load_to_vaddr)
				image->sections[j].base_address = field32(elf,
						elf->segments32[i].p_vaddr);
			else
				image->sections[j].base_address = field32(elf,
						elf->segments32[i].p_paddr);
			image->sections[j].private = &elf->segments32[i];
			image->sections[j].flags = field32(elf, elf->segments32[i].p_flags);
			j++;
		}
	}

	image->start_address_set = true;
	image->start_address = field32(elf, elf->header32->e_entry);

	return ERROR_OK;
}

static int image_elf64_read_headers(struct image *image)
{
	struct image_elf *elf = image->type_private;
	size_t read_bytes;
	uint32_t i, j;
	int retval;
	uint32_t nload;
	bool load_to_vaddr = false;

	retval = fileio_seek(elf->fileio, 0);
	if (retval != ERROR_OK) {
		LOG_ERROR("cannot seek to ELF file header, read failed");
		return retval;
	}

	elf->header64 = malloc(sizeof(Elf64_Ehdr));

	if (!elf->header64) {
		LOG_ERROR("insufficient memory to perform operation");
		return ERROR_FILEIO_OPERATION_FAILED;
	}

	retval = fileio_read(elf->fileio, sizeof(Elf64_Ehdr), (uint8_t *)elf->header64, &read_bytes);
	if (retval != ERROR_OK) {
		LOG_ERROR("cannot read ELF file header, read failed");
		return ERROR_FILEIO_OPERATION_FAILED;
	}
	if (read_bytes != sizeof(Elf64_Ehdr)) {
		LOG_ERROR("cannot read ELF file header, only partially read");
		return ERROR_FILEIO_OPERATION_FAILED;
	}

	elf->segment_count = field16(elf, elf->header64->e_phnum);
	if (elf->segment_count == 0) {
		LOG_ERROR("invalid ELF file, no program headers");
		return ERROR_IMAGE_FORMAT_ERROR;
	}

	retval = fileio_seek(elf->fileio, field64(elf, elf->header64->e_phoff));
	if (retval != ERROR_OK) {
		LOG_ERROR("cannot seek to ELF program header table, read failed");
		return retval;
	}

	elf->segments64 = malloc(elf->segment_count*sizeof(Elf64_Phdr));
	if (!elf->segments64) {
		LOG_ERROR("insufficient memory to perform operation");
		return ERROR_FILEIO_OPERATION_FAILED;
	}

	retval = fileio_read(elf->fileio, elf->segment_count*sizeof(Elf64_Phdr),
			(uint8_t *)elf->segments64, &read_bytes);
	if (retval != ERROR_OK) {
		LOG_ERROR("cannot read ELF segment headers, read failed");
		return retval;
	}
	if (read_bytes != elf->segment_count*sizeof(Elf64_Phdr)) {
		LOG_ERROR("cannot read ELF segment headers, only partially read");
		return ERROR_FILEIO_OPERATION_FAILED;
	}

	/* count useful segments (loadable), ignore BSS section */
	image->num_sections = 0;
	for (i = 0; i < elf->segment_count; i++)
		if ((field32(elf,
			elf->segments64[i].p_type) == PT_LOAD) &&
			(field64(elf, elf->segments64[i].p_filesz) != 0))
			image->num_sections++;

	if (image->num_sections == 0) {
		LOG_ERROR("invalid ELF file, no loadable segments");
		return ERROR_IMAGE_FORMAT_ERROR;
	}

	/**
	 * some ELF linkers produce binaries with *all* the program header
	 * p_paddr fields zero (there can be however one loadable segment
	 * that has valid physical address 0x0).
	 * If we have such a binary with more than
	 * one PT_LOAD header, then use p_vaddr instead of p_paddr
	 * (ARM ELF standard demands p_paddr = 0 anyway, and BFD
	 * library uses this approach to workaround zero-initialized p_paddrs
	 * when obtaining lma - look at elf.c of BDF)
	 */
	for (nload = 0, i = 0; i < elf->segment_count; i++)
		if (elf->segments64[i].p_paddr != 0)
			break;
		else if ((field32(elf,
			elf->segments64[i].p_type) == PT_LOAD) &&
			(field64(elf, elf->segments64[i].p_memsz) != 0))
			++nload;

	if (i >= elf->segment_count && nload > 1)
		load_to_vaddr = true;

	/* alloc and fill sections array with loadable segments */
	image->sections = malloc(image->num_sections * sizeof(struct imagesection));
	if (!image->sections) {
		LOG_ERROR("insufficient memory to perform operation");
		return ERROR_FILEIO_OPERATION_FAILED;
	}

	for (i = 0, j = 0; i < elf->segment_count; i++) {
		if ((field32(elf,
			elf->segments64[i].p_type) == PT_LOAD) &&
			(field64(elf, elf->segments64[i].p_filesz) != 0)) {
			image->sections[j].size = field64(elf, elf->segments64[i].p_filesz);
			if (load_to_vaddr)
				image->sections[j].base_address = field64(elf,
						elf->segments64[i].p_vaddr);
			else
				image->sections[j].base_address = field64(elf,
						elf->segments64[i].p_paddr);
			image->sections[j].private = &elf->segments64[i];
			image->sections[j].flags = field64(elf, elf->segments64[i].p_flags);
			j++;
		}
	}

	image->start_address_set = true;
	image->start_address = field64(elf, elf->header64->e_entry);

	return ERROR_OK;
}

static int image_elf_read_headers(struct image *image)
{
	struct image_elf *elf = image->type_private;
	size_t read_bytes;
	unsigned char e_ident[EI_NIDENT];
	int retval;

	retval = fileio_read(elf->fileio, EI_NIDENT, e_ident, &read_bytes);
	if (retval != ERROR_OK) {
		LOG_ERROR("cannot read ELF file header, read failed");
		return ERROR_FILEIO_OPERATION_FAILED;
	}
	if (read_bytes != EI_NIDENT) {
		LOG_ERROR("cannot read ELF file header, only partially read");
		return ERROR_FILEIO_OPERATION_FAILED;
	}

	if (strncmp((char *)e_ident, ELFMAG, SELFMAG) != 0) {
		LOG_ERROR("invalid ELF file, bad magic number");
		return ERROR_IMAGE_FORMAT_ERROR;
	}

	elf->endianness = e_ident[EI_DATA];
	if ((elf->endianness != ELFDATA2LSB)
		&& (elf->endianness != ELFDATA2MSB)) {
		LOG_ERROR("invalid ELF file, unknown endianness setting");
		return ERROR_IMAGE_FORMAT_ERROR;
	}

	switch (e_ident[EI_CLASS]) {
	case ELFCLASS32:
		LOG_DEBUG("ELF32 image detected.");
		elf->is_64_bit = false;
		return image_elf32_read_headers(image);

	case ELFCLASS64:
		LOG_DEBUG("ELF64 image detected.");
		elf->is_64_bit = true;
		return image_elf64_read_headers(image);

	default:
		LOG_ERROR("invalid ELF file, only 32/64 bit ELF files are supported");
		return ERROR_IMAGE_FORMAT_ERROR;
	}
}

static int image_elf32_read_section(struct image *image,
	int section,
	target_addr_t offset,
	uint32_t size,
	uint8_t *buffer,
	size_t *size_read)
{
	struct image_elf *elf = image->type_private;
	Elf32_Phdr *segment = (Elf32_Phdr *)image->sections[section].private;
	size_t read_size, really_read;
	int retval;

	*size_read = 0;

	LOG_DEBUG("load segment %d at 0x%" TARGET_PRIxADDR " (sz = 0x%" PRIx32 ")", section, offset, size);

	/* read initialized data in current segment if any */
	if (offset < field32(elf, segment->p_filesz)) {
		/* maximal size present in file for the current segment */
		read_size = MIN(size, field32(elf, segment->p_filesz) - offset);
		LOG_DEBUG("read elf: size = 0x%zx at 0x%" TARGET_PRIxADDR "", read_size,
			field32(elf, segment->p_offset) + offset);
		/* read initialized area of the segment */
		retval = fileio_seek(elf->fileio, field32(elf, segment->p_offset) + offset);
		if (retval != ERROR_OK) {
			LOG_ERROR("cannot find ELF segment content, seek failed");
			return retval;
		}
		retval = fileio_read(elf->fileio, read_size, buffer, &really_read);
		if (retval != ERROR_OK) {
			LOG_ERROR("cannot read ELF segment content, read failed");
			return retval;
		}
		size -= read_size;
		*size_read += read_size;
		/* need more data ? */
		if (!size)
			return ERROR_OK;
	}

	return ERROR_OK;
}

static int image_elf64_read_section(struct image *image,
	int section,
	target_addr_t offset,
	uint32_t size,
	uint8_t *buffer,
	size_t *size_read)
{
	struct image_elf *elf = image->type_private;
	Elf64_Phdr *segment = (Elf64_Phdr *)image->sections[section].private;
	size_t read_size, really_read;
	int retval;

	*size_read = 0;

	LOG_DEBUG("load segment %d at 0x%" TARGET_PRIxADDR " (sz = 0x%" PRIx32 ")", section, offset, size);

	/* read initialized data in current segment if any */
	if (offset < field64(elf, segment->p_filesz)) {
		/* maximal size present in file for the current segment */
		read_size = MIN(size, field64(elf, segment->p_filesz) - offset);
		LOG_DEBUG("read elf: size = 0x%zx at 0x%" TARGET_PRIxADDR "", read_size,
			field64(elf, segment->p_offset) + offset);
		/* read initialized area of the segment */
		retval = fileio_seek(elf->fileio, field64(elf, segment->p_offset) + offset);
		if (retval != ERROR_OK) {
			LOG_ERROR("cannot find ELF segment content, seek failed");
			return retval;
		}
		retval = fileio_read(elf->fileio, read_size, buffer, &really_read);
		if (retval != ERROR_OK) {
			LOG_ERROR("cannot read ELF segment content, read failed");
			return retval;
		}
		size -= read_size;
		*size_read += read_size;
		/* need more data ? */
		if (!size)
			return ERROR_OK;
	}

	return ERROR_OK;
}

static int image_elf_read_section(struct image *image,
	int section,
	target_addr_t offset,
	uint32_t size,
	uint8_t *buffer,
	size_t *size_read)
{
	struct image_elf *elf = image->type_private;

	if (elf->is_64_bit)
		return image_elf64_read_section(image, section, offset, size, buffer, size_read);
	else
		return image_elf32_read_section(image, section, offset, size, buffer, size_read);
}

static int image_mot_buffer_complete_inner(struct image *image,
	char *lpsz_line,
	struct imagesection *section)
{
	struct image_mot *mot = image->type_private;
	struct fileio *fileio = mot->fileio;
	uint32_t full_address;
	uint32_t cooked_bytes;
	bool end_rec = false;

	/* we can't determine the number of sections that we'll have to create ahead of time,
	 * so we locally hold them until parsing is finished */

	int retval;
	size_t filesize;
	retval = fileio_size(fileio, &filesize);
	if (retval != ERROR_OK)
		return retval;

	mot->buffer = malloc(filesize >> 1);
	cooked_bytes = 0x0;
	image->num_sections = 0;

	while (!fileio_feof(fileio)) {
		full_address = 0x0;
		section[image->num_sections].private = &mot->buffer[cooked_bytes];
		section[image->num_sections].base_address = 0x0;
		section[image->num_sections].size = 0x0;
		section[image->num_sections].flags = 0;

		while (fileio_fgets(fileio, 1023, lpsz_line) == ERROR_OK) {
			uint32_t count;
			uint32_t address;
			uint32_t record_type;
			uint32_t checksum;
			uint8_t cal_checksum = 0;
			uint32_t bytes_read = 0;

			/* skip comments and blank lines */
			if ((lpsz_line[0] == '#') || (strlen(lpsz_line + strspn(lpsz_line, "\n\t\r ")) == 0))
				continue;

			/* get record type and record length */
			if (sscanf(&lpsz_line[bytes_read], "S%1" SCNx32 "%2" SCNx32, &record_type,
				&count) != 2)
				return ERROR_IMAGE_FORMAT_ERROR;

			bytes_read += 4;
			cal_checksum += (uint8_t)count;

			/* skip checksum byte */
			count -= 1;

			if (record_type == 0) {
				/* S0 - starting record (optional) */
				int value;

				while (count-- > 0) {
					sscanf(&lpsz_line[bytes_read], "%2x", &value);
					cal_checksum += (uint8_t)value;
					bytes_read += 2;
				}
			} else if (record_type >= 1 && record_type <= 3) {
				switch (record_type) {
					case 1:
						/* S1 - 16 bit address data record */
						sscanf(&lpsz_line[bytes_read], "%4" SCNx32, &address);
						cal_checksum += (uint8_t)(address >> 8);
						cal_checksum += (uint8_t)address;
						bytes_read += 4;
						count -= 2;
						break;

					case 2:
						/* S2 - 24 bit address data record */
						sscanf(&lpsz_line[bytes_read], "%6" SCNx32, &address);
						cal_checksum += (uint8_t)(address >> 16);
						cal_checksum += (uint8_t)(address >> 8);
						cal_checksum += (uint8_t)address;
						bytes_read += 6;
						count -= 3;
						break;

					case 3:
						/* S3 - 32 bit address data record */
						sscanf(&lpsz_line[bytes_read], "%8" SCNx32, &address);
						cal_checksum += (uint8_t)(address >> 24);
						cal_checksum += (uint8_t)(address >> 16);
						cal_checksum += (uint8_t)(address >> 8);
						cal_checksum += (uint8_t)address;
						bytes_read += 8;
						count -= 4;
						break;

				}

				if (full_address != address) {
					/* we encountered a nonconsecutive location, create a new section,
					 * unless the current section has zero size, in which case this specifies
					 * the current section's base address
					 */
					if (section[image->num_sections].size != 0) {
						image->num_sections++;
						section[image->num_sections].size = 0x0;
						section[image->num_sections].flags = 0;
						section[image->num_sections].private =
							&mot->buffer[cooked_bytes];
					}
					section[image->num_sections].base_address = address;
					full_address = address;
				}

				while (count-- > 0) {
					unsigned int value;
					sscanf(&lpsz_line[bytes_read], "%2x", &value);
					mot->buffer[cooked_bytes] = (uint8_t)value;
					cal_checksum += (uint8_t)mot->buffer[cooked_bytes];
					bytes_read += 2;
					cooked_bytes += 1;
					section[image->num_sections].size += 1;
					full_address++;
				}
			} else if (record_type == 5 || record_type == 6) {
				/* S5 and S6 are the data count records, we ignore them */
				uint32_t dummy;

				while (count-- > 0) {
					sscanf(&lpsz_line[bytes_read], "%2" SCNx32, &dummy);
					cal_checksum += (uint8_t)dummy;
					bytes_read += 2;
				}
			} else if (record_type >= 7 && record_type <= 9) {
				/* S7, S8, S9 - ending records for 32, 24 and 16bit */
				image->num_sections++;

				/* copy section information */
				image->sections = malloc(sizeof(struct imagesection) * image->num_sections);
				for (unsigned int i = 0; i < image->num_sections; i++) {
					image->sections[i].private = section[i].private;
					image->sections[i].base_address = section[i].base_address;
					image->sections[i].size = section[i].size;
					image->sections[i].flags = section[i].flags;
				}

				end_rec = true;
				break;
			} else {
				LOG_ERROR("unhandled S19 record type: %i", (int)(record_type));
				return ERROR_IMAGE_FORMAT_ERROR;
			}

			/* account for checksum, will always be 0xFF */
			sscanf(&lpsz_line[bytes_read], "%2" SCNx32, &checksum);
			cal_checksum += (uint8_t)checksum;

			if (cal_checksum != 0xFF) {
				/* checksum failed */
				LOG_ERROR("incorrect record checksum found in S19 file");
				return ERROR_IMAGE_CHECKSUM;
			}

			if (end_rec) {
				end_rec = false;
				LOG_WARNING("continuing after end-of-file record: %.40s", lpsz_line);
			}
		}
	}

	if (end_rec)
		return ERROR_OK;
	else {
		LOG_ERROR("premature end of S19 file, no matching end-of-file record found");
		return ERROR_IMAGE_FORMAT_ERROR;
	}
}

/**
 * Allocate memory dynamically instead of on the stack. This
 * is important w/embedded hosts.
 */
static int image_mot_buffer_complete(struct image *image)
{
	char *lpsz_line = malloc(1023);
	if (!lpsz_line) {
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}
	struct imagesection *section = malloc(sizeof(struct imagesection) * IMAGE_MAX_SECTIONS);
	if (!section) {
		free(lpsz_line);
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}
	int retval;

	retval = image_mot_buffer_complete_inner(image, lpsz_line, section);

	free(section);
	free(lpsz_line);

	return retval;
}

int image_open(struct image *image, const char *url, const char *type_string)
{
	int retval = ERROR_OK;

	retval = identify_image_type(image, type_string, url);
	if (retval != ERROR_OK)
		return retval;

	if (image->type == IMAGE_BINARY) {
		struct image_binary *image_binary;

		image_binary = image->type_private = malloc(sizeof(struct image_binary));

		retval = fileio_open(&image_binary->fileio, url, FILEIO_READ, FILEIO_BINARY);
		if (retval != ERROR_OK)
			goto free_mem_on_error;

		size_t filesize;
		retval = fileio_size(image_binary->fileio, &filesize);
		if (retval != ERROR_OK) {
			fileio_close(image_binary->fileio);
			goto free_mem_on_error;
		}

		image->num_sections = 1;
		image->sections = malloc(sizeof(struct imagesection));
		image->sections[0].base_address = 0x0;
		image->sections[0].size = filesize;
		image->sections[0].flags = 0;
	} else if (image->type == IMAGE_IHEX) {
		struct image_ihex *image_ihex;

		image_ihex = image->type_private = malloc(sizeof(struct image_ihex));

		retval = fileio_open(&image_ihex->fileio, url, FILEIO_READ, FILEIO_TEXT);
		if (retval != ERROR_OK)
			goto free_mem_on_error;

		retval = image_ihex_buffer_complete(image);
		if (retval != ERROR_OK) {
			LOG_ERROR(
				"failed buffering IHEX image, check server output for additional information");
			fileio_close(image_ihex->fileio);
			goto free_mem_on_error;
		}
	} else if (image->type == IMAGE_ELF) {
		struct image_elf *image_elf;

		image_elf = image->type_private = malloc(sizeof(struct image_elf));

		retval = fileio_open(&image_elf->fileio, url, FILEIO_READ, FILEIO_BINARY);
		if (retval != ERROR_OK)
			goto free_mem_on_error;

		retval = image_elf_read_headers(image);
		if (retval != ERROR_OK) {
			fileio_close(image_elf->fileio);
			goto free_mem_on_error;
		}
	} else if (image->type == IMAGE_MEMORY) {
		struct target *target = get_target(url);

		if (!target) {
			LOG_ERROR("target '%s' not defined", url);
			return ERROR_FAIL;
		}

		struct image_memory *image_memory;

		image->num_sections = 1;
		image->sections = malloc(sizeof(struct imagesection));
		image->sections[0].base_address = 0x0;
		image->sections[0].size = 0xffffffff;
		image->sections[0].flags = 0;

		image_memory = image->type_private = malloc(sizeof(struct image_memory));

		image_memory->target = target;
		image_memory->cache = NULL;
		image_memory->cache_address = 0x0;
	} else if (image->type == IMAGE_SRECORD) {
		struct image_mot *image_mot;

		image_mot = image->type_private = malloc(sizeof(struct image_mot));

		retval = fileio_open(&image_mot->fileio, url, FILEIO_READ, FILEIO_TEXT);
		if (retval != ERROR_OK)
			goto free_mem_on_error;

		retval = image_mot_buffer_complete(image);
		if (retval != ERROR_OK) {
			LOG_ERROR(
				"failed buffering S19 image, check server output for additional information");
			fileio_close(image_mot->fileio);
			goto free_mem_on_error;
		}
	} else if (image->type == IMAGE_BUILDER) {
		image->num_sections = 0;
		image->base_address_set = false;
		image->sections = NULL;
		image->type_private = NULL;
	}

	if (image->base_address_set) {
		/* relocate */
		for (unsigned int section = 0; section < image->num_sections; section++)
			image->sections[section].base_address += image->base_address;
											/* we're done relocating. The two statements below are mainly
											* for documentation purposes: stop anyone from empirically
											* thinking they should use these values henceforth. */
		image->base_address = 0;
		image->base_address_set = false;
	}

	return retval;

free_mem_on_error:
	free(image->type_private);
	image->type_private = NULL;
	return retval;
};

int image_read_section(struct image *image,
	int section,
	target_addr_t offset,
	uint32_t size,
	uint8_t *buffer,
	size_t *size_read)
{
	int retval;

	/* don't read past the end of a section */
	if (offset + size > image->sections[section].size) {
		LOG_DEBUG(
			"read past end of section: 0x%8.8" TARGET_PRIxADDR " + 0x%8.8" PRIx32 " > 0x%8.8" PRIx32 "",
			offset,
			size,
			image->sections[section].size);
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if (image->type == IMAGE_BINARY) {
		struct image_binary *image_binary = image->type_private;

		/* only one section in a plain binary */
		if (section != 0)
			return ERROR_COMMAND_SYNTAX_ERROR;

		/* seek to offset */
		retval = fileio_seek(image_binary->fileio, offset);
		if (retval != ERROR_OK)
			return retval;

		/* return requested bytes */
		retval = fileio_read(image_binary->fileio, size, buffer, size_read);
		if (retval != ERROR_OK)
			return retval;
	} else if (image->type == IMAGE_IHEX) {
		memcpy(buffer, (uint8_t *)image->sections[section].private + offset, size);
		*size_read = size;

		return ERROR_OK;
	} else if (image->type == IMAGE_ELF) {
		return image_elf_read_section(image, section, offset, size, buffer, size_read);
	} else if (image->type == IMAGE_MEMORY) {
		struct image_memory *image_memory = image->type_private;
		uint32_t address = image->sections[section].base_address + offset;

		*size_read = 0;

		while ((size - *size_read) > 0) {
			uint32_t size_in_cache;

			if (!image_memory->cache
				|| (address < image_memory->cache_address)
				|| (address >=
				(image_memory->cache_address + IMAGE_MEMORY_CACHE_SIZE))) {
				if (!image_memory->cache)
					image_memory->cache = malloc(IMAGE_MEMORY_CACHE_SIZE);

				if (target_read_buffer(image_memory->target, address &
					~(IMAGE_MEMORY_CACHE_SIZE - 1),
					IMAGE_MEMORY_CACHE_SIZE, image_memory->cache) != ERROR_OK) {
					free(image_memory->cache);
					image_memory->cache = NULL;
					return ERROR_IMAGE_TEMPORARILY_UNAVAILABLE;
				}
				image_memory->cache_address = address &
					~(IMAGE_MEMORY_CACHE_SIZE - 1);
			}

			size_in_cache =
				(image_memory->cache_address + IMAGE_MEMORY_CACHE_SIZE) - address;

			memcpy(buffer + *size_read,
				image_memory->cache + (address - image_memory->cache_address),
				(size_in_cache > size) ? size : size_in_cache
				);

			*size_read += (size_in_cache > size) ? size : size_in_cache;
			address += (size_in_cache > size) ? size : size_in_cache;
		}
	} else if (image->type == IMAGE_SRECORD) {
		memcpy(buffer, (uint8_t *)image->sections[section].private + offset, size);
		*size_read = size;

		return ERROR_OK;
	} else if (image->type == IMAGE_BUILDER) {
		memcpy(buffer, (uint8_t *)image->sections[section].private + offset, size);
		*size_read = size;

		return ERROR_OK;
	}

	return ERROR_OK;
}

int image_add_section(struct image *image, target_addr_t base, uint32_t size, uint64_t flags, uint8_t const *data)
{
	struct imagesection *section;

	/* only image builder supports adding sections */
	if (image->type != IMAGE_BUILDER)
		return ERROR_COMMAND_SYNTAX_ERROR;

	/* see if there's a previous section */
	if (image->num_sections) {
		section = &image->sections[image->num_sections - 1];

		/* see if it's enough to extend the last section,
		 * adding data to previous sections or merging is not supported */
		if (((section->base_address + section->size) == base) &&
			(section->flags == flags)) {
			section->private = realloc(section->private, section->size + size);
			memcpy((uint8_t *)section->private + section->size, data, size);
			section->size += size;
			return ERROR_OK;
		}
	}

	/* allocate new section */
	image->num_sections++;
	image->sections =
		realloc(image->sections, sizeof(struct imagesection) * image->num_sections);
	section = &image->sections[image->num_sections - 1];
	section->base_address = base;
	section->size = size;
	section->flags = flags;
	section->private = malloc(sizeof(uint8_t) * size);
	memcpy((uint8_t *)section->private, data, size);

	return ERROR_OK;
}

void image_close(struct image *image)
{
	if (image->type == IMAGE_BINARY) {
		struct image_binary *image_binary = image->type_private;

		fileio_close(image_binary->fileio);
	} else if (image->type == IMAGE_IHEX) {
		struct image_ihex *image_ihex = image->type_private;

		fileio_close(image_ihex->fileio);

		free(image_ihex->buffer);
		image_ihex->buffer = NULL;
	} else if (image->type == IMAGE_ELF) {
		struct image_elf *image_elf = image->type_private;

		fileio_close(image_elf->fileio);

		if (image_elf->is_64_bit) {
			free(image_elf->header64);
			image_elf->header64 = NULL;

			free(image_elf->segments64);
			image_elf->segments64 = NULL;
		} else {
			free(image_elf->header32);
			image_elf->header32 = NULL;

			free(image_elf->segments32);
			image_elf->segments32 = NULL;
		}
	} else if (image->type == IMAGE_MEMORY) {
		struct image_memory *image_memory = image->type_private;

		free(image_memory->cache);
		image_memory->cache = NULL;
	} else if (image->type == IMAGE_SRECORD) {
		struct image_mot *image_mot = image->type_private;

		fileio_close(image_mot->fileio);

		free(image_mot->buffer);
		image_mot->buffer = NULL;
	} else if (image->type == IMAGE_BUILDER) {
		for (unsigned int i = 0; i < image->num_sections; i++) {
			free(image->sections[i].private);
			image->sections[i].private = NULL;
		}
	}

	free(image->type_private);
	image->type_private = NULL;

	free(image->sections);
	image->sections = NULL;
}

int image_calculate_checksum(const uint8_t *buffer, uint32_t nbytes, uint32_t *checksum)
{
	uint32_t crc = 0xffffffff;
	LOG_DEBUG("Calculating checksum");

	static uint32_t crc32_table[256];

	static bool first_init;
	if (!first_init) {
		/* Initialize the CRC table and the decoding table.  */
		unsigned int i, j, c;
		for (i = 0; i < 256; i++) {
			/* as per gdb */
			for (c = i << 24, j = 8; j > 0; --j)
				c = c & 0x80000000 ? (c << 1) ^ 0x04c11db7 : (c << 1);
			crc32_table[i] = c;
		}

		first_init = true;
	}

	while (nbytes > 0) {
		int run = nbytes;
		if (run > 32768)
			run = 32768;
		nbytes -= run;
		while (run--) {
			/* as per gdb */
			crc = (crc << 8) ^ crc32_table[((crc >> 24) ^ *buffer++) & 255];
		}
		keep_alive();
		if (openocd_is_shutdown_pending())
			return ERROR_SERVER_INTERRUPTED;
	}

	LOG_DEBUG("Calculating checksum done; checksum=0x%" PRIx32, crc);

	*checksum = crc;
	return ERROR_OK;
}

/* Begin codes from Cypress fork */
/**
 * @brief Convenience function, perfroms seek+read operation with corresponding error checking
 * @param io pointer to fileio structure
 * @param offset starting offset of the data
 * @param size size of the data
 * @param buffer pointer to output buffer
 * @return ERROR_OK in case of success, ERROR_XXX code otherwise
 */
static int seek_read(struct fileio *io, size_t offset, size_t size, void *buffer)
{
	int hr;
	size_t read;

	hr = fileio_seek(io, offset);
	if (hr != ERROR_OK)
		return hr;

	hr = fileio_read(io, size, buffer, &read);
	if (hr != ERROR_OK)
		return hr;

	if (read != size)
		return ERROR_FILEIO_OPERATION_FAILED;

	return ERROR_OK;
}

/**
 * @brief Resolves section names as symbols
 * @param elf structure representing elf image
 * @param sect_hdrs pointer to an array of section headers
 * @param symbols array of structures to be populated with resolved addresses
 * @return ERROR_OK in case of success, ERROR_XXX code otherwise
 */
static int resolve_section_names(struct image_elf *elf, Elf32_Shdr *sect_hdrs,
	struct symbol *symbols)
{
	uint32_t sh_offs, sh_size;
	int hr;

  if (elf->is_64_bit) {
		LOG_ERROR("Symbol resolution is supported for ELF32 images only");
		return ERROR_IMAGE_FORMAT_ERROR;  
  }

	/* Locate section header representing string table with section names */
	const size_t str_table_idx = field16(elf, elf->header32->e_shstrndx);
	Elf32_Shdr *str_table_hdr = &sect_hdrs[str_table_idx];

	/* Load string table with section names */
	sh_offs = field32(elf, str_table_hdr->sh_offset);
	sh_size = field32(elf, str_table_hdr->sh_size);
	char str_tbl[sh_size];

	hr = seek_read(elf->fileio, sh_offs, sh_size, str_tbl);
	if (hr != ERROR_OK)
		return hr;

	/* Resolve section names as symbols */
	const size_t section_header_num = field16(elf, elf->header32->e_shnum);
	for (size_t i = 0; i < section_header_num; i++) {
		struct symbol *crnt_symbol = symbols;
		while (crnt_symbol->name) {
			const uint32_t sh_name = field32(elf, sect_hdrs[i].sh_name);
			if (!strcmp(str_tbl + sh_name, crnt_symbol->name))
				crnt_symbol->offset = field32(elf, sect_hdrs[i].sh_addr);

			crnt_symbol++;
		}
	}

	return ERROR_OK;
}

/**
 * @brief Parses ELF headers and performs symbol resolution. Function is capable
 * of resolwing section names as symbols. This is required by CMSIS Flash Algorithms.
 * @param image structure representing elf image
 * @param symbols array of structures to be populated with resolved addresses
 * @return ERROR_OK in case of success, ERROR_XXX code otherwise
 */
int image_resolve_symbols(struct image *image, struct symbol *symbols)
{
	if (image->type != IMAGE_ELF) {
		LOG_ERROR("Symbol resolution is supported for ELF images only");
		return ERROR_IMAGE_FORMAT_ERROR;
	}

	struct image_elf *elf = image->type_private;
	uint32_t sh_offs, sh_size;
	int hr;

  if (elf->is_64_bit) {
		LOG_ERROR("Symbol resolution is supported for ELF32 images only");
		return ERROR_IMAGE_FORMAT_ERROR;  
  }

	/* Read all section headers */
	sh_offs = field32(elf, elf->header32->e_shoff);
	sh_size = field16(elf, elf->header32->e_shnum) * sizeof(Elf32_Shdr);
	Elf32_Shdr section_hdrs[sh_size];

	hr = seek_read(elf->fileio, sh_offs, sh_size, section_hdrs);
	if (hr != ERROR_OK)
		return hr;

	/* Symbols may include section names, resolve load addresses of all sections */
	hr = resolve_section_names(elf, section_hdrs, symbols);
	if (hr != ERROR_OK)
		return hr;

	size_t symbol_count = 0;
	size_t string_tbl_idx = 0;
	Elf32_Sym *sym_table = NULL;

	/* Loop through all section headers */
	const size_t section_header_num = field16(elf, elf->header32->e_shnum);
	for (size_t i = 0; i < section_header_num; i++) {

		/* If current section header represents symbol table */
		if (section_hdrs[i].sh_type == SHT_SYMTAB) {

			/* Read symbol table */
			sh_offs = field32(elf, section_hdrs[i].sh_offset);
			sh_size = field32(elf, section_hdrs[i].sh_size);

			sym_table = malloc(sh_size);
			if (!sym_table)
				return ERROR_FAIL;

			hr = seek_read(elf->fileio, sh_offs, sh_size, sym_table);
			if (hr != ERROR_OK)
				goto free_sym_table;

			/* sh_link contains index of corresponding String Table header */
			string_tbl_idx = field32(elf, section_hdrs[i].sh_link);
			symbol_count = sh_size / sizeof(Elf32_Sym);
			break;
		}
	}

	if (!sym_table) {
		LOG_ERROR("Symbol Table not found in elf object, symbols stripped???");
		return ERROR_IMAGE_FORMAT_ERROR;
	}

	/* Load string table header with symbol names */
	sh_offs = field32(elf, section_hdrs[string_tbl_idx].sh_offset);
	sh_size = field32(elf, section_hdrs[string_tbl_idx].sh_size);

	char *strtab = malloc(section_hdrs[string_tbl_idx].sh_size);
	if (!strtab) {
		hr = ERROR_FAIL;
		goto free_sym_table;
	}

	hr = seek_read(elf->fileio, sh_offs, sh_size, strtab);
	if (hr != ERROR_OK)
		goto free_str_table;

	/* Resolve symbols */
	for (size_t j = 0; j < symbol_count; j++) {
		struct symbol *crnt_symbol = symbols;

		while (crnt_symbol->name) {
			uint32_t st_name = field32(elf, sym_table[j].st_name);
			if(sym_table[j].st_shndx != 0 /* STN_UNDEF */)
				if (!strcmp(&strtab[st_name], crnt_symbol->name))
					crnt_symbol->offset = sym_table[j].st_value;

			crnt_symbol++;
		}
	}

free_str_table:
	free(strtab);
free_sym_table:
	free(sym_table);
	return hr;
}
/* End codes from Cypress fork */