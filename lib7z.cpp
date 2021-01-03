#include <string>
#include <errno.h>
#ifdef __ANDROID__
#include <android/log.h>
#define APP_TAG "LIB"
#define Log_Debug(APP_TAG, ...)    __android_log_print(ANDROID_LOG_DEBUG,   APP_TAG, __VA_ARGS__)
#define Log_Info(APP_TAG, ...)     __android_log_print(ANDROID_LOG_INFO,    APP_TAG, __VA_ARGS__)
#define Log_Error(APP_TAG, ...)    __android_log_print(ANDROID_LOG_ERROR,   APP_TAG, __VA_ARGS__)
#else
#define Log_Debug(APP_TAG, ...)    printf(__VA_ARGS__)
#define Log_Info(APP_TAG, ...)     printf(__VA_ARGS__)
#define Log_Error(APP_TAG, ...)    printf(__VA_ARGS__)
#endif

#include <stdio.h>
#include <string.h>
#include "Compiler.h"

#include "7zTypes.h"
#include "7zFile.h"
#include "7zAlloc.h"
#include "7z.h"
#include "7zCrc.h"
#include "7zBuf.h"

#include "lib7z.h"

#ifndef XXX_USE_WINDOWS_FILE
/* for mkdir */
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <errno.h>
#endif
#endif

#define MY_STDAPI int MY_STD_CALL
#define kInputBufSize ((size_t)1 << 18)
typedef enum taga7zErrorCode_ {
	a7z_OK = 0,
	a7z_Err_Open_Failed = 1,
} a7zErrorCode;


static const ISzAlloc g_Alloc = { SzAlloc, SzFree };

static void UInt64ToStr(UInt64 value, char *s, int numDigits)
{
	char temp[32];
	int pos = 0;
	do
	{
		temp[pos++] = (char)('0' + (unsigned)(value % 10));
		value /= 10;
	} while (value != 0);

	for (numDigits -= pos; numDigits > 0; numDigits--)
		*s++ = ' ';

	do
		*s++ = temp[--pos];
	while (pos);
	*s = '\0';
}


static int Buf_EnsureSize(CBuf *dest, size_t size)
{
	if (dest->size >= size)
		return 1;
	Buf_Free(dest, &g_Alloc);
	return Buf_Create(dest, size, &g_Alloc);
}

#define _USE_UTF8

#define _UTF8_START(n) (0x100 - (1 << (7 - (n))))

#define _UTF8_RANGE(n) (((UInt32)1) << ((n) * 5 + 6))

#define _UTF8_HEAD(n, val) ((Byte)(_UTF8_START(n) + (val >> (6 * (n)))))
#define _UTF8_CHAR(n, val) ((Byte)(0x80 + (((val) >> (6 * (n))) & 0x3F)))


static size_t Utf16_To_Utf8_Calc(const UInt16 *src, const UInt16 *srcLim)
{
	size_t size = 0;
	for (;;)
	{
		UInt32 val;
		if (src == srcLim)
			return size;

		size++;
		val = *src++;

		if (val < 0x80)
			continue;

		if (val < _UTF8_RANGE(1))
		{
			size++;
			continue;
		}

		if (val >= 0xD800 && val < 0xDC00 && src != srcLim)
		{
			UInt32 c2 = *src;
			if (c2 >= 0xDC00 && c2 < 0xE000)
			{
				src++;
				size += 3;
				continue;
			}
		}

		size += 2;
	}
}

static Byte *Utf16_To_Utf8(Byte *dest, const UInt16 *src, const UInt16 *srcLim)
{
	for (;;)
	{
		UInt32 val;
		if (src == srcLim)
			return dest;

		val = *src++;

		if (val < 0x80)
		{
			*dest++ = (char)val;
			continue;
		}

		if (val < _UTF8_RANGE(1))
		{
			dest[0] = _UTF8_HEAD(1, val);
			dest[1] = _UTF8_CHAR(0, val);
			dest += 2;
			continue;
		}

		if (val >= 0xD800 && val < 0xDC00 && src != srcLim)
		{
			UInt32 c2 = *src;
			if (c2 >= 0xDC00 && c2 < 0xE000)
			{
				src++;
				val = (((val - 0xD800) << 10) | (c2 - 0xDC00)) + 0x10000;
				dest[0] = _UTF8_HEAD(3, val);
				dest[1] = _UTF8_CHAR(2, val);
				dest[2] = _UTF8_CHAR(1, val);
				dest[3] = _UTF8_CHAR(0, val);
				dest += 4;
				continue;
			}
		}

		dest[0] = _UTF8_HEAD(2, val);
		dest[1] = _UTF8_CHAR(1, val);
		dest[2] = _UTF8_CHAR(0, val);
		dest += 3;
	}
}

static SRes Utf16_To_Utf8Buf(CBuf *dest, const UInt16 *src, size_t srcLen)
{
	size_t destLen = Utf16_To_Utf8_Calc(src, src + srcLen);
	destLen += 1;
	if (!Buf_EnsureSize(dest, destLen))
		return SZ_ERROR_MEM;
	*Utf16_To_Utf8(dest->data, src, src + srcLen) = 0;
	return SZ_OK;
}

//#ifdef _WIN32
//#ifndef XXX_USE_WINDOWS_FILE
//static UINT g_FileCodePage = CP_ACP;
//#endif
//#define MY_FILE_CODE_PAGE_PARAM ,g_FileCodePage
//#else
//#define MY_FILE_CODE_PAGE_PARAM
//#endif

#define MY_FILE_CODE_PAGE_PARAM 

static SRes Utf16_To_Char(CBuf *buf, const UInt16 *s
	)
{
	unsigned len = 0;
	for (len = 0; s[len] != 0; len++);
	return Utf16_To_Utf8Buf(buf, s, len);
}

static WRes MyCreateDir(const UInt16 *name)
{

	CBuf buf;
	WRes res;
	Buf_Init(&buf);
	RINOK(Utf16_To_Char(&buf, name MY_FILE_CODE_PAGE_PARAM));

	res =
#ifdef _WIN32
		_mkdir((const char *)buf.data)
#else
		mkdir((const char *)buf.data, 0777)
#endif
		== 0 ? 0 : errno;
	Buf_Free(&buf, &g_Alloc);
	return res;
}

static WRes MyCreateDirByChar(const char *name)
{
    WRes res;
    res =
#ifdef _WIN32
_mkdir((const char *)buf.data)
#else
mkdir((const char *)name, 0777)
#endif
== 0 ? 0 : errno;
    return res;
}

static WRes OutFile_OpenUtf16(CSzFile *p, const UInt16 *name)
{
	CBuf buf;
	WRes res;
	Buf_Init(&buf);
	RINOK(Utf16_To_Char(&buf, name MY_FILE_CODE_PAGE_PARAM));
	res = OutFile_Open(p, (const char *)buf.data);
	Buf_Free(&buf, &g_Alloc);
	return res;
}


static char *UIntToStr(char *s, unsigned value, int numDigits)
{
	char temp[16];
	int pos = 0;
	do
		temp[pos++] = (char)('0' + (value % 10));
	while (value /= 10);

	for (numDigits -= pos; numDigits > 0; numDigits--)
		*s++ = '0';

	do
		*s++ = temp[--pos];
	while (pos);
	*s = '\0';
	return s;
}

static void UIntToStr_2(char *s, unsigned value)
{
	s[0] = (char)('0' + (value / 10));
	s[1] = (char)('0' + (value % 10));
}

#define PERIOD_4 (4 * 365 + 1)
#define PERIOD_100 (PERIOD_4 * 25 - 1)
#define PERIOD_400 (PERIOD_100 * 4 + 1)

static void ConvertFileTimeToString(const CNtfsFileTime *nt, char *s)
{
	unsigned year, mon, hour, min, sec;
	Byte ms[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	unsigned t;
	UInt32 v;
	UInt64 v64 = nt->Low | ((UInt64)nt->High << 32);
	v64 /= 10000000;
	sec = (unsigned)(v64 % 60); v64 /= 60;
	min = (unsigned)(v64 % 60); v64 /= 60;
	hour = (unsigned)(v64 % 24); v64 /= 24;

	v = (UInt32)v64;

	year = (unsigned)(1601 + v / PERIOD_400 * 400);
	v %= PERIOD_400;

	t = v / PERIOD_100; if (t == 4) t = 3; year += t * 100; v -= t * PERIOD_100;
	t = v / PERIOD_4;   if (t == 25) t = 24; year += t * 4;   v -= t * PERIOD_4;
	t = v / 365;        if (t == 4) t = 3; year += t;       v -= t * 365;

	if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
		ms[1] = 29;
	for (mon = 0;; mon++)
	{
		unsigned d = ms[mon];
		if (v < d)
			break;
		v -= d;
	}
	s = UIntToStr(s, year, 4); *s++ = '-';
	UIntToStr_2(s, mon + 1); s[2] = '-'; s += 3;
	UIntToStr_2(s, (unsigned)v + 1); s[2] = ' '; s += 3;
	UIntToStr_2(s, hour); s[2] = ':'; s += 3;
	UIntToStr_2(s, min); s[2] = ':'; s += 3;
	UIntToStr_2(s, sec); s[2] = 0;
}


static void GetAttribString(UInt32 wa, Bool isDir, char *s)
{
	s[0] = (char)(((wa & (1 << 4)) != 0 || isDir) ? 'D' : '.');
	s[1] = 0;
}


DLL_API int a7zList(const char *inputFile, int *numFiles)
{
	ISzAlloc allocImp;
	ISzAlloc allocTempImp;

	CFileInStream archiveStream;
	CLookToRead2 lookStream;
	CSzArEx db;
	SRes res;
	size_t tempSize = 0;

	allocImp = g_Alloc;
	allocTempImp = g_Alloc;

	if (InFile_Open(&archiveStream.file, inputFile))
	{
		return a7z_Err_Open_Failed;
	}

	FileInStream_CreateVTable(&archiveStream);
	LookToRead2_CreateVTable(&lookStream, False);
	lookStream.buf = NULL;

	res = SZ_OK;
	{
		lookStream.buf = (Byte*)ISzAlloc_Alloc(&allocImp, kInputBufSize);
		if (!lookStream.buf) {
        res = SZ_ERROR_MEM;
    }
		else
		{
			lookStream.bufSize = kInputBufSize;
			lookStream.realStream = &archiveStream.vt;
			LookToRead2_Init(&lookStream);
		}
	}

	CrcGenerateTable();
	SzArEx_Init(&db);

	if (res == SZ_OK)
	{
		res = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp);
	}

	if (res == SZ_OK)
	{
		*numFiles = db.NumFiles;

		SzArEx_Free(&db, &allocImp);
		ISzAlloc_Free(&allocImp, lookStream.buf);

		File_Close(&archiveStream.file);

		return a7z_OK;
	}
	return res;
}


static SRes Utf16ToAnsiChar(const UInt16 *s, char *outBuffer, int *iOutBuffLen)
{
	CBuf buf;
	SRes res;
	Buf_Init(&buf);
	res = Utf16_To_Char(&buf, s);
	if (res == SZ_OK) {
		int iBuffLen = strlen((const char *)buf.data);
		int iCopyBuff = iBuffLen;
		if (iCopyBuff > *iOutBuffLen) {
			iCopyBuff = *iOutBuffLen;
		}
		else {
			*iOutBuffLen = iCopyBuff;
		}
		memcpy(outBuffer, (const char *)buf.data, iCopyBuff);
	}
	Buf_Free(&buf, &g_Alloc);
	return res;
}

// Min buffer length 256
DLL_API int a7zFileNameByIndex(const char *inputFile, const int fileIndex, char *outputFileName, int *iBufferLen, int *bIsDir)
{
	ISzAlloc allocImp;
	ISzAlloc allocTempImp;
	CFileInStream archiveStream;
	CLookToRead2 lookStream;
	CSzArEx db;
	SRes res;
	UInt16 *temp = NULL;
	size_t tempSize = 0;
	allocImp = g_Alloc;
	allocTempImp = g_Alloc;

	if (InFile_Open(&archiveStream.file, inputFile))
	{
		return a7z_Err_Open_Failed;
	}
	FileInStream_CreateVTable(&archiveStream);
	LookToRead2_CreateVTable(&lookStream, False);
	lookStream.buf = NULL;
	res = SZ_OK;
	{
		lookStream.buf = (Byte*)ISzAlloc_Alloc(&allocImp, kInputBufSize);
		if (!lookStream.buf) {
            res = SZ_ERROR_MEM;
        }
		else {
			lookStream.bufSize = kInputBufSize;
			lookStream.realStream = &archiveStream.vt;
			LookToRead2_Init(&lookStream);
		}
	}
	CrcGenerateTable();
	SzArEx_Init(&db);
	if (res == SZ_OK) {
		res = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp);
	}
	if (res == SZ_OK) {
		int listCommand = 1, fullPaths = 0;
		fullPaths = 1;
		if (res == SZ_OK)
		{
			UInt32 i;
			UInt32 blockIndex = 0xFFFFFFFF; /* it can have any value before first call (if outBuffer = 0) */
			Byte *outBuffer = 0; /* it must be 0 before first call for each new archive. */
			size_t outBufferSize = 0;  /* it can have any value before first call (if outBuffer = 0) */
			for (i = 0; i < db.NumFiles && fileIndex >= 0 && fileIndex < db.NumFiles; i++)
			{
				size_t len;
				unsigned isDir = SzArEx_IsDir(&db, i);
				if (listCommand == 0 && isDir && !fullPaths)
					continue;
				len = SzArEx_GetFileNameUtf16(&db, i, NULL);
				if (len > tempSize) {
					SzFree(NULL, temp);
					tempSize = len;
					temp = (UInt16 *)SzAlloc(NULL, tempSize * sizeof(temp[0]));
					if (!temp) {
						res = SZ_ERROR_MEM;
						break;
					}
				}
				SzArEx_GetFileNameUtf16(&db, i, temp);
				if (listCommand)
				{
					if (fileIndex != i) {
						continue;
					}
					char attr[8], s[32], t[32];
					UInt64 fileSize;
					GetAttribString(SzBitWithVals_Check(&db.Attribs, i) ? db.Attribs.Vals[i] : 0, isDir, attr);
					fileSize = SzArEx_GetFileSize(&db, i);
					UInt64ToStr(fileSize, s, 10);
					if (SzBitWithVals_Check(&db.MTime, i)) {
						ConvertFileTimeToString(&db.MTime.Vals[i], t);
					}
					else {
						size_t j;
						for (j = 0; j < 19; j++)
							t[j] = ' ';
						t[j] = '\0';
					}
					res = Utf16ToAnsiChar(temp, outputFileName, iBufferLen);
					*bIsDir = isDir;
					break;
				}
			}
		}
	}
	SzFree(NULL, temp);
	SzArEx_Free(&db, &allocImp);
	ISzAlloc_Free(&allocImp, lookStream.buf);
	File_Close(&archiveStream.file);
	if (res == SZ_OK) {
		return 0;
	}
	return 1;
}


DLL_API int a7zUncompress(const char *inputFile, const char *outputDir)
{
	ISzAlloc allocImp;
	ISzAlloc allocTempImp;

	CFileInStream archiveStream;
	CLookToRead2 lookStream;
	CSzArEx db;
	SRes res;
	UInt16 *temp = NULL;
	size_t tempSize = 0;

	allocImp = g_Alloc;
	allocTempImp = g_Alloc;

	if (InFile_Open(&archiveStream.file, inputFile))
	{
		return a7z_Err_Open_Failed;
	}

	FileInStream_CreateVTable(&archiveStream);
	LookToRead2_CreateVTable(&lookStream, False);
	lookStream.buf = NULL;

	res = SZ_OK;

	{
		lookStream.buf = (Byte*)ISzAlloc_Alloc(&allocImp, kInputBufSize);
		if (!lookStream.buf)
			res = SZ_ERROR_MEM;
		else
		{
			lookStream.bufSize = kInputBufSize;
			lookStream.realStream = &archiveStream.vt;
			LookToRead2_Init(&lookStream);
		}
	}

	CrcGenerateTable();

	SzArEx_Init(&db);

	if (res == SZ_OK)
	{
		res = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp);
	}

	if (res == SZ_OK)
	{
		int listCommand = 0,  fullPaths = 0;
		fullPaths = 1;


		if (res == SZ_OK)
		{
			UInt32 i;

			/*
			if you need cache, use these 3 variables.
			if you use external function, you can make these variable as static.
			*/
			UInt32 blockIndex = 0xFFFFFFFF; /* it can have any value before first call (if outBuffer = 0) */
			Byte *outBuffer = 0; /* it must be 0 before first call for each new archive. */
			size_t outBufferSize = 0;  /* it can have any value before first call (if outBuffer = 0) */

			for (i = 0; i < db.NumFiles; i++)
			{
				size_t offset = 0;
				size_t outSizeProcessed = 0;
				size_t len;
				unsigned isDir = SzArEx_IsDir(&db, i);
				if (listCommand == 0 && isDir && !fullPaths) {
                    continue;
                }
				len = SzArEx_GetFileNameUtf16(&db, i, NULL);
				if (len > tempSize)
				{
					SzFree(NULL, temp);
					tempSize = len;
					temp = (UInt16 *)SzAlloc(NULL, tempSize * sizeof(temp[0]));
					if (!temp)
					{
						res = SZ_ERROR_MEM;
						break;
					}
				}
				SzArEx_GetFileNameUtf16(&db, i, temp);
				if (!isDir) {
					res = SzArEx_Extract(&db, &lookStream.vt, i,
						&blockIndex, &outBuffer, &outBufferSize,
						&offset, &outSizeProcessed,
						&allocImp, &allocTempImp);
					if (res != SZ_OK)
						break;
				}

				{
					CSzFile outFile;
					size_t processedSize;
					size_t j;
					UInt16 *name = (UInt16 *)temp;
					const UInt16 *destPath = (const UInt16 *)name;

					for (j = 0; name[j] != 0; j++) {
                        if (name[j] == '/') {
                            if (fullPaths) {
                                name[j] = 0;
                                MyCreateDir(name);
                                name[j] = CHAR_PATH_SEPARATOR;
                            } else
                                destPath = name + j + 1;
                        }
                    }

					int iBufferLen = 128;
          char outputFileName[128] = {0x00};
          int tmpRes = Utf16ToAnsiChar(destPath, outputFileName, &iBufferLen);
          std::string strDestFullPath(outputDir);
          strDestFullPath.append("//");
          strDestFullPath.append(outputFileName);
					if (isDir)
					{
            MyCreateDirByChar(strDestFullPath.c_str());
						continue;
					}
					else if (OutFile_Open(&outFile, strDestFullPath.c_str()))
					{
						res = SZ_ERROR_FAIL;
						break;
					}
					processedSize = outSizeProcessed;
					if (File_Write(&outFile, outBuffer + offset, &processedSize) != 0 || processedSize != outSizeProcessed)
					{
						res = SZ_ERROR_FAIL;
						break;
					}
					if (File_Close(&outFile))
					{
						res = SZ_ERROR_FAIL;
						break;
					}
				}
			}
			ISzAlloc_Free(&allocImp, outBuffer);
		}
	}
	SzFree(NULL, temp);
	SzArEx_Free(&db, &allocImp);
	ISzAlloc_Free(&allocImp, lookStream.buf);
	File_Close(&archiveStream.file);
	if (res == SZ_OK)
	{
		return 0;
	}
	return res;
}

