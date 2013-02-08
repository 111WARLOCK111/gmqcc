/*
 * Copyright (C) 2013
 *     Dale Weiler 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <sys/stat.h>
#include <dirent.h>
#include "gmqcc.h"

/*
 * The PAK format uses a FOURCC concept for storing the magic ident within
 * the header as a uint32_t.
 */  
#define PAK_FOURCC ((uint32_t)(('P' << 24) | ('A' << 16) | ('C' << 8) | 'K'))

typedef struct {
    uint32_t magic;  /* "PACK" */

    /*
     * Offset to first directory entry in PAK file.  It's often
     * best to store the directories at the end of the file opposed
     * to the front, since it allows easy insertion without having
     * to load the entire file into memory again.
     */     
    uint32_t diroff;
    uint32_t dirlen;
} pak_header_t;

/*
 * A directory, is sort of a "file entry".  The concept of
 * a directory in Quake world is a "file entry/record". This
 * describes a file (with directories/nested ones too in it's
 * file name).  Hence it can be a file, file with directory, or
 * file with directories.
 */ 
typedef struct {
    char     name[56];
    uint32_t pos;
    uint32_t len;
} pak_directory_t;

/*
 * Used to get the next token from a string, where the
 * strings themselfs are seperated by chracters from
 * `sep`.  This is essentially strsep.
 */   
static char *pak_tree_sep(char **str, const char *sep) {
    char *beg = *str;
    char *end;

    if (!beg)
        return NULL;

    if (*(end = beg + strcspn(beg, sep)))
        * end++ = '\0'; /* null terminate */
    else
          end   = 0;

    *str = end;
    return beg;
}

/*
 * When given a string like "a/b/c/d/e/file"
 * this function will handle the creation of
 * the directory structure, included nested
 * directories.
 */
static void pak_tree_build(const char *entry) {
    char *directory;
    char *elements[28];
    char *pathsplit;
    char *token;

    size_t itr;
    size_t jtr;

    pathsplit = mem_a(56);
    directory = mem_a(56);

    memset(pathsplit, 0, 56);
    memset(directory, 0, 56);

    strncpy(directory, entry, 56);
    for (itr = 0; (token = pak_tree_sep(&directory, "/")) != NULL; itr++) {
        elements[itr] = token;
    }

    for (jtr = 0; jtr < itr - 1; jtr++) {
        strcat(pathsplit, elements[jtr]);
        strcat(pathsplit, "/");

        if (fs_dir_make(pathsplit)) {
            mem_d(pathsplit);
            mem_d(directory);

            /* TODO: undo on fail */

            return;
        }
    }

    mem_d(pathsplit);
    mem_d(directory);
}

typedef struct {
    pak_directory_t *directories;
    pak_header_t     header;
    FILE            *handle;
    bool             insert;
} pak_file_t;

static pak_file_t *pak_open_read(const char *file) {
    pak_file_t *pak;
    size_t      itr;

    if (!(pak = mem_a(sizeof(pak_file_t))))
        return NULL;

    if (!(pak->handle = fs_file_open(file, "rb"))) {
        mem_d(pak);
        return NULL;
    }

    pak->directories = NULL;
    pak->insert      = false; /* read doesn't allow insert */

    memset         (&pak->header, 0, sizeof(pak_header_t));
    fs_file_read   (&pak->header,    sizeof(pak_header_t), 1, pak->handle);
    util_endianswap(&pak->header, 1, sizeof(pak_header_t));

    /*
     * Every PAK file has "PACK" stored as FOURCC data in the
     * header.  If this data cannot compare (as checked here), it's
     * probably not a PAK file.
     */
    if (pak->header.magic != PAK_FOURCC) {
        fs_file_close(pak->handle);
        mem_d        (pak);
        return NULL;
    }

    /*
     * Time to read in the directory handles and prepare the directories
     * vector.  We're going to be reading some the file inwards soon.
     */      
    fs_file_seek(pak->handle, pak->header.diroff, SEEK_SET);

    /*
     * Read in all directories from the PAK file. These are considered
     * to be the "file entries".
     */   
    for (itr = 0; itr < pak->header.dirlen / 64; itr++) {
        pak_directory_t dir;
        fs_file_read   (&dir,    sizeof(pak_directory_t), 1, pak->handle);
        util_endianswap(&dir, 1, sizeof(pak_directory_t));

        vec_push(pak->directories, dir);
    }
    return pak;
}

static pak_file_t *pak_open_write(const char *file) {
    pak_file_t *pak;

    if (!(pak = mem_a(sizeof(pak_file_t))))
        return NULL;

    /*
     * Generate the required directory structure / tree for
     * writing this PAK file too.
     */   
    pak_tree_build(file);

    if (!(pak->handle = fs_file_open(file, "wb"))) {
        /*
         * The directory tree that was created, needs to be
         * removed entierly if we failed to open a file.
         */   
        /* TODO backup directory clean */

        return NULL;
    }

    memset(&(pak->header), 0, sizeof(pak_header_t));

    /*
     * We're in "insert" mode, we need to do things like header
     * "patching" and writing the directories at the end of the
     * file.
     */
    pak->insert       = true;
    pak->header.magic = PAK_FOURCC;

    /*
     * We need to write out the header since files will be wrote out to
     * this even with directory entries, and that not wrote.  The header
     * will need to be patched in later with a file_seek, and overwrite,
     * we could use offsets and other trickery.  This is just easier.
     */
    fs_file_write(&(pak->header), sizeof(pak_header_t), 1, pak->handle);

    return pak;
}

