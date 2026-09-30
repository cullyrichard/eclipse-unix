#ifndef _FS_H
#define _FS_H

/* fs.h -- a small V6-Unix-style filesystem on the DSK block device. See
 * FS_NOTES.md for the on-disk format; kernel/fstools/mkfs.py and fsck.py
 * implement the same format on the host.
 *
 * Disk: 512-byte blocks (256 words, DSK_WORDS_PER_SECTOR), words stored
 * big-endian as bytes within a block exactly as LDB/STB see them.
 *
 *   block 0            boot (unused)
 *   block 1            superblock
 *   block BMAP..       free-block bitmap, 1 bit per block, 1 = in use
 *   block ISTART..     inodes, 16 words each, 16 per block (inode 0 unused)
 *   block DSTART..     data
 */

#define BSIZE      512          /* bytes per block */
#define BWORDS     256          /* words per block */
#define FS_MAGIC   0105106      /* "EF" */
#define ROOTINO    1
#define IWORDS     16           /* words per inode */
#define IPB        16           /* inodes per block */
#define NDIRECT    8
#define NINDIRECT  256          /* block numbers per indirect block */
#define DIRSIZ     14           /* name bytes per directory entry */
#define DENTSZ     16           /* bytes per directory entry */
#define MAXFILE    65535u       /* size is a 16-bit byte count */

/* superblock words */
#define SB_MAGIC   0
#define SB_NBLOCKS 1
#define SB_NINODES 2
#define SB_BMAP    3
#define SB_ISTART  4
#define SB_DSTART  5

/* inode words */
#define I_TYPE     0            /* T_* */
#define I_NLINK    1
#define I_SIZE     2            /* bytes */
#define I_ADDR     4            /* 4..11: direct blocks */
#define I_INDIR    12           /* indirect block */

#define T_FREE     0
#define T_FILE     1
#define T_DIR      2

int fs_init(void);                              /* 0 ok, -1 bad superblock */
int fs_iread(unsigned int inum, unsigned int *ip);   /* ip[IWORDS] */
void fs_iwrite(unsigned int inum, const unsigned int *ip);
unsigned int fs_ialloc(unsigned int type);      /* 0 = none free */
int fs_readi(unsigned int inum, unsigned int off, char *dst, unsigned int n);
int fs_writei(unsigned int inum, unsigned int off, const char *src, unsigned int n);
void fs_trunc(unsigned int inum);
unsigned int fs_namei(const char *path);        /* 0 = not found */
/* Parent directory of path, and the last component copied to name
 * (DIRSIZ bytes, NUL padded). 0 = parent not found / bad path. */
unsigned int fs_nameiparent(const char *path, char *name);
unsigned int fs_dirlookup(unsigned int dinum, const char *name);
int fs_dirlink(unsigned int dinum, const char *name, unsigned int inum);
int fs_dirunlink(unsigned int dinum, const char *name);
int fs_mkdir(const char *path);
int fs_unlink(const char *path);
unsigned int fs_create(const char *path);        /* existing file or new; 0 = fail */
unsigned int fs_freeblocks(void);
extern unsigned int fs_cwd;             /* start of relative paths */

#endif
