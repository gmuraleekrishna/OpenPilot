/**
 ******************************************************************************
 * @file       pios_flashfs.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2015.
 * @addtogroup PIOS PIOS Core hardware abstraction layer
 * @see        The GNU Public License (GPL) Version 3
 * @{
 * @addtogroup PIOS_FLASHFS Flash Filesystem API Definition
 * @{
 * @brief PIOS API for internal or onboard filesystem using SPIFFS.
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */


#include "pios.h"


#ifdef PIOS_INCLUDE_FLASH_SPIFFS

#include <stdbool.h>
#include <openpilot.h>
#include <pios_math.h>
#include <pios_wdg.h>
#include "pios_flashfs_logfs_priv.h"
#include "spiffs_config.h"

#ifdef PIOS_ENABLE_DEBUG_PINS
#include "pios_debug.h"
#endif

#define SPIFFS_FDS_SIZE (32*4)

#if defined(PIOS_INCLUDE_FREERTOS)
xSemaphoreHandle flashfs_mutex = 0;
#endif

#ifdef PIOS_ENABLE_DEBUG_PINS
#define PINDEBUG_SPIFFS_INIT 0
#define PINDEBUG_SPIFFS_READ 1
#define PINDEBUG_SPIFFS_WRITE 2
#define PINDEBUG_SPIFFS_FORMAT 3
#define PINDEBUG_SPIFFS_DELETE 4
#define PINDEBUG_SPIFFS_STATS 5
#else
#define PIOS_DEBUG_PinHigh(pin)
#define PIOS_DEBUG_PinLow(pin)
#endif

#define PIOS_FLASHFS_OPID_CONTENT "www.openpilot.org\n"
#define PIOS_FLASHFS_OPID_SIZE_MAX 256
#define  PIOS_FLASHFS_OPID_FILENAME "OPID.txt"

enum pios_flashfs_dev_magic {
    PIOS_FLASHFS_DEV_MAGIC = 0x94938201,
};


static uint32_t pios_flashfs2spiffs_lseek_flags[] = { SPIFFS_SEEK_SET, SPIFFS_SEEK_CUR, SPIFFS_SEEK_END };

struct flashfs_state {
    enum pios_flashfs_dev_magic magic;
    bool mounted;
    u8_t *work_buf;
    u8_t *cache_buf;
    u8_t *copy_buf;
    spiffs_config cfg_spiffs;
    u8_t spiffs_fds[SPIFFS_FDS_SIZE];
    spiffs fs;
    /* Underlying flash driver glue */
    const struct pios_flash_driver *driver;
    const struct flashfs_cfg *cfg;
    uint16_t files;
    uintptr_t flash_id;
};


/**
 * @brief Validate flashfs struct.
 */
static bool PIOS_FLASHFS_Validate(const struct flashfs_state *flashfs)
{
    return flashfs && (flashfs->magic == PIOS_FLASHFS_DEV_MAGIC);
}


/**
 * @brief Allocate flashfs struct.
 */
#if defined(PIOS_INCLUDE_FREERTOS)
static struct flashfs_state *PIOS_FLASHFS_alloc(void)
{
    struct flashfs_state *flashfs;

    flashfs = (struct flashfs_state *)pios_malloc(sizeof(*flashfs));
    if (!flashfs) {
        return NULL;
    }

    flashfs->magic = PIOS_FLASHFS_DEV_MAGIC;
    return flashfs;
}
static void PIOS_FLASHFS_free(struct flashfs_state *flashfs)
{
    /* Invalidate the magic */
	flashfs->magic = ~PIOS_FLASHFS_DEV_MAGIC;
    vPortFree(flashfs);
}
#else
static struct flashfs_state pios_flashfs_devs[PIOS_FLASHFS_MAX_DEVS];
static uint8_t pios_flashfs_num_devs;
static struct flashfs_state *PIOS_FLASHFS_alloc(void)
{
    struct flashfs_state *flashfs;

    if (pios_flashfs_num_devs >= PIOS_FLASHFS_MAX_DEVS) {
        return NULL;
    }

    flashfs = &pios_flashfs_devs[pios_flashfs_num_devs++];
    flashfs->magic = PIOS_FLASHFS_DEV_MAGIC;

    return flashfs;
}
static void PIOS_FLASHFS_free(struct flashfs_state *flashfs)
{
    /* Invalidate the magic */
	flashfs->magic = ~PIOS_FLASHFS_DEV_MAGIC;
}
#endif /* if defined(PIOS_INCLUDE_FREERTOS) */


/**
 * @brief HAL page read
 */
static s32_t PIOS_FLASHFS_spiffs_read(void *fs, u32_t addr, u32_t size, u8_t *dst)
{
	struct flashfs_state *flashfs = (struct flashfs_state*)((spiffs*)fs)->cfg.fs_id;
	flashfs->driver->read_data(flashfs->flash_id, addr, dst, (u16_t)size);
	return SPIFFS_OK;
}


/**
 * @brief HAL page write
 */
static s32_t PIOS_FLASHFS_spiffs_write(void *fs, u32_t addr, u32_t size, u8_t *src)
{
	struct flashfs_state *flashfs = (struct flashfs_state*)((spiffs*)fs)->cfg.fs_id;
	flashfs->driver->write_data(flashfs->flash_id, addr, src, (u16_t)size);
	return SPIFFS_OK;
}


/**
 * @brief HAL sector erase
 */
static s32_t PIOS_FLASHFS_spiffs_erase(void *fs, u32_t addr, __attribute__((unused)) u32_t size)
{
    struct flashfs_state *flashfs = (struct flashfs_state*)((spiffs*)fs)->cfg.fs_id;
    flashfs->driver->erase_sector(flashfs->flash_id, addr);
	return SPIFFS_OK;
}


/**
 * @brief Mount the file system
 */
static int32_t PIOS_FLASHFS_AddIDFile(__attribute__((unused)) uintptr_t fs_id)
{
    int32_t rc = PIOS_FLASHFS_OK;
    int16_t fh;
    char buf[PIOS_FLASHFS_OPID_SIZE_MAX];

    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

    /* Add id file with some info for sanity check. */
    fh = SPIFFS_open(&flashfs->fs, PIOS_FLASHFS_OPID_FILENAME, SPIFFS_CREAT | SPIFFS_WRONLY | SPIFFS_DIRECT, 0);
    if (fh < 0)
    {
        rc = PIOS_FLASHFS_ERROR_OPEN_FILE;
    }
    else
    {
        memset(buf, 0, sizeof(buf));
        snprintf(buf, sizeof(buf) - 1, PIOS_FLASHFS_OPID_CONTENT"%s %s\n", __DATE__, __TIME__);
        SPIFFS_write(&flashfs->fs, fh, (void *)buf, sizeof(buf));
        SPIFFS_close(&flashfs->fs, fh);
    }

    return rc;
}


/**
 * @brief Mount the file system
 */
static int32_t PIOS_FLASHFS_Mount(__attribute__((unused)) uintptr_t fs_id)
{
    int8_t rc = PIOS_FLASHFS_OK;

    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

    if (!PIOS_FLASHFS_Validate(flashfs))
        return PIOS_FLASHFS_ERROR_FS_INVALID;

    memset(flashfs->cache_buf, 0, flashfs->cfg->cache_buffer_size);

    if (SPIFFS_mount(&flashfs->fs,
                        &flashfs->cfg_spiffs,
                        flashfs->work_buf,
                        flashfs->spiffs_fds,
                        SPIFFS_FDS_SIZE,
                        flashfs->cache_buf,
                        flashfs->cfg->cache_buffer_size,
                        NULL) != SPIFFS_OK) {
        rc = PIOS_FLASHFS_ERROR_FS_MOUNT;
    }
    else
    {
        flashfs->mounted  = true;


        /* How many file available? */
        spiffs_DIR d;
        struct spiffs_dirent e;
        struct spiffs_dirent *pe = &e;

        SPIFFS_opendir(&flashfs->fs, "/", &d);

        flashfs->files = 0;
        while ((pe = SPIFFS_readdir(&d, pe))) {
            spiffs_file fd = SPIFFS_open_by_dirent(&flashfs->fs, pe, SPIFFS_RDWR, 0);
            if (fd >= 0) {
                flashfs->files++;
                SPIFFS_close(&flashfs->fs, fd);
            }
        }
        SPIFFS_closedir(&d);
        rc = flashfs->files;
    }

    return rc;
}


/**
 * @brief Initialize the file system
 */
int32_t PIOS_FLASHFS_Init(__attribute__((unused)) uintptr_t *fs_id,
                          const struct flashfs_cfg *cfg,
						  const struct pios_flash_driver *driver,
						  uintptr_t flash_id)
{
    int16_t fh;
    int16_t rc = PIOS_FLASHFS_OK;
    spiffs_stat fstats;

    struct flashfs_state *flashfs = (struct flashfs_state *)*fs_id;

    PIOS_DEBUG_PinHigh(PINDEBUG_SPIFFS_INIT);

    if (driver)
    {
		/* Make sure the underlying flash driver provides the minimal set of required methods */
		PIOS_Assert(driver->start_transaction);
		PIOS_Assert(driver->end_transaction);
		PIOS_Assert(driver->erase_sector);
		PIOS_Assert(driver->write_data);
		PIOS_Assert(driver->read_data);
    }

    /* We only support one instance of SPIFFS (SPIFFS_SINGLETON = 1). */

    /* This function can be called after a format. */
    /* (we won't re-allocate memory in that case) */
    if (!flashfs)
    {
        flashfs = (struct flashfs_state *)PIOS_FLASHFS_alloc();

        if (!flashfs)
            return PIOS_FLASHFS_ERROR_FS_ALLOC;


        flashfs->work_buf = pios_malloc(cfg->work_buffer_size);
        flashfs->cache_buf = pios_malloc(cfg->cache_buffer_size);
        flashfs->copy_buf = pios_malloc(cfg->logical_page_size);

        /* Mutex is used in SPIFFS */
#if defined(PIOS_INCLUDE_FREERTOS)
        if (!flashfs_mutex)
            flashfs_mutex = xSemaphoreCreateRecursiveMutex();
#endif

        /* Bind configuration parameters to this filesystem instance */
        flashfs->driver   = driver; /* lower-level flash driver */
        flashfs->flash_id = flash_id; /* lower-level flash device id */
        flashfs->mounted  = false;
        flashfs->cfg      = cfg;

        /* HAL Callbacks */
        flashfs->cfg_spiffs.hal_read_f = PIOS_FLASHFS_spiffs_read;
        flashfs->cfg_spiffs.hal_write_f = PIOS_FLASHFS_spiffs_write;
        flashfs->cfg_spiffs.hal_erase_f = PIOS_FLASHFS_spiffs_erase;

        flashfs->cfg_spiffs.phys_size = cfg->physical_size;
        flashfs->cfg_spiffs.phys_addr = cfg->physical_addr;
        flashfs->cfg_spiffs.phys_erase_block = cfg->physical_erase_block;
        flashfs->cfg_spiffs.log_block_size = cfg->logical_block_size;
        flashfs->cfg_spiffs.log_page_size = cfg->logical_page_size;

        flashfs->cfg_spiffs.fs_id = (void *)flashfs;

        /* Init buffers */
        flashfs->cfg_spiffs.buffer = flashfs->copy_buf;
    }

    /* File system should have one file (the "ID" file) */
    /* Currently that's the only integrity scheme available */
    if ((PIOS_FLASHFS_Mount((uintptr_t)flashfs) < 1) ||
        (SPIFFS_stat(&flashfs->fs, PIOS_FLASHFS_OPID_FILENAME, &fstats)) ||
        (!fstats.size))
    {
        PIOS_FLASHFS_Format((uintptr_t)flashfs);
        PIOS_FLASHFS_AddIDFile((uintptr_t)flashfs);
    }

    fh = SPIFFS_open(&flashfs->fs, PIOS_FLASHFS_OPID_FILENAME, SPIFFS_RDONLY, 0);
    if (fh >= 0) {
        /* We should probably check ID file content... */
        SPIFFS_close(&flashfs->fs, fh);

        *fs_id = (uintptr_t)flashfs;
    }
    else
    {
        rc = PIOS_FLASHFS_ERROR_FS_INVALID;
    }

    PIOS_DEBUG_PinLow(PINDEBUG_SPIFFS_INIT);

    PIOS_Assert(rc == PIOS_FLASHFS_OK);
    return rc;
}


/**
 * Delete the filesystem information from memory.
 * @param[in] fs_id flash device id
 */
int32_t PIOS_FLASHFS_Destroy(__attribute__((unused)) uintptr_t fs_id)
{
    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

    if (!PIOS_FLASHFS_Validate(flashfs))
        return PIOS_FLASHFS_ERROR_FS_INVALID;

    if (flashfs->mounted == true)
	{
		SPIFFS_unmount(&flashfs->fs);
		flashfs->mounted = false;
	}

    PIOS_FLASHFS_free(flashfs);

    return PIOS_FLASHFS_OK;
}


/**
 * @brief Opens/creates a file.
 * @param[in] fs_id The filesystem to use for this action
 * @param[in] path the path of the new file
 * @param[in] flags the flags for the open command, can be combinations of
 *                      SPIFFS_APPEND, SPIFFS_TRUNC, SPIFFS_CREAT, SPIFFS_RDONLY,
 *                      SPIFFS_WRONLY, SPIFFS_RDWR, SPIFFS_DIRECT
 */
int16_t PIOS_FLASHFS_Open(uintptr_t fs_id, const char *path, uint16_t flags)
{
	int16_t file_id;

    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

    if (!PIOS_FLASHFS_Validate(flashfs))
        return PIOS_FLASHFS_ERROR_FS_INVALID;

    file_id = (int16_t)SPIFFS_open(&flashfs->fs, path, flags, 0);

    if (file_id < PIOS_FLASHFS_OK)
    	return PIOS_FLASHFS_ERROR_OPEN_FILE;

    return file_id;
}


/**
 * @brief Close a filehandle.
 * @param[in] fs_id The filesystem to use for this action
 * @param[in] the filehandle of the file to close
 */
int32_t PIOS_FLASHFS_Close(uintptr_t fs_id, int16_t file_id)
{
    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

    if (!PIOS_FLASHFS_Validate(flashfs))
        return PIOS_FLASHFS_ERROR_FS_INVALID;

    SPIFFS_close(&flashfs->fs, file_id);

    return PIOS_FLASHFS_OK;
}


/**
 * @brief Saves one object instance to the filesystem
 * @param[in] fs_id The filesystem to use for this action
 * @param[in] fh File handle
 * @param[in] data Contents of the object being written
 * @param[in] size Size of the object being saved
 */
int32_t PIOS_FLASHFS_Write(uintptr_t fs_id, int16_t fh, uint8_t *data, uint16_t size)
{
	int32_t bytes_written = 0;
	int32_t rc = PIOS_FLASHFS_OK;

    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

	PIOS_DEBUG_PinHigh(PINDEBUG_SPIFFS_WRITE);

    if (!PIOS_FLASHFS_Validate(flashfs))
        return PIOS_FLASHFS_ERROR_FS_INVALID;

	bytes_written = SPIFFS_write(&flashfs->fs, (spiffs_file)fh, data, (int32_t)size);

	if (bytes_written != size) {
		rc = PIOS_FLASHFS_ERROR_WRITE_FILE;
		goto out_exit;
	}

	flashfs->files++;

out_exit:
	PIOS_DEBUG_PinLow(PINDEBUG_SPIFFS_WRITE);
    return rc;
}

/**
 * @brief Load one object instance from the filesystem
 * @param[in] fs_id The filesystem to use for this action
 * @param[in] fh File handle
 * @param[in] data Buffer to hold the contents of the loaded object
 * @param[in] size Size of the object to be loaded
 */
int32_t PIOS_FLASHFS_Read(uintptr_t fs_id, int16_t fh, uint8_t *data, uint16_t size)
{
    int32_t bytes_read = 0;
    int32_t rc = PIOS_FLASHFS_OK;

    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

    PIOS_DEBUG_PinHigh(PINDEBUG_SPIFFS_READ);

    if (!PIOS_FLASHFS_Validate(flashfs))
        return PIOS_FLASHFS_ERROR_FS_INVALID;

    bytes_read = SPIFFS_read(&flashfs->fs, (spiffs_file)fh, data, (int32_t)size);

    if (bytes_read != size) {
        rc = PIOS_FLASHFS_ERROR_READ_FILE;
		goto out_exit;
    }

out_exit:
    PIOS_DEBUG_PinLow(PINDEBUG_SPIFFS_READ);
    return rc;
}


/**
 * @brief Moves the read/write file offset
 * @param[in] fs_id The filesystem to use for this action
 * @param[in] fh File handle
 * @param[in] how much/where to move the offset
 * @param[in] if FLASHFS_SEEK_SET, the file offset shall be set to offset bytes
 *            if FLASHFS_SEEK_CUR, the file offset shall be set to its current location + offset
 *            if FLASHFS_SEEK_END, the file offset shall be set to the size of the file - offset
 */
int32_t PIOS_FLASHFS_Lseek(uintptr_t fs_id, int16_t fh, int32_t offset, enum pios_flashfs_lseek_flags flag)
{
    int32_t rc = PIOS_FLASHFS_OK;

    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

    if (!PIOS_FLASHFS_Validate(flashfs))
        return PIOS_FLASHFS_ERROR_FS_INVALID;

    rc = SPIFFS_lseek(&flashfs->fs, (spiffs_file)fh, offset, pios_flashfs2spiffs_lseek_flags[flag]);

    if (rc == SPIFFS_ERR_END_OF_OBJECT)
        return PIOS_FLASHFS_ERROR_EOF;

    if (rc < 0)
        return PIOS_FLASHFS_ERROR_READ_FILE;

    return PIOS_FLASHFS_OK;
}


/**
 * @brief Delete one instance of an object from the filesystem
 * @param[in] fs_id The filesystem to use for this action
 * @param[in] path name of the file to delete
 */
int32_t PIOS_FLASHFS_Remove(uintptr_t fs_id, const char *path)
{
    int32_t rc = PIOS_FLASHFS_OK;
    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

    PIOS_DEBUG_PinHigh(PINDEBUG_SPIFFS_DELETE);

    if (!PIOS_FLASHFS_Validate(flashfs))
        return PIOS_FLASHFS_ERROR_FS_INVALID;

    if (SPIFFS_remove(&flashfs->fs, path) != SPIFFS_OK) {
    	rc = PIOS_FLASHFS_ERROR_REMOVE_FILE;
    }

    flashfs->files--;

	PIOS_DEBUG_PinLow(PINDEBUG_SPIFFS_DELETE);
    return rc;
}


/**
 * @brief Helper function: 'find / -name "prefix*" | wc -l'
 * @param[in] fs_id The filesystem to use for this action
 * @param[in] path string to use for the search
 * @param[in] size number of bytes to use as a prefix for the search
 * @param[in] flags the flags for the find command, can be combinations of REMOVE
 */
int32_t PIOS_FLASHFS_Find(uintptr_t fs_id, const char *path, uint16_t prefix_size, uint32_t flags)
{
	int32_t filecount = 0;
	int16_t file_id;

    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

    if (!PIOS_FLASHFS_Validate(flashfs))
        return PIOS_FLASHFS_ERROR_FS_INVALID;

    if (prefix_size > FLASHFS_FILENAME_LEN)
        return PIOS_FLASHFS_ERROR_PREFIX_SIZE;

	spiffs_DIR d;
	struct spiffs_dirent e;
	struct spiffs_dirent *pe = &e;

	SPIFFS_opendir(&flashfs->fs, "/", &d);

	if ((strcmp(path, "*") == 0) && (prefix_size == 1)) {
	    while ((pe = SPIFFS_readdir(&d, pe)))
	        filecount++;
	}
	else
	{
	    while ((pe = SPIFFS_readdir(&d, pe))) {
	        if (strncmp(path, (char*)pe->name, prefix_size) == 0) {
	            if (flags & PIOS_FLASHFS_REMOVE) {
	                file_id = SPIFFS_open_by_dirent(&flashfs->fs, pe, SPIFFS_RDWR, 0);
	                SPIFFS_fremove(&flashfs->fs, file_id);
	                flashfs->files--;
	            }
	            filecount++;
	        }
	    }
	}

    SPIFFS_closedir(&d);

    return filecount;
}


/**
 * @brief Helper function: 'ls / | awk '{nr++; if( nr == file_number) print $0}'
 * @param[in] fs_id The filesystem to use for this action
 * @param[out] file name.
 * @param[out] file size.
 * @param[in] file number in the dir.
 * @param[in] flags the flags for the open command, can be combinations of TBD
 */
int32_t PIOS_FLASHFS_Info(uintptr_t fs_id, char *path, uint32_t *size, uint32_t file_number, __attribute__((unused)) uint32_t flags)
{
    uint32_t filecount = 0;

    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

    if (!PIOS_FLASHFS_Validate(flashfs))
        return PIOS_FLASHFS_ERROR_FS_INVALID;

    spiffs_DIR d;
    struct spiffs_dirent e;
    struct spiffs_dirent *pe = &e;

    SPIFFS_opendir(&flashfs->fs, "/", &d);

    while ((pe = SPIFFS_readdir(&d, pe))) {
        if (filecount == file_number) {
            strcpy(path, (char*)pe->name);
            *size = pe->size;
        }
        filecount++;
    }

    SPIFFS_closedir(&d);

    return filecount;
}


/**
 * @brief Erases all filesystem arenas and activate the first arena
 * @param[in] fs_id The filesystem to use for this action
 */
int32_t PIOS_FLASHFS_Format(uintptr_t fs_id)
{
    int32_t rc = PIOS_FLASHFS_OK;
    bool previous_mount_status;

    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

    PIOS_DEBUG_PinHigh(PINDEBUG_SPIFFS_FORMAT);

    if (!PIOS_FLASHFS_Validate(flashfs))
        return PIOS_FLASHFS_ERROR_FS_INVALID;

    previous_mount_status = flashfs->mounted;

    if (previous_mount_status)
    {
        SPIFFS_unmount(&flashfs->fs);
        flashfs->mounted = false;
    }

    flashfs->driver->erase_chip(flashfs->flash_id);

    if (previous_mount_status)
    {
        rc = PIOS_FLASHFS_Mount((uintptr_t)flashfs);
        PIOS_FLASHFS_AddIDFile((uintptr_t)flashfs);
    }

    flashfs->files = 0;
    PIOS_DEBUG_PinLow(PINDEBUG_SPIFFS_FORMAT);
    return rc;
}


/**
 * @brief Returs stats for the filesystems
 * @param[in] fs_id The filesystem to use for this action
 */
int32_t PIOS_FLASHFS_GetStats(__attribute__((unused)) uintptr_t fs_id, struct PIOS_FLASHFS_Stats *stats)
{
    PIOS_Assert(stats);

    struct flashfs_state *flashfs = (struct flashfs_state *)fs_id;

    PIOS_DEBUG_PinHigh(PINDEBUG_SPIFFS_STATS);
    if (!PIOS_FLASHFS_Validate(flashfs))
        return PIOS_FLASHFS_ERROR_FS_INVALID;

    stats->block_free = flashfs->fs.free_blocks;
    stats->block_used = flashfs->fs.block_count - stats->block_free;
#if SPIFFS_CACHE_STATS
    stats->cache_hits = flashfs->fs.cache_hits;
    stats->cache_misses = flashfs->fs.cache_misses;
#else
    stats->cache_hits = 0;
    stats->cache_misses = 0;
#endif
#if SPIFFS_GC_STATS
    stats->gc = flashfs->fs.stats_gc_runs;
#else
    stats->gc = 0;
#endif
    stats->saved = flashfs->files;

    PIOS_DEBUG_PinLow(PINDEBUG_SPIFFS_STATS);

    return PIOS_FLASHFS_OK;
}
#endif /* PIOS_INCLUDE_FLASH */

/**
 * @}
 * @}
 */
