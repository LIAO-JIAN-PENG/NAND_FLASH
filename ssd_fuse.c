/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME       "ssd_file"

enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};

enum
{
    DIRTY = -2,         // dirty page(過時的資料：已被更新)
    CLEAR = -1         // invalid page(未被寫入資料：0XFFFFFFFF)
};

int reserve_nand = PHYSICAL_NAND_NUM - 1; // reserve nand
int P2L[PHYSICAL_NAND_NUM][NAND_SIZE_KB * 1024 / 512] = {[0 ... PHYSICAL_NAND_NUM-1][0 ... NAND_SIZE_KB * 1024 / 512-1] = CLEAR};
int remain_pages = (PHYSICAL_NAND_NUM - 1) * NAND_SIZE_KB * 1024 / 512;
int dirty_block_list[PHYSICAL_NAND_NUM] = {0}; // 紀錄每個block的dirty page數，用於gc找最少dirty block

static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

typedef union pca_rule PCA_RULE;
union pca_rule
{
    unsigned int pca;
    struct
    {
        unsigned int page : 16;
        unsigned int block: 16;
    } fields;
};

PCA_RULE curr_pca;

unsigned int* L2P;

static int ssd_resize(size_t new_size)
{
    //set logic size to new_size
    //Logic size is 5 * 10 * 1024 = 5 * 20 * 512
    if (new_size > LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024  )
    {
        return -ENOMEM;
    }
    else
    {
        logic_size = new_size;
        return 0;
    }

}

static int ssd_expand(size_t new_size)
{
    //logical size must be less than logic limit

    if (new_size > logic_size)
    {
        return ssd_resize(new_size);
    }

    return 0;
}

static int nand_read(char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    //read from nand
    if ( (fptr = fopen(nand_name, "r") ))
    {
        fseek( fptr, my_pca.fields.page * 512, SEEK_SET );
        fread(buf, 1, 512, fptr);
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    return 512;
}
static int nand_write(const char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    //write to nand
    if ( (fptr = fopen(nand_name, "r+")))
    {
        fseek( fptr, my_pca.fields.page * 512, SEEK_SET );
        fwrite(buf, 1, 512, fptr);
        fclose(fptr);
        physic_size ++;
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }

    nand_write_size += 512;
    return 512;
}

static int nand_erase(int block)
{
    char nand_name[100];
	// int found = 0;
    FILE* fptr;

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block);

    //erase nand
    if ( (fptr = fopen(nand_name, "w")))
    {
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand (%s) erase nand = %d, return %d\n", nand_name, block, -EINVAL);
        return -EINVAL;
    }


	// if (found == 0)
	// {
	// 	printf("nand erase not found\n");
	// 	return -EINVAL;
	// }

    printf("nand erase %d pass\n", block);

    physic_size -= NAND_SIZE_KB * 1024 / 512;
    
    return 1;
}

// Pre-declare the function ftl_gc
static unsigned int ftl_gc();