pak_file_t *pak_open(const char *file, const char *mode) {
    if (!file || !mode)
        return NULL;

    switch (*mode) {
        case 'r': return pak_open_read (file);
        case 'w': return pak_open_write(file);
    }

    return NULL;
}

bool pak_exists(pak_file_t *pak, const char *file, pak_directory_t **dir) {
    size_t itr;

    if (!pak || !file)
        return false;
  
    for (itr = 0; itr < vec_size(pak->directories); itr++) {
        if (!strcmp(pak->directories[itr].name, file)) {
            /*
             * Store back a pointer to the directory that matches
             * the request if requested (NULL is not allowed).
             */   
            if (dir) {
                *dir = &(pak->directories[itr]);
            }
            return true;
        }
    }

    return false;
}

/*
 * Extraction abilities.  These work as you expect them to.
 */ 
bool pak_extract_one(pak_file_t *pak, const char *file) {
    pak_directory_t *dir = NULL;
    unsigned char   *dat = NULL;
    FILE            *out;

    if (!pak_exists(pak, file, &dir)) {
        return false;
    }

    if (!(dat = (unsigned char *)mem_a(dir->len))) {
        return false;
    }

    /*
     * Generate the directory structure / tree that will be required
     * to store the extracted file.
     */   
    pak_tree_build(file);

    /*
     * Now create the file, if this operation fails.  Then abort
     * It shouldn't fail though.
     */   
    if (!(out = fs_file_open(file, "wb"))) {
        mem_d(dat);
        return false;
    }


    /* read */
    fs_file_seek (pak->handle, dir->pos, SEEK_SET);
    fs_file_read (dat, 1, dir->len, pak->handle);

    /* write */
    fs_file_write(dat, 1, dir->len, out);

    /* close */
    fs_file_close(out);

    /* free */
    mem_d(dat);

    return true;
}

bool pak_extract_all(pak_file_t *pak, const char *dir) {
    size_t itr;

    if (!fs_dir_make(dir))
        return false;

    if (fs_dir_change(dir))
        return false;

    for (itr = 0; itr < vec_size(pak->directories); itr++) {
        if (!pak_extract_one(pak, pak->directories[itr].name))
            return false;
    }

    return true;
}

/*
 * Insertion functions (the opposite of extraction).  Yes for generating
 * PAKs.
 */
bool pak_insert_one(pak_file_t *pak, const char *file) {
    pak_directory_t dir;
    unsigned char  *dat;
    FILE           *fp;

    /*
     * We don't allow insertion on files that already exist within the
     * pak file.  Weird shit can happen if we allow that ;). We also
     * don't allow insertion if the pak isn't opened in write mode.  
     */ 
    if (!pak || !file || !pak->insert || pak_exists(pak, file, NULL))
        return false;

    if (!(fp = fs_file_open(file, "rb")))
        return false;

    /*
     * Calculate the total file length, since it will be wrote to
     * the directory entry, and the actual contents of the file
     * to the PAK file itself.
     */
    fs_file_seek(fp, 0, SEEK_END);
    dir.len = fs_file_tell(fp);
    fs_file_seek(fp, 0, SEEK_SET);

    dir.pos = fs_file_tell(pak->handle);

    /*
     * We're limited to 56 bytes for a file name string, that INCLUDES
     * the directory and '/' seperators.
     */   
    if (strlen(file) >= 56) {
        fs_file_close(fp);
        return false;
    }

    strcpy(dir.name, file);

    /*
     * Allocate some memory for loading in the data that will be
     * redirected into the PAK file.
     */   
    if (!(dat = (unsigned char *)mem_a(dir.len))) {
        fs_file_close(fp);
        return false;
    }

    fs_file_read (dat, dir.len, 1, fp);
    fs_file_close(fp);
    fs_file_write(dat, dir.len, 1, pak->handle);

    /*
     * Now add the directory to the directories vector, so pak_close
     * can actually write it.
     */
    vec_push(pak->directories, dir);

    return true;
}

/*
 * Like pak_insert_one, except this collects files in all directories
 * from a root directory, and inserts them all.
 */  
bool pak_insert_all(pak_file_t *pak, const char *dir) {
    DIR           *dp;
    struct dirent *dirp;

    if (!(pak->insert))
        return false;

    if (!(dp = fs_dir_open(dir)))
        return false;

    while ((dirp = fs_dir_read(dp))) {
        if (!(pak_insert_one(pak, dirp->d_name))) {
            fs_dir_close(dp);
            return false;
        }
    }

    fs_dir_close(dp);
    return true;
}

bool pak_close(pak_file_t *pak) {
    size_t itr;

    if (!pak)
        return false;

    /*
     * In insert mode we need to patch the header, and write
     * our directory entries at the end of the file.
     */  
    if (pak->insert) {
        pak->header.dirlen = vec_size(pak->directories) * 64;
        pak->header.diroff = ftell(pak->handle);

        /* patch header */ 
        fs_file_seek (pak->handle, 0, SEEK_SET);
        fs_file_write(&(pak->header), sizeof(pak_header_t), 1, pak->handle);

        /* write directories */
        fs_file_seek (pak->handle, pak->header.diroff, SEEK_SET);

        for (itr = 0; itr < vec_size(pak->directories); itr++) {
            fs_file_write(&(pak->directories[itr]), sizeof(pak_directory_t), 1, pak->handle);
        }
    }

    vec_free     (pak->directories);
    fs_file_close(pak->handle);
    mem_d        (pak);

    return true;
}