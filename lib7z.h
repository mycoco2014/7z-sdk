#ifndef __7Z_LIB_HEADER__
#define __7Z_LIB_HEADER__

#ifdef _WIN32
#define DLL_API extern "C" __declspec(dllexport)
#else
#define DLL_API extern "C" 
#endif

#ifdef __cplusplus
extern "C" {
#endif

DLL_API int a7zList(const char *inputFile, int *numFiles);

DLL_API int a7zFileNameByIndex(const char *inputFile, const int fileIndex, char *outputFileName, int *iBufferLen, int *bIsDir);

DLL_API int a7zUncompress(const char *inputFile, const char *outputDir);

#ifdef __cplusplus
}
#endif

#endif
