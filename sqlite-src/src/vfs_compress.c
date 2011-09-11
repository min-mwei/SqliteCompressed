/*
** 2011 Sep 03
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains code implements a VFS shim that writes compressed database.
**
** USAGE:
**
** This source file exports a single symbol which is the name of a function:
**
**   int vfscompress_register(
**       int trace,                  // True to trace operations to stderr
**       int compressionLevel        // The compression level: -1 for default, 1 fastest, 9 best
**   );
**
*/
#include "sqliteInt.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "zlib.h"
#include <winbase.h>
#include <WinIoCtl.h>

#ifdef __CYGWIN__
# include <sys/cygwin.h>
#endif

extern void *convertUtf8Filename(const char *zFilename);

/*
** The chunk size is the compression unit.
** It must be in multiple of max-page-size.
** Defaults to 4 max-pages (64k * 4).
*/
#define CHUNK_SIZE_BYTES    (4 * 64 * 1024)

enum state
{
    empty,          //< no data at all.
    uncompressed,   //< new data not compressed.
    unwritten,      //< compressed data in memory.
    cached          //< compressed data flushed.
};

typedef struct vfsc_chunk vfsc_chunk;
struct vfsc_chunk {
    sqlite_int64 offset;
    int origSize;
    int compSize;
    char pOrigData[CHUNK_SIZE_BYTES + 1024];
    char pCompData[CHUNK_SIZE_BYTES + 1024];
    char state;
};


/*
** An instance of this structure is attached to the each trace VFS to
** provide auxiliary information.
*/
typedef struct vfstrace_info vfstrace_info;
struct vfstrace_info {
  sqlite3_vfs *pRootVfs;              /* The underlying real VFS */
  int (*xOut)(const char*, void*);    /* Send output here */
  void *pOutArg;                      /* First argument to xOut */
  const char *zVfsName;               /* Name of this trace-VFS */
  sqlite3_vfs *pTraceVfs;             /* Pointer back to the trace VFS */
  vfsc_chunk *pCache;
  int trace;
  //int pages_per_chunk;                /* Number of pages in a single chunk */
};

/*
** The sqlite3_file object for the trace VFS
*/
typedef struct vfstrace_file vfstrace_file;
struct vfstrace_file {
  sqlite3_file base;        /* Base class.  Must be first */
  vfstrace_info *pInfo;     /* The trace-VFS to which this file belongs */
  const char *zFName;       /* Base name of the file */
  sqlite3_file *pReal;      /* The real underlying file */
  HANDLE hFile;             /* The underlying file handle */
};

/*
** Method declarations for vfstrace_file.
*/
static int vfstraceClose(sqlite3_file*);
static int vfstraceRead(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
static int vfstraceWrite(sqlite3_file*,const void*,int iAmt, sqlite3_int64);
static int vfstraceTruncate(sqlite3_file*, sqlite3_int64 size);
static int vfstraceSync(sqlite3_file*, int flags);
static int vfstraceFileSize(sqlite3_file*, sqlite3_int64 *pSize);
static int vfstraceLock(sqlite3_file*, int);
static int vfstraceUnlock(sqlite3_file*, int);
static int vfstraceCheckReservedLock(sqlite3_file*, int *);
static int vfstraceFileControl(sqlite3_file*, int op, void *pArg);
static int vfstraceSectorSize(sqlite3_file*);
static int vfstraceDeviceCharacteristics(sqlite3_file*);
static int vfstraceShmLock(sqlite3_file*,int,int,int);
static int vfstraceShmMap(sqlite3_file*,int,int,int, void volatile **);
static void vfstraceShmBarrier(sqlite3_file*);
static int vfstraceShmUnmap(sqlite3_file*,int);

/*
** Method declarations for vfstrace_vfs.
*/
static int vfstraceOpen(sqlite3_vfs*, const char *, sqlite3_file*, int , int *);
static int vfstraceDelete(sqlite3_vfs*, const char *zName, int syncDir);
static int vfstraceAccess(sqlite3_vfs*, const char *zName, int flags, int *);
static int vfstraceFullPathname(sqlite3_vfs*, const char *zName, int, char *);
static void *vfstraceDlOpen(sqlite3_vfs*, const char *zFilename);
static void vfstraceDlError(sqlite3_vfs*, int nByte, char *zErrMsg);
static void (*vfstraceDlSym(sqlite3_vfs*,void*, const char *zSymbol))(void);
static void vfstraceDlClose(sqlite3_vfs*, void*);
static int vfstraceRandomness(sqlite3_vfs*, int nByte, char *zOut);
static int vfstraceSleep(sqlite3_vfs*, int microseconds);
static int vfstraceCurrentTime(sqlite3_vfs*, double*);
static int vfstraceGetLastError(sqlite3_vfs*, int, char*);
static int vfstraceCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64*);
static int vfstraceSetSystemCall(sqlite3_vfs*,const char*, sqlite3_syscall_ptr);
static sqlite3_syscall_ptr vfstraceGetSystemCall(sqlite3_vfs*, const char *);
static const char *vfstraceNextSystemCall(sqlite3_vfs*, const char *zName);

static int CompressionLevel = Z_DEFAULT_COMPRESSION;


/*
** Return a pointer to the tail of the pathname.  Examples:
**
**     /home/drh/xyzzy.txt -> xyzzy.txt
**     xyzzy.txt           -> xyzzy.txt
*/
static const char *fileTail(const char *z){
  int i;
  if( z==0 ) return 0;
  i = strlen(z)-1;
  while( i>0 && z[i-1]!='/' ){ i--; }
  return &z[i];
}