static unsigned int get_next_pca()
{
    /*  TODO (Done): seq A, need to change to seq B */
    /*  Hint: Change next pca address to next page instead of next NAND */
	int gc;
    if (curr_pca.pca == INVALID_PCA)
    {
        //init
        curr_pca.pca = 0;
        return curr_pca.pca;
    }
    else if (curr_pca.pca == FULL_PCA)
    {
        //ssd is full, no pca can be allocated
        printf("No new PCA\n");
        return FULL_PCA;
    }

    if ( curr_pca.fields.page == (NAND_SIZE_KB * 1024 / 512) - 1)
    {
        curr_pca.fields.block += 1;
        if (curr_pca.fields.block == reserve_nand)
            curr_pca.fields.block += 1; // skip reserve block
    }
    curr_pca.fields.page = (curr_pca.fields.page + 1 ) % (NAND_SIZE_KB * 1024 / 512);
    
    if (remain_pages == 0)
    {
        printf("GC start\n");
        printf("reserve_nand = %d\n", reserve_nand);
        gc = ftl_gc();
        printf("curr_pca = %d block = %d page = %d\n", curr_pca.pca, curr_pca.fields.block, curr_pca.fields.page);
        return gc;
    }
    
    printf("PCA = page %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);
    return curr_pca.pca;
}

static int ftl_read( char* buf, size_t lba)
{
    PCA_RULE pca;

	pca.pca = L2P[lba];
	if (pca.pca == INVALID_PCA) {
	    //data has not be written, return 0
	    return 0;
	}
	else {
	    return nand_read(buf, pca.pca);
	}
}

static int ftl_write(const char* buf, size_t lba_range, size_t lba)
{
    /*  TODO (Done): only basic write case, need to consider other cases */
    /*  Hint: 3 things
        • Use get_next_pca to find empty PCA for data writing
        • Write data from buffer to PCA
        • Update L2P table
    */
    PCA_RULE pca;
    pca.pca = get_next_pca();

    printf("Write LBA %ld to PCA %d\n", lba, pca.pca);

    if (pca.pca == FULL_PCA)
    {
        printf("No empty PCA available\n");
        return -ENOMEM;
    }

    if (nand_write(buf, pca.pca) > 0)
    {
        PCA_RULE outdate_pca;
        if(L2P[lba] != CLEAR) // 有資料
        {
            outdate_pca.pca = L2P[lba];
            dirty_block_list[outdate_pca.fields.block]++;
            printf(" --> outdate_pca = %d => DIRTY\n", outdate_pca.pca);
            P2L[outdate_pca.fields.block][outdate_pca.fields.page] = DIRTY;
        }
        printf(" --> page = %d, block = %d\n", pca.fields.page, pca.fields.block);
        L2P[lba] = pca.pca;
        P2L[pca.fields.block][pca.fields.page] = lba;
        printf(" --> L2P[%d] = %d\n", (int)lba, L2P[lba]);
        remain_pages--;
        printf(" --> remain_pages = %d\n", remain_pages);
        printf(" --> reserve_nand = %d\n", reserve_nand);
        printf(" --> P2L\n");
        int i, j;
        for (i = 0; i < PHYSICAL_NAND_NUM; i++){
            for (j = 0; j < NAND_SIZE_KB*2; j++){
                printf("%2d ", P2L[i][j]);
            }
            printf("\n");
        }
        return 512;
    }
    else
    {
        printf("Write failed\n");
        return -EINVAL;
    }
}

/*  TODO: Implement ftl_gc to do garbage collection */

static unsigned int ftl_gc()
{
    /* TODO: GC garbage collection */
    /* Hint: 4 steps
        1. choose the most dirty pages block as source block
        2. copy valid page to reserve block, update L2P table
        3. update P2L table
        4. set source block as new reserve block
    */

    PCA_RULE reserve_pca;
    reserve_pca.fields.block = reserve_nand;
    reserve_pca.fields.page = 0;

    // Step 1: choose the most dirty pages block as source block
    int max = 0;        // max dirty page
    int max_idx = 0;    // max dirty page block index
    for(int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        if(dirty_block_list[i] > max)
        {
            max = dirty_block_list[i];
            max_idx = i;
        }
    }
    if(max == 0)
    {
        printf("No dirty block -> SSD FULL\n");
        curr_pca.pca = FULL_PCA;
        return FULL_PCA;
    }

    dirty_block_list[max_idx] = 0;
    curr_pca.fields.block = max_idx;
    curr_pca.fields.page = 0;

    // Step 2: copy valid page to reserve block, update L2P table
    for(int page=0; page < NAND_SIZE_KB*1024/512;page++){
        

        if(P2L[curr_pca.fields.block][page] >= 0){ // current page is valid(with lba)
            char buf[512];
            nand_read(buf, curr_pca.pca + page);
            nand_write(buf, reserve_pca.pca);
            L2P[P2L[curr_pca.fields.block][page]] = reserve_pca.pca;

            P2L[reserve_pca.fields.block][reserve_pca.fields.page] = P2L[curr_pca.fields.block][page];
            reserve_pca.fields.page++;
        }
        else{
            remain_pages++;  // recycle invalid page
        }
    }

    // Step 3: update P2L table
    nand_erase(curr_pca.fields.block);
    for(int page=0; page < NAND_SIZE_KB*1024/512;page++){
        P2L[curr_pca.fields.block][page] = CLEAR;
    }

    // Step 4: set source block as new reserve block
    reserve_nand = curr_pca.fields.block;
    if (reserve_pca.fields.page >= NAND_SIZE_KB * (1024 / 512)){
        printf("After GC -> SSD FULL\n");
        curr_pca.pca = FULL_PCA;
        return FULL_PCA;
    }
        
    curr_pca.pca = reserve_pca.pca;
    
    return curr_pca.pca;
}



static int ssd_file_type(const char* path)
{
    if (strcmp(path, "/") == 0)
    {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0)
    {
        return SSD_FILE;
    }
    return SSD_NONE;
}
static int ssd_getattr(const char* path, struct stat* stbuf,
                       struct fuse_file_info* fi)
{
    (void) fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path))
    {
        case SSD_ROOT:
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;
        case SSD_FILE:
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = logic_size;
            break;
        case SSD_NONE:
            return -ENOENT;
    }
    return 0;
}
static int ssd_open(const char* path, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_NONE)
    {
        return 0;
    }
    return -ENOENT;
}
static int ssd_do_read(char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range, rst ;
    char* tmp_buf;

    // out of limit
    if ((offset ) >= logic_size)
    {
        return 0;
    }
    if ( size > logic_size - offset)
    {
        //is valid data section
        size = logic_size - offset;
    }

    tmp_lba = offset / 512;
	tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    tmp_buf = calloc(tmp_lba_range * 512, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++) {
        rst = ftl_read(tmp_buf + i * 512, tmp_lba++);
        if ( rst == 0)
        {
            //data has not be written, return empty data
            memset(tmp_buf + i * 512, 0, 512);
        }
        else if (rst < 0 )
        {
            free(tmp_buf);
            return rst;
        }
    }

    memcpy(buf, tmp_buf + offset % 512, size);

    free(tmp_buf);
    return size;
}
static int ssd_read(const char* path, char* buf, size_t size,
                    off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}
static int ssd_do_write(const char* buf, size_t size, off_t offset)
{
/*  TODO (Done): only basic write case, need to consider other cases */
    /*  Hint: 2 thins
        • Divide write cmd into 512B package by size
        • Use ftl_write to write data
        • Need to handle writing non-aligned data
    */
	
    int tmp_lba, tmp_lba_range, process_size;
    int idx, curr_size, remain_size, rst;

    host_write_size += size;

    if (ssd_expand(offset + size) != 0)
    {
        printf("SSD Expand failed!\n");
        return -ENOMEM;
    }

    tmp_lba = offset / 512;
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;

    process_size = 0;
    remain_size = size;
    curr_size = 0;
    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        int aligned_offset = offset % 512;
        int aligned_size = 512 - aligned_offset;
        int write_size = aligned_size < remain_size ? aligned_size : remain_size;

        if (aligned_offset == 0 && write_size == 512)
        {
            // Aligned to 512B, send FTL-write API by LBA
            rst = ftl_write(buf + process_size, write_size, tmp_lba + idx);
        }
        else
        {
            // Not aligned to 512B, do Read Modify Write operation (RMW)
            char* tmp_buf = calloc(512, sizeof(char));
            rst = ftl_read(tmp_buf, tmp_lba + idx);
            if (rst < 0)
            {
                free(tmp_buf);
                return rst;
            }

            memcpy(tmp_buf + aligned_offset, buf + process_size, write_size);

            rst = ftl_write(tmp_buf, 512, tmp_lba + idx);
            free(tmp_buf);
        }

        if (rst < 0)
        {
            return rst;
        }

        curr_size += write_size;
        remain_size -= write_size;
        process_size += write_size;
        offset += write_size;
    }

    return size;
}
static int ssd_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}
static int ssd_truncate(const char* path, off_t size,
                        struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }

    return ssd_resize(size);
}
static int ssd_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info* fi,
                       enum fuse_readdir_flags flags)
{
    (void) fi;
    (void) offset;
    (void) flags;
    if (ssd_file_type(path) != SSD_ROOT)
    {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}
static int ssd_ioctl(const char* path, unsigned int cmd, void* arg,
                     struct fuse_file_info* fi, unsigned int flags, void* data)
{

    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT)
    {
        return -ENOSYS;
    }
    switch (cmd)
    {
        case SSD_GET_LOGIC_SIZE:
            *(size_t*)data = logic_size;
            printf(" --> logic size: %ld\n", logic_size);
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            printf(" --> physic size: %ld\n", physic_size);
            return 0;
        case SSD_GET_WA:
            *(double*)data = (double)nand_write_size / (double)host_write_size;
            return 0;
    }
    return -EINVAL;
}
static const struct fuse_operations ssd_oper =
{
    .getattr        = ssd_getattr,
    .readdir        = ssd_readdir,
    .truncate       = ssd_truncate,
    .open           = ssd_open,
    .read           = ssd_read,
    .write          = ssd_write,
    .ioctl          = ssd_ioctl,
};
int main(int argc, char* argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
	nand_write_size = 0;
	host_write_size = 0;
    curr_pca.pca = INVALID_PCA;
    L2P = malloc(LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / 512 * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int)*LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / 512);

    //create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        FILE* fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL)
        {
            printf("open fail");
        }
        fclose(fptr);
    }
    return fuse_main(argc, argv, &ssd_oper, NULL);
}