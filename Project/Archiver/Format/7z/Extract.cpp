// Extract.cpp

#include "StdAfx.h"

#include "Handler.h"
#include "Util/MultiStream.h"
#include "FolderOutStream.h"

#include "RegistryInfo.h"

#include "Interface/StreamObjects.h"
#include "Interface/ProgressUtils.h"
#include "Interface/LimitedStreams.h"

#include "Decode.h"

#include "Windows/COMTry.h"

using namespace NArchive;
using namespace N7z;

struct CExtractFolderInfo
{
  int FileIndex;
  int FolderIndex;
  CBoolVector ExtractStatuses;
  UINT64 UnPackSize;
  CExtractFolderInfo(int fileIndex, int folderIndex): 
    FileIndex(fileIndex),
    FolderIndex(folderIndex), 
    UnPackSize(0) 
  {
    if (fileIndex >= 0)
    {
      ExtractStatuses.Reserve(1);
      ExtractStatuses.Add(true);
    }
  };
};

STDMETHODIMP CHandler::Extract(const UINT32* indices, UINT32 numItems,
    INT32 testModeSpec, IExtractCallback200 *extractCallBackSpec)
{
  COM_TRY_BEGIN
  bool testMode = (testModeSpec != 0);
  CComPtr<IExtractCallback200> extractCallback = extractCallBackSpec;
  UINT64 importantTotalUnPacked = 0, importantTotalPacked = 0;
  UINT64 censoredTotalUnPacked = 0, censoredTotalPacked = 0;
  if(numItems == 0)
    return S_OK;

  CObjectVector<CExtractFolderInfo> extractFolderInfoVector;
  for(UINT64 indexIndex = 0; indexIndex < numItems; indexIndex++)
  {
    int fileIndex = indices[indexIndex];
    int folderIndex = _database.FileIndexToFolderIndexMap[fileIndex];
    if (folderIndex < 0)
    {
      extractFolderInfoVector.Add(CExtractFolderInfo(fileIndex, -1));
      continue;
    }
    if (extractFolderInfoVector.IsEmpty() || 
        folderIndex != extractFolderInfoVector.Back().FolderIndex)
      extractFolderInfoVector.Add(CExtractFolderInfo(-1, folderIndex));
    CExtractFolderInfo &extractFolderInfo = extractFolderInfoVector.Back();

    // const CFolderInfo &folderInfo = m_dam_Folders[folderIndex];
    UINT32 startIndex = _database.FolderStartFileIndex[folderIndex];
    for (UINT64 index = extractFolderInfo.ExtractStatuses.Size();
        index <= fileIndex - startIndex; index++)
    {
      UINT64 unPackSize = _database.Files[startIndex + index].UnPackSize;
      extractFolderInfo.UnPackSize += unPackSize;
      importantTotalUnPacked += unPackSize;
      extractFolderInfo.ExtractStatuses.Add(index == fileIndex - startIndex);
    }
  }

  extractCallback->SetTotal(importantTotalUnPacked);

  CDecoder decoder;

  UINT64 currentImportantTotalUnPacked = 0;
  UINT64 totalFolderUnPacked;

  for(int i = 0; i < extractFolderInfoVector.Size(); i++, 
      currentImportantTotalUnPacked += totalFolderUnPacked)
  {
    CExtractFolderInfo &extractFolderInfo = extractFolderInfoVector[i];
    totalFolderUnPacked = extractFolderInfo.UnPackSize;

    RETURN_IF_NOT_S_OK(extractCallback->SetCompleted(&currentImportantTotalUnPacked));

    CComObjectNoLock<CFolderOutStream> *aFolderOutStream = 
      new CComObjectNoLock<CFolderOutStream>;
    CComPtr<ISequentialOutStream> outStream(aFolderOutStream);

    UINT32 startIndex;
    if (extractFolderInfo.FileIndex >= 0)
      startIndex = extractFolderInfo.FileIndex;
    else
      startIndex = _database.FolderStartFileIndex[extractFolderInfo.FolderIndex];


    RETURN_IF_NOT_S_OK(aFolderOutStream->Init(&_database, startIndex, 
        &extractFolderInfo.ExtractStatuses, extractCallback, testMode));

    if (extractFolderInfo.FileIndex >= 0)
      continue;

    UINT32 folderIndex = extractFolderInfo.FolderIndex;
    const CFolderItemInfo &folderInfo = _database.Folders[folderIndex];

    CObjectVector< CComPtr<ISequentialInStream> > inStreams;

    CLockedInStream lockedInStream;
    lockedInStream.Init(_inStream);


    UINT64 folderStartPackStreamIndex = _database.FolderStartPackStreamIndex[folderIndex];

    for (int j = 0; j < folderInfo.PackStreams.Size(); j++)
    {
      const CPackStreamInfo &packStreamInfo = folderInfo.PackStreams[j];
      CComObjectNoLock<CLockedSequentialInStreamImp> *lockedStreamImpSpec = new 
        CComObjectNoLock<CLockedSequentialInStreamImp>;
      CComPtr<ISequentialInStream> lockedStreamImp = lockedStreamImpSpec;
      UINT64 streamStartPos = _database.GetFolderStreamPos(folderIndex, j);
      lockedStreamImpSpec->Init(&lockedInStream, streamStartPos);

      CComObjectNoLock<CLimitedSequentialInStream> *streamSpec = new 
        CComObjectNoLock<CLimitedSequentialInStream>;
      CComPtr<ISequentialInStream> inStream = streamSpec;
      streamSpec->Init(lockedStreamImp, 
          _database.PackSizes[folderStartPackStreamIndex + j]);
      inStreams.Add(inStream);
    }

    CComObjectNoLock<CLocalProgress> *localProgressSpec = new  CComObjectNoLock<CLocalProgress>;
    CComPtr<ICompressProgressInfo> progress = localProgressSpec;
    localProgressSpec->Init(extractCallback, false);

    CComObjectNoLock<CLocalCompressProgressInfo> *localCompressProgressSpec = 
      new  CComObjectNoLock<CLocalCompressProgressInfo>;
    CComPtr<ICompressProgressInfo> compressProgress = localCompressProgressSpec;
    localCompressProgressSpec->Init(progress, NULL, &currentImportantTotalUnPacked);

    UINT32 packStreamIndex = _database.FolderStartPackStreamIndex[folderIndex];
    UINT64 folderStartPackPos = _database.GetFolderStreamPos(folderIndex, 0);

    try
    {
      HRESULT result = decoder.Decode(_inStream,
          folderStartPackPos, 
          &_database.PackSizes[packStreamIndex],
          folderInfo,
          outStream,
          compressProgress);

      if (result == S_FALSE)
        throw "data error";
      if (result != S_OK)
        return result;
      RETURN_IF_NOT_S_OK(aFolderOutStream->WasWritingFinished());
    }
    catch(...)
    {
      RETURN_IF_NOT_S_OK(aFolderOutStream->FlushCorrupted());
      continue;
    }
  }
  return S_OK;
  COM_TRY_END
}

STDMETHODIMP CHandler::ExtractAllItems(INT32 testMode,
      IExtractCallback200 *extractCallback)
{
  COM_TRY_BEGIN
  CRecordVector<UINT32> indices;
  indices.Reserve(_database.Files.Size());
  for(int i = 0; i < _database.Files.Size(); i++)
    indices.Add(i);
  return Extract(&indices.Front(), indices.Size(), testMode,
      extractCallback);
  COM_TRY_END
}