/*
** Send trace output defined by zFormat and subsequent arguments.
*/
static void vfstrace_printf(
  vfstrace_info *pInfo,
  const char *zFormat,
  ...
){
  va_list ap;
  char *zMsg;
  if (pInfo->trace)
  {
      va_start(ap, zFormat);
      zMsg = sqlite3_vmprintf(zFormat, ap);
      va_end(ap);
      pInfo->xOut(zMsg, pInfo->pOutArg);
      sqlite3_free(zMsg);
  }
}

/*
** Convert value rc into a string and print it using zFormat.  zFormat
** should have exactly one %s
*/
static void vfstrace_print_errcode(
  vfstrace_info *pInfo,
  const char *zFormat,
  int rc
){
  char zBuf[50];
  char *zVal;
  switch( rc ){
    case SQLITE_OK:         zVal = "SQLITE_OK";          break;
    case SQLITE_ERROR:      zVal = "SQLITE_ERROR";       break;
    case SQLITE_PERM:       zVal = "SQLITE_PERM";        break;
    case SQLITE_ABORT:      zVal = "SQLITE_ABORT";       break;
    case SQLITE_BUSY:       zVal = "SQLITE_BUSY";        break;
    case SQLITE_NOMEM:      zVal = "SQLITE_NOMEM";       break;
    case SQLITE_READONLY:   zVal = "SQLITE_READONLY";    break;
    case SQLITE_INTERRUPT:  zVal = "SQLITE_INTERRUPT";   break;
    case SQLITE_IOERR:      zVal = "SQLITE_IOERR";       break;
    case SQLITE_CORRUPT:    zVal = "SQLITE_CORRUPT";     break;
    case SQLITE_FULL:       zVal = "SQLITE_FULL";        break;
    case SQLITE_CANTOPEN:   zVal = "SQLITE_CANTOPEN";    break;
    case SQLITE_PROTOCOL:   zVal = "SQLITE_PROTOCOL";    break;
    case SQLITE_EMPTY:      zVal = "SQLITE_EMPTY";       break;
    case SQLITE_SCHEMA:     zVal = "SQLITE_SCHEMA";      break;
    case SQLITE_CONSTRAINT: zVal = "SQLITE_CONSTRAINT";  break;
    case SQLITE_MISMATCH:   zVal = "SQLITE_MISMATCH";    break;
    case SQLITE_MISUSE:     zVal = "SQLITE_MISUSE";      break;
    case SQLITE_NOLFS:      zVal = "SQLITE_NOLFS";       break;
    case SQLITE_IOERR_READ:         zVal = "SQLITE_IOERR_READ";         break;
    case SQLITE_IOERR_SHORT_READ:   zVal = "SQLITE_IOERR_SHORT_READ";   break;
    case SQLITE_IOERR_WRITE:        zVal = "SQLITE_IOERR_WRITE";        break;
    case SQLITE_IOERR_FSYNC:        zVal = "SQLITE_IOERR_FSYNC";        break;
    case SQLITE_IOERR_DIR_FSYNC:    zVal = "SQLITE_IOERR_DIR_FSYNC";    break;
    case SQLITE_IOERR_TRUNCATE:     zVal = "SQLITE_IOERR_TRUNCATE";     break;
    case SQLITE_IOERR_FSTAT:        zVal = "SQLITE_IOERR_FSTAT";        break;
    case SQLITE_IOERR_UNLOCK:       zVal = "SQLITE_IOERR_UNLOCK";       break;
    case SQLITE_IOERR_RDLOCK:       zVal = "SQLITE_IOERR_RDLOCK";       break;
    case SQLITE_IOERR_DELETE:       zVal = "SQLITE_IOERR_DELETE";       break;
    case SQLITE_IOERR_BLOCKED:      zVal = "SQLITE_IOERR_BLOCKED";      break;
    case SQLITE_IOERR_NOMEM:        zVal = "SQLITE_IOERR_NOMEM";        break;
    case SQLITE_IOERR_ACCESS:       zVal = "SQLITE_IOERR_ACCESS";       break;
    case SQLITE_IOERR_CHECKRESERVEDLOCK:
                               zVal = "SQLITE_IOERR_CHECKRESERVEDLOCK"; break;
    case SQLITE_IOERR_LOCK:         zVal = "SQLITE_IOERR_LOCK";         break;
    case SQLITE_IOERR_CLOSE:        zVal = "SQLITE_IOERR_CLOSE";        break;
    case SQLITE_IOERR_DIR_CLOSE:    zVal = "SQLITE_IOERR_DIR_CLOSE";    break;
    case SQLITE_IOERR_SHMOPEN:      zVal = "SQLITE_IOERR_SHMOPEN";      break;
    case SQLITE_IOERR_SHMSIZE:      zVal = "SQLITE_IOERR_SHMSIZE";      break;
    case SQLITE_IOERR_SHMLOCK:      zVal = "SQLITE_IOERR_SHMLOCK";      break;
    case SQLITE_LOCKED_SHAREDCACHE: zVal = "SQLITE_LOCKED_SHAREDCACHE"; break;
    case SQLITE_BUSY_RECOVERY:      zVal = "SQLITE_BUSY_RECOVERY";      break;
    case SQLITE_CANTOPEN_NOTEMPDIR: zVal = "SQLITE_CANTOPEN_NOTEMPDIR"; break;
    default: {
       sqlite3_snprintf(sizeof(zBuf), zBuf, "%d", rc);
       zVal = zBuf;
       break;
    }
  }
  vfstrace_printf(pInfo, zFormat, zVal);
}

/*
** Append to a buffer.
*/
static void strappend(char *z, int *pI, const char *zAppend){
  int i = *pI;
  while( zAppend[0] ){ z[i++] = *(zAppend++); }
  z[i] = 0;
  *pI = i;
}


/*
** Compression interface.
** Returns the output size in bytes.
*/
static int Compress(const void* input, int input_length, void* output, int max_output_length)
{
    int ret;
    int output_length;

    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    ret = deflateInit(&strm, CompressionLevel);
    if (ret != Z_OK)
    {
        return -1;
    }

    strm.avail_in = input_length;
    strm.next_in = (Bytef*)input;
    strm.avail_out = max_output_length;
    strm.next_out = (Bytef*)output;
    ret = deflate(&strm, Z_FINISH);    /* no bad return value */

    output_length = max_output_length - strm.avail_out;
    (void)deflateEnd(&strm);

    {
#if 0
        char dout[CHUNK_SIZE_BYTES];
        int dec = Decompress(output, CHUNK_SIZE_BYTES, dout, CHUNK_SIZE_BYTES);
        if (dec != input_length)
        {
            printf("ERROR: Decompression failure!\n");
            exit(1);
        }
#endif
    }

    return output_length;
}

/*
** Decompression interface.
** Returns the output size in bytes.
*/
static int Decompress(const void* input, int input_length, void* output, int max_output_length)
{
    int ret;
    unsigned output_length;
    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit(&strm);
    if (ret != Z_OK)
    {
        return -1;
    }

    strm.avail_in = input_length;
    strm.next_in = (Bytef*)input;
    strm.avail_out = max_output_length;
    strm.next_out = (Bytef*)output;
    ret = inflate(&strm, Z_NO_FLUSH);

    output_length = max_output_length - strm.avail_out;
    (void)inflateEnd(&strm);

    return output_length;
}

static DWORD SetSparseRange(HANDLE hSparseFile, LONGLONG start, LONGLONG size)
{
    typedef struct _FILE_ZERO_DATA_INFORMATION {

        LARGE_INTEGER FileOffset;
        LARGE_INTEGER BeyondFinalZero;

    } FILE_ZERO_DATA_INFORMATION, *PFILE_ZERO_DATA_INFORMATION;

    FILE_ZERO_DATA_INFORMATION fzdi;
    DWORD dwTemp;
    BOOL res;

    if (size <= 0)
    {
        return 0;
    }

    // Specify the starting and the ending address (not the size) of the
    // sparse zero block
    fzdi.FileOffset.QuadPart = start;
    fzdi.BeyondFinalZero.QuadPart = start + size;

#define FSCTL_SET_ZERO_DATA             CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 50, METHOD_BUFFERED, FILE_WRITE_DATA) // FILE_ZERO_DATA_INFORMATION,

    // Mark the range as sparse zero block
    SetLastError(0);
    res = DeviceIoControl(hSparseFile,
        FSCTL_SET_ZERO_DATA,
        &fzdi,
        sizeof(fzdi),
        NULL,
        0,
        &dwTemp,
        NULL);

    if (res)
    {
        return 0; //Sucess
    }

    // return the error value
    return GetLastError();
}

static int FlushChunk(vfstrace_file *pFile, vfsc_chunk *pCache)
{
    vfstrace_info *pInfo = pFile->pInfo;
    int rc = SQLITE_OK;
    if (pCache != NULL && pCache->origSize > 0 &&
        (pCache->state == uncompressed || pCache->state == unwritten))
    {
        if (pCache->state == uncompressed)
        {
            // Compress...
            pCache->compSize = Compress(pCache->pOrigData, pCache->origSize, pCache->pCompData, CHUNK_SIZE_BYTES);
            vfstrace_printf(pInfo, "Compressed %d into %d bytes from offset %lld.\n", pCache->origSize, pCache->compSize, pCache->offset);
        }

        // Write the chunk.
        vfstrace_printf(pInfo, "> %s.Flush(%s,n=%d,ofst=%lld)",
            pInfo->zVfsName, pFile->zFName, pCache->compSize, pCache->offset);
        vfstrace_printf(pInfo, "  Chunk=%lld, Data=%d bytes", pCache->offset, pCache->compSize);
        rc = pFile->pReal->pMethods->xWrite(pFile->pReal, pCache->pCompData, pCache->compSize, pCache->offset);
        vfstrace_print_errcode(pInfo, " -> %s\n", rc);

        SetSparseRange(pFile->hFile, pCache->offset + pCache->compSize, CHUNK_SIZE_BYTES - pCache->compSize);
        pCache->state = cached;
    }

    return rc;
}

static int FlushCache(vfstrace_file *pFile)
{
    vfstrace_info *pInfo = pFile->pInfo;

    //TODO: Iterate over the complete cache and flush each chunk.
    vfsc_chunk *pCache = pInfo->pCache;
    return FlushChunk(pFile, pCache);
}

static int ReadCache(vfstrace_file *pFile, int chunkOffset, vfsc_chunk* pChunk)
{
    int rc = pFile->pReal->pMethods->xRead(pFile->pReal, pChunk->pCompData, CHUNK_SIZE_BYTES, chunkOffset);
    if (rc == SQLITE_IOERR_READ || rc == SQLITE_FULL)
    {
        return rc;
    }

    if (pChunk->pCompData[0] == 0)
    {
        // The first byte should contain the length, hence can't be zero for compressed streams.
        pChunk->compSize = 0;
        pChunk->origSize = 0;
        //memset(pChunk->pCompData, 0, CHUNK_SIZE_BYTES);
    }
    else
    {
        pChunk->compSize = CHUNK_SIZE_BYTES; //TODO: Check if we read less.
        pChunk->origSize = Decompress(pChunk->pCompData, pChunk->compSize, pChunk->pOrigData, sizeof(pChunk->pOrigData));
        vfstrace_printf(pFile->pInfo, "> Decompressed %d bytes from offset %d.\n", pChunk->origSize, chunkOffset);
    }

    pChunk->offset = chunkOffset;
    pChunk->state = cached;
    memset(pChunk->pOrigData + pChunk->origSize, 0, CHUNK_SIZE_BYTES - pChunk->origSize);

    return rc;
}

/*
** Finds the chunk in cache or reads from disk.
*/
static int GetCache(vfstrace_file *pFile, int chunkOffset, vfsc_chunk** pChunk)
{
    int rc = 0;
    if (pFile->pInfo->pCache->offset != chunkOffset ||
        pFile->pInfo->pCache->state == empty)
    {
        // Flush current cache if necessary.
        FlushChunk(pFile, pFile->pInfo->pCache);

        // Not cached, read from disk and cache.
        rc = ReadCache(pFile, chunkOffset, pFile->pInfo->pCache);
    }

    *pChunk = pFile->pInfo->pCache;
    return rc;
}


/*
** Close an vfstrace-file.
*/
static int vfstraceClose(sqlite3_file *pFile){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;

  FlushCache(p);
  CloseHandle(p->hFile);
  p->hFile = 0;

  vfstrace_printf(pInfo, "%s.xClose(%s)", pInfo->zVfsName, p->zFName);
  rc = p->pReal->pMethods->xClose(p->pReal);
  vfstrace_print_errcode(pInfo, " -> %s\n", rc);
  if( rc==SQLITE_OK ){
    sqlite3_free((void*)p->base.pMethods);
    p->base.pMethods = 0;
  }
  return rc;
}

/*
** Read data from an vfstrace-file.
*/
static int vfstraceRead(
  sqlite3_file *pFile,
  void *zBuf,
  int iAmt,
  sqlite_int64 iOfst
){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc = 0;
  sqlite_int64 chunkOffset;

  if (p->hFile != 0)
  {
      chunkOffset = iOfst - (iOfst % CHUNK_SIZE_BYTES);
      rc = GetCache(p, chunkOffset, &pInfo->pCache);

      // Copy the data from the cache.
      //TODO: Check if the required data crosses chunk boundaries.
      memcpy(zBuf, pInfo->pCache->pOrigData + (iOfst % CHUNK_SIZE_BYTES), iAmt);

      vfstrace_printf(pInfo, "> %s.xRead(%s,n=%d,ofst=%lld)",
          pInfo->zVfsName, p->zFName, iAmt, iOfst);
      vfstrace_printf(pInfo, "  Chunk=%lld", chunkOffset);
      vfstrace_print_errcode(pInfo, " -> %s\n", rc);
  }
  else
  {
      vfstrace_printf(pInfo, "%s.xRead(%s,n=%d,ofst=%lld)",
          pInfo->zVfsName, p->zFName, iAmt, iOfst);
      rc = p->pReal->pMethods->xRead(p->pReal, zBuf, iAmt, iOfst);
      vfstrace_print_errcode(pInfo, " -> %s\n", rc);
  }

  return rc;
}

/*
** Write data to an vfstrace-file.
*/
static int vfstraceWrite(
  sqlite3_file *pFile,
  const void *zBuf,
  int iAmt,
  sqlite_int64 iOfst
){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc = SQLITE_OK;
  sqlite_int64 chunkOffset;

  if (p->hFile != 0)
  {
      // Get the cache chunk.
      int offsetInChunk = iOfst % CHUNK_SIZE_BYTES;
      chunkOffset = iOfst - offsetInChunk;
      GetCache(p, chunkOffset, &pInfo->pCache);

      // Write the new data.
      memcpy(pInfo->pCache->pOrigData + offsetInChunk, zBuf, iAmt);
      pInfo->pCache->state = uncompressed;
      pInfo->pCache->origSize = max(pInfo->pCache->origSize, offsetInChunk + iAmt);
      if (pInfo->pCache->origSize > CHUNK_SIZE_BYTES)
      {
          printf("ERROR: CHUNK OVERRUN!!!!\n");
          exit(1);
      }

#if 0
      // Compress...
      pInfo->pCache->compSize = Compress(pInfo->pCache->pOrigData, pInfo->pCache->origSize, pInfo->pCache->pCompData, CHUNK_SIZE_BYTES);
      vfstrace_printf(pInfo, "> Compressed %d into %d bytes from offset %lld.\n", pInfo->pCache->origSize, pInfo->pCache->compSize, chunkOffset);

      // Write the chunk.
      rc = p->pReal->pMethods->xWrite(p->pReal, pInfo->pCache->pCompData, pInfo->pCache->compSize, chunkOffset);
      SetSparseRange(p->hFile, chunkOffset + pInfo->pCache->compSize, CHUNK_SIZE_BYTES - pInfo->pCache->compSize);
#endif

      vfstrace_printf(pInfo, "> %s.xWrite(%s,n=%d,ofst=%lld)",
          pInfo->zVfsName, p->zFName, iAmt, iOfst);
      vfstrace_printf(pInfo, "  Chunk=%lld, Data=%d bytes", chunkOffset, pInfo->pCache->compSize);
      vfstrace_print_errcode(pInfo, " -> %s\n", rc);
  }
  else
  {
      vfstrace_printf(pInfo, "%s.xWrite(%s,n=%d,ofst=%lld)",
          pInfo->zVfsName, p->zFName, iAmt, iOfst);
      rc = p->pReal->pMethods->xWrite(p->pReal, zBuf, iAmt, iOfst);
      vfstrace_print_errcode(pInfo, " -> %s\n", rc);
  }

  return rc;
}

/*
** Truncate an vfstrace-file.
*/
static int vfstraceTruncate(sqlite3_file *pFile, sqlite_int64 size){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;
  vfstrace_printf(pInfo, "%s.xTruncate(%s,%lld)", pInfo->zVfsName, p->zFName,
                  size);
  rc = p->pReal->pMethods->xTruncate(p->pReal, size);
  vfstrace_printf(pInfo, " -> %d\n", rc);
  return rc;
}

/*
** Sync an vfstrace-file.
*/
static int vfstraceSync(sqlite3_file *pFile, int flags){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;
  int i;
  char zBuf[100];

  FlushCache(p);

  memcpy(zBuf, "|0", 3);
  i = 0;
  if( flags & SQLITE_SYNC_FULL )        strappend(zBuf, &i, "|FULL");
  else if( flags & SQLITE_SYNC_NORMAL ) strappend(zBuf, &i, "|NORMAL");
  if( flags & SQLITE_SYNC_DATAONLY )    strappend(zBuf, &i, "|DATAONLY");
  if( flags & ~(SQLITE_SYNC_FULL|SQLITE_SYNC_DATAONLY) ){
    sqlite3_snprintf(sizeof(zBuf)-i, &zBuf[i], "|0x%x", flags);
  }
  vfstrace_printf(pInfo, "%s.xSync(%s,%s)", pInfo->zVfsName, p->zFName,
                  &zBuf[1]);
  rc = p->pReal->pMethods->xSync(p->pReal, flags);
  vfstrace_printf(pInfo, " -> %d\n", rc);
  return rc;
}

/*
** Return the current file-size of an vfstrace-file.
*/
static int vfstraceFileSize(sqlite3_file *pFile, sqlite_int64 *pSize){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;
  vfstrace_printf(pInfo, "%s.xFileSize(%s)", pInfo->zVfsName, p->zFName);
  rc = p->pReal->pMethods->xFileSize(p->pReal, pSize);
  vfstrace_print_errcode(pInfo, " -> %s,", rc);
  vfstrace_printf(pInfo, " size=%lld\n", *pSize);
  return rc;
}

/*
** Return the name of a lock.
*/
static const char *lockName(int eLock){
  const char *azLockNames[] = {
     "NONE", "SHARED", "RESERVED", "PENDING", "EXCLUSIVE"
  };
  if( eLock<0 || eLock>=sizeof(azLockNames)/sizeof(azLockNames[0]) ){
    return "???";
  }else{
    return azLockNames[eLock];
  }
}

/*
** Lock an vfstrace-file.
*/
static int vfstraceLock(sqlite3_file *pFile, int eLock){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;
  vfstrace_printf(pInfo, "%s.xLock(%s,%s)", pInfo->zVfsName, p->zFName,
                  lockName(eLock));
  rc = p->pReal->pMethods->xLock(p->pReal, eLock);
  vfstrace_print_errcode(pInfo, " -> %s\n", rc);
  return rc;
}

/*
** Unlock an vfstrace-file.
*/
static int vfstraceUnlock(sqlite3_file *pFile, int eLock){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;
  vfstrace_printf(pInfo, "%s.xUnlock(%s,%s)", pInfo->zVfsName, p->zFName,
                  lockName(eLock));
  rc = p->pReal->pMethods->xUnlock(p->pReal, eLock);
  vfstrace_print_errcode(pInfo, " -> %s\n", rc);
  return rc;
}

/*
** Check if another file-handle holds a RESERVED lock on an vfstrace-file.
*/
static int vfstraceCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;
  vfstrace_printf(pInfo, "%s.xCheckReservedLock(%s,%d)",
                  pInfo->zVfsName, p->zFName);
  rc = p->pReal->pMethods->xCheckReservedLock(p->pReal, pResOut);
  vfstrace_print_errcode(pInfo, " -> %s", rc);
  vfstrace_printf(pInfo, ", out=%d\n", *pResOut);
  return rc;
}

/*
** File control method. For custom operations on an vfstrace-file.
*/
static int vfstraceFileControl(sqlite3_file *pFile, int op, void *pArg){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;
  char zBuf[100];
  char *zOp;
  switch( op ){
    case SQLITE_FCNTL_LOCKSTATE:    zOp = "LOCKSTATE";          break;
    case SQLITE_GET_LOCKPROXYFILE:  zOp = "GET_LOCKPROXYFILE";  break;
    case SQLITE_SET_LOCKPROXYFILE:  zOp = "SET_LOCKPROXYFILE";  break;
    case SQLITE_LAST_ERRNO:         zOp = "LAST_ERRNO";         break;
    case SQLITE_FCNTL_SIZE_HINT: {
      sqlite3_snprintf(sizeof(zBuf), zBuf, "SIZE_HINT,%lld",
                       *(sqlite3_int64*)pArg);
      zOp = zBuf;
      break;
    }
    case SQLITE_FCNTL_CHUNK_SIZE: {
      sqlite3_snprintf(sizeof(zBuf), zBuf, "CHUNK_SIZE,%d", *(int*)pArg);
      zOp = zBuf;
      break;
    }
    case SQLITE_FCNTL_FILE_POINTER: zOp = "FILE_POINTER";       break;
    case SQLITE_FCNTL_SYNC_OMITTED: {
        FlushCache(p);
        zOp = "SYNC_OMITTED";
        break;
    }
    case 0xca093fa0:                zOp = "DB_UNCHANGED";       break;
    default: {
      sqlite3_snprintf(sizeof zBuf, zBuf, "%d", op);
      zOp = zBuf;
      break;
    }
  }
  vfstrace_printf(pInfo, "%s.xFileControl(%s,%s)",
                  pInfo->zVfsName, p->zFName, zOp);
  rc = p->pReal->pMethods->xFileControl(p->pReal, op, pArg);
  vfstrace_print_errcode(pInfo, " -> %s\n", rc);
  return rc;
}

/*
** Return the sector-size in bytes for an vfstrace-file.
*/
static int vfstraceSectorSize(sqlite3_file *pFile){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;
  vfstrace_printf(pInfo, "%s.xSectorSize(%s)", pInfo->zVfsName, p->zFName);
  rc = p->pReal->pMethods->xSectorSize(p->pReal);
  vfstrace_printf(pInfo, " -> %d\n", rc);
  return rc;
}

/*
** Return the device characteristic flags supported by an vfstrace-file.
*/
static int vfstraceDeviceCharacteristics(sqlite3_file *pFile){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;
  vfstrace_printf(pInfo, "%s.xDeviceCharacteristics(%s)",
                  pInfo->zVfsName, p->zFName);
  rc = p->pReal->pMethods->xDeviceCharacteristics(p->pReal);
  vfstrace_printf(pInfo, " -> 0x%08x\n", rc);
  return rc;
}

/*
** Shared-memory operations.
*/
static int vfstraceShmLock(sqlite3_file *pFile, int ofst, int n, int flags){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;
  char zLck[100];
  int i = 0;
  memcpy(zLck, "|0", 3);
  if( flags & SQLITE_SHM_UNLOCK )    strappend(zLck, &i, "|UNLOCK");
  if( flags & SQLITE_SHM_LOCK )      strappend(zLck, &i, "|LOCK");
  if( flags & SQLITE_SHM_SHARED )    strappend(zLck, &i, "|SHARED");
  if( flags & SQLITE_SHM_EXCLUSIVE ) strappend(zLck, &i, "|EXCLUSIVE");
  if( flags & ~(0xf) ){
     sqlite3_snprintf(sizeof(zLck)-i, &zLck[i], "|0x%x", flags);
  }
  vfstrace_printf(pInfo, "%s.xShmLock(%s,ofst=%d,n=%d,%s)",
                  pInfo->zVfsName, p->zFName, ofst, n, &zLck[1]);
  rc = p->pReal->pMethods->xShmLock(p->pReal, ofst, n, flags);
  vfstrace_print_errcode(pInfo, " -> %s\n", rc);
  return rc;
}
static int vfstraceShmMap(
  sqlite3_file *pFile,
  int iRegion,
  int szRegion,
  int isWrite,
  void volatile **pp
){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;
  vfstrace_printf(pInfo, "%s.xShmMap(%s,iRegion=%d,szRegion=%d,isWrite=%d,*)",
                  pInfo->zVfsName, p->zFName, iRegion, szRegion, isWrite);
  rc = p->pReal->pMethods->xShmMap(p->pReal, iRegion, szRegion, isWrite, pp);
  vfstrace_print_errcode(pInfo, " -> %s\n", rc);
  return rc;
}
static void vfstraceShmBarrier(sqlite3_file *pFile){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  vfstrace_printf(pInfo, "%s.xShmBarrier(%s)\n", pInfo->zVfsName, p->zFName);
  p->pReal->pMethods->xShmBarrier(p->pReal);
}
static int vfstraceShmUnmap(sqlite3_file *pFile, int delFlag){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  int rc;
  vfstrace_printf(pInfo, "%s.xShmUnmap(%s,delFlag=%d)",
                  pInfo->zVfsName, p->zFName, delFlag);
  rc = p->pReal->pMethods->xShmUnmap(p->pReal, delFlag);
  vfstrace_print_errcode(pInfo, " -> %s\n", rc);
  return rc;
}

HANDLE MakeSparseFile(const char *zName)
{
    // Use CreateFile as you would normally - Create file with whatever flags
    //and File Share attributes that works for you
    DWORD dwTemp;
    DWORD res;
    void *zConverted;              /* Filename in OS encoding */
    HANDLE hSparseFile;

    /* Convert the filename to the system encoding. */
    zConverted = convertUtf8Filename(zName);
    if( zConverted==0 ){
        return INVALID_HANDLE_VALUE;
    }

    hSparseFile = CreateFileW((WCHAR*)zConverted,
        GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (hSparseFile == INVALID_HANDLE_VALUE)
    {
        return hSparseFile;
    }

    res = DeviceIoControl(hSparseFile,
                            FSCTL_SET_SPARSE,
                            NULL,
                            0,
                            NULL,
                            0,
                            &dwTemp,
                            NULL);
    return hSparseFile;
}


/*
** Open an vfstrace file handle.
*/
static int vfstraceOpen(
  sqlite3_vfs *pVfs,
  const char *zName,
  sqlite3_file *pFile,
  int flags,
  int *pOutFlags
){
  int rc;
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  p->pInfo = pInfo;
  p->zFName = zName ? fileTail(zName) : "<temp>";
  p->pReal = (sqlite3_file *)&p[1];
  rc = pRoot->xOpen(pRoot, zName, p->pReal, flags, pOutFlags);

  if ((flags & 0xFFFFFF00) == SQLITE_OPEN_MAIN_DB)
  {
      // Now reopen the file and mark it sparse.
      p->hFile = MakeSparseFile(zName);
      vfstrace_printf(pInfo, "> %s.xOpen(%s,flags=0x%x)",
          pInfo->zVfsName, p->zFName, flags);
  }
  else
  {
      vfstrace_printf(pInfo, "%s.xOpen(%s,flags=0x%x)",
          pInfo->zVfsName, p->zFName, flags);
  }

  if( p->pReal->pMethods ){
    sqlite3_io_methods *pNew = (sqlite3_io_methods*)sqlite3_malloc( sizeof(*pNew) );
    const sqlite3_io_methods *pSub = p->pReal->pMethods;
    memset(pNew, 0, sizeof(*pNew));
    pNew->iVersion = pSub->iVersion;
    pNew->xClose = vfstraceClose;
    pNew->xRead = vfstraceRead;
    pNew->xWrite = vfstraceWrite;
    pNew->xTruncate = vfstraceTruncate;
    pNew->xSync = vfstraceSync;
    pNew->xFileSize = vfstraceFileSize;
    pNew->xLock = vfstraceLock;
    pNew->xUnlock = vfstraceUnlock;
    pNew->xCheckReservedLock = vfstraceCheckReservedLock;
    pNew->xFileControl = vfstraceFileControl;
    pNew->xSectorSize = vfstraceSectorSize;
    pNew->xDeviceCharacteristics = vfstraceDeviceCharacteristics;
    if( pNew->iVersion>=2 ){
      pNew->xShmMap = pSub->xShmMap ? vfstraceShmMap : 0;
      pNew->xShmLock = pSub->xShmLock ? vfstraceShmLock : 0;
      pNew->xShmBarrier = pSub->xShmBarrier ? vfstraceShmBarrier : 0;
      pNew->xShmUnmap = pSub->xShmUnmap ? vfstraceShmUnmap : 0;
    }
    pFile->pMethods = pNew;
  }
  vfstrace_print_errcode(pInfo, " -> %s", rc);
  if( pOutFlags ){
    vfstrace_printf(pInfo, ", outFlags=0x%x\n", *pOutFlags);
  }else{
    vfstrace_printf(pInfo, "\n");
  }
  return rc;
}

/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int vfstraceDelete(sqlite3_vfs *pVfs, const char *zPath, int dirSync){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  int rc;
  vfstrace_printf(pInfo, "%s.xDelete(\"%s\",%d)",
                  pInfo->zVfsName, zPath, dirSync);
  rc = pRoot->xDelete(pRoot, zPath, dirSync);
  vfstrace_print_errcode(pInfo, " -> %s\n", rc);
  return rc;
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int vfstraceAccess(
  sqlite3_vfs *pVfs,
  const char *zPath,
  int flags,
  int *pResOut
){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  int rc;
  vfstrace_printf(pInfo, "%s.xDelete(\"%s\",%d)",
                  pInfo->zVfsName, zPath, flags);
  rc = pRoot->xAccess(pRoot, zPath, flags, pResOut);
  vfstrace_print_errcode(pInfo, " -> %s", rc);
  vfstrace_printf(pInfo, ", out=%d\n", *pResOut);
  return rc;
}

/*
** Populate buffer zOut with the full canonical pathname corresponding
** to the pathname in zPath. zOut is guaranteed to point to a buffer
** of at least (DEVSYM_MAX_PATHNAME+1) bytes.
*/
static int vfstraceFullPathname(
  sqlite3_vfs *pVfs,
  const char *zPath,
  int nOut,
  char *zOut
){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  int rc;
  vfstrace_printf(pInfo, "%s.xFullPathname(\"%s\")",
                  pInfo->zVfsName, zPath);
  rc = pRoot->xFullPathname(pRoot, zPath, nOut, zOut);
  vfstrace_print_errcode(pInfo, " -> %s", rc);
  vfstrace_printf(pInfo, ", out=\"%.*s\"\n", nOut, zOut);
  return rc;
}

/*
** Open the dynamic library located at zPath and return a handle.
*/
static void *vfstraceDlOpen(sqlite3_vfs *pVfs, const char *zPath){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  vfstrace_printf(pInfo, "%s.xDlOpen(\"%s\")\n", pInfo->zVfsName, zPath);
  return pRoot->xDlOpen(pRoot, zPath);
}

/*
** Populate the buffer zErrMsg (size nByte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated
** with dynamic libraries.
*/
static void vfstraceDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  vfstrace_printf(pInfo, "%s.xDlError(%d)", pInfo->zVfsName, nByte);
  pRoot->xDlError(pRoot, nByte, zErrMsg);
  vfstrace_printf(pInfo, " -> \"%s\"", zErrMsg);
}

/*
** Return a pointer to the symbol zSymbol in the dynamic library pHandle.
*/
static void (*vfstraceDlSym(sqlite3_vfs *pVfs,void *p,const char *zSym))(void){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  vfstrace_printf(pInfo, "%s.xDlSym(\"%s\")\n", pInfo->zVfsName, zSym);
  return pRoot->xDlSym(pRoot, p, zSym);
}

/*
** Close the dynamic library handle pHandle.
*/
static void vfstraceDlClose(sqlite3_vfs *pVfs, void *pHandle){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  vfstrace_printf(pInfo, "%s.xDlOpen()\n", pInfo->zVfsName);
  pRoot->xDlClose(pRoot, pHandle);
}

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of
** random data.
*/
static int vfstraceRandomness(sqlite3_vfs *pVfs, int nByte, char *zBufOut){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  vfstrace_printf(pInfo, "%s.xRandomness(%d)\n", pInfo->zVfsName, nByte);
  return pRoot->xRandomness(pRoot, nByte, zBufOut);
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds
** actually slept.
*/
static int vfstraceSleep(sqlite3_vfs *pVfs, int nMicro){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xSleep(pRoot, nMicro);
}

/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
static int vfstraceCurrentTime(sqlite3_vfs *pVfs, double *pTimeOut){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xCurrentTime(pRoot, pTimeOut);
}
static int vfstraceCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *pTimeOut){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xCurrentTimeInt64(pRoot, pTimeOut);
}

/*
** Return th3 emost recent error code and message
*/
static int vfstraceGetLastError(sqlite3_vfs *pVfs, int iErr, char *zErr){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xGetLastError(pRoot, iErr, zErr);
}

/*
** Override system calls.
*/
static int vfstraceSetSystemCall(
  sqlite3_vfs *pVfs,
  const char *zName,
  sqlite3_syscall_ptr pFunc
){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xSetSystemCall(pRoot, zName, pFunc);
}
static sqlite3_syscall_ptr vfstraceGetSystemCall(
  sqlite3_vfs *pVfs,
  const char *zName
){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xGetSystemCall(pRoot, zName);
}
static const char *vfstraceNextSystemCall(sqlite3_vfs *pVfs, const char *zName){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xNextSystemCall(pRoot, zName);
}


/*
** Clients invoke this routine to construct a new trace-vfs shim.
**
** Return SQLITE_OK on success.
**
** SQLITE_NOMEM is returned in the case of a memory allocation error.
** SQLITE_NOTFOUND is returned if zOldVfsName does not exist.
*/
SQLITE_API int vfscompress_register(
   int trace,                  /* True to trace operations to stderr */
   int compressionLevel        /* The compression level: -1 for default, 1 fastest, 9 best */
){
  sqlite3_vfs *pNew;
  sqlite3_vfs *pRoot;
  vfstrace_info *pInfo;
  int nName;
  int nByte;

  CompressionLevel = compressionLevel;

  // Find the windows VFS.
  pRoot = sqlite3_vfs_find("win32");
  if( pRoot==0 ) return SQLITE_NOTFOUND;
  nName = strlen("vfscompress");
  nByte = sizeof(*pNew) + sizeof(*pInfo) + nName + 1;
  pNew = (sqlite3_vfs*)sqlite3_malloc( nByte );
  if( pNew==0 ) return SQLITE_NOMEM;
  memset(pNew, 0, nByte);
  pInfo = (vfstrace_info*)&pNew[1];
  pNew->iVersion = pRoot->iVersion;
  pNew->szOsFile = pRoot->szOsFile + sizeof(vfstrace_file);
  pNew->mxPathname = pRoot->mxPathname;
  pNew->zName = (char*)&pInfo[1];
  memcpy((char*)&pInfo[1], "vfscompress", nName+1);
  pNew->pAppData = pInfo;
  pNew->xOpen = vfstraceOpen;
  pNew->xDelete = vfstraceDelete;
  pNew->xAccess = vfstraceAccess;
  pNew->xFullPathname = vfstraceFullPathname;
  pNew->xDlOpen = pRoot->xDlOpen==0 ? 0 : vfstraceDlOpen;
  pNew->xDlError = pRoot->xDlError==0 ? 0 : vfstraceDlError;
  pNew->xDlSym = pRoot->xDlSym==0 ? 0 : vfstraceDlSym;
  pNew->xDlClose = pRoot->xDlClose==0 ? 0 : vfstraceDlClose;
  pNew->xRandomness = vfstraceRandomness;
  pNew->xSleep = vfstraceSleep;
  pNew->xCurrentTime = vfstraceCurrentTime;
  pNew->xGetLastError = pRoot->xGetLastError==0 ? 0 : vfstraceGetLastError;
  if( pNew->iVersion>=2 ){
    pNew->xCurrentTimeInt64 = pRoot->xCurrentTimeInt64==0 ? 0 :
                                   vfstraceCurrentTimeInt64;
    if( pNew->iVersion>=3 ){
      pNew->xSetSystemCall = pRoot->xSetSystemCall==0 ? 0 :
                                   vfstraceSetSystemCall;
      pNew->xGetSystemCall = pRoot->xGetSystemCall==0 ? 0 :
                                   vfstraceGetSystemCall;
      pNew->xNextSystemCall = pRoot->xNextSystemCall==0 ? 0 :
                                   vfstraceNextSystemCall;
    }
  }
  pInfo->pRootVfs = pRoot;
  pInfo->xOut = (int(*)(const char*,void*))fputs;
  pInfo->pOutArg = stderr;
  pInfo->zVfsName = pNew->zName;
  pInfo->pTraceVfs = pNew;
  pInfo->trace = trace;

  pInfo->pCache = (vfsc_chunk*)sqlite3_malloc(sizeof(vfsc_chunk));
  memset(pInfo->pCache, 0, sizeof(vfsc_chunk));
  if( pInfo->pCache==0 ) return SQLITE_NOMEM;
  pInfo->pCache->origSize = -1;

  if (!pInfo->trace)
  {
      pInfo->trace = 1;
      vfstrace_printf(pInfo, "%s.enabled_for(\"%s\")\n",
          pInfo->zVfsName, pRoot->zName);
  }

  pInfo->trace = trace;
  return sqlite3_vfs_register(pNew, 1);
}