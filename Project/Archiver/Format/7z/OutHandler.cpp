// 7z/OutHandler.cpp

#include "StdAfx.h"

#include "Handler.h"
#include "OutEngine.h"
#include "Common/StringConvert.h"
#include "UpdateMain.h"

#include "Windows/PropVariant.h"
#include "Windows/Time.h"
#include "Windows/COMTry.h"

#include "RegistryInfo.h"

using namespace NArchive;
using namespace N7z;

using namespace NWindows;
using namespace NCOM;
using namespace NTime;

#ifdef COMPRESS_LZMA
static NArchive::N7z::CMethodID k_LZMA = { { 0x3, 0x1, 0x1 }, 3 };
#endif

#ifdef COMPRESS_PPMD
static NArchive::N7z::CMethodID k_PPMD = { { 0x3, 0x4, 0x1 }, 3 };
#endif

#ifdef COMPRESS_BCJ_X86
static NArchive::N7z::CMethodID k_BCJ_X86 = { { 0x3, 0x3, 0x1, 0x3 }, 4 };
#endif

#ifdef COMPRESS_BCJ2
static NArchive::N7z::CMethodID k_BCJ2 = { { 0x3, 0x3, 0x1, 0x1B }, 4 };
#endif

#ifdef COMPRESS_COPY
static NArchive::N7z::CMethodID k_Copy = { { 0x0 }, 1 };
#endif

#ifdef COMPRESS_DEFLATE
static NArchive::N7z::CMethodID k_Deflate = { { 0x4, 0x1, 0x8 }, 3 };
#endif

#ifdef COMPRESS_BZIP2
static NArchive::N7z::CMethodID k_BZip2 = { { 0x4, 0x2, 0x2 }, 3 };
#endif

const char *kLZMAMethodName = "LZMA";
// const char *kDeflateMethodName = "Deflate";

const UINT32 kAlgorithmForX = (2);
const UINT32 kDicSizeForX = (1 << 22);
const UINT32 kFastBytesForX = (64);

const UINT32 kAlgorithmForFast = (0);
const UINT32 kDicSizeForFast = (1 << 15);

const char *kDefaultMethodName = kLZMAMethodName;

// const char *kDefaultMatchFinder = "BT4";
const char *kDefaultMatchFinderForFast = "HC3";

static bool IsLZMAMethod(const AString &methodName)
{
  return (methodName.CompareNoCase(kLZMAMethodName) == 0);
}

static bool IsLZMethod(const AString &methodName)
{
  return (IsLZMAMethod(methodName) 
      // || methodName.CompareNoCase(kDeflateMethodName) == 0
      );
}

STDMETHODIMP CHandler::GetFileTimeType(UINT32 *type)
{
  *type = NFileTimeType::kWindows;
  return S_OK;
}


// it's work only fopr non-solid archives

STDMETHODIMP CHandler::DeleteItems(IOutStream *outStream, 
    const UINT32* indices, UINT32 numItems, IUpdateCallBack *updateCallback)
{
  COM_TRY_BEGIN
  CRecordVector<bool> compressStatuses;
  CRecordVector<UINT32> copyIndexes;
  int index = 0;
  int i;
  for(i = 0; i < _database.NumUnPackStreamsVector.Size(); i++)
  {
    if (_database.NumUnPackStreamsVector[i] != 1)
      return E_NOTIMPL;
  }
  for(i = 0; i < _database.Files.Size(); i++)
  {
    // bool copyMode = true;
    if(index < numItems && i == indices[index])
      index++;
    else
    {
      compressStatuses.Add(false);
      copyIndexes.Add(i);
    }
  }
  CCompressionMethodMode methodMode, headerMethod;
  RETURN_IF_NOT_S_OK(SetCompressionMethod(methodMode, headerMethod));
  methodMode.MultiThread = _multiThread;
  methodMode.MultiThreadMult = _multiThreadMult;
  headerMethod.MultiThread = _multiThread;
  headerMethod.MultiThreadMult = _multiThreadMult;

  UpdateMain(_database, compressStatuses,
      CObjectVector<CUpdateItemInfo>(), copyIndexes,
      outStream, _inStream, &_database.ArchiveInfo, 
      NULL, (_compressHeaders ? &headerMethod: 0), 
      updateCallback, false);
  return S_OK;
  COM_TRY_END
}

struct CNameToPropID
{
  PROPID PropID;
  VARTYPE VarType;
  const char *Name;
  bool CoderProperties;
};

CNameToPropID g_NameToPropID[] = 
{
  { NEncodedStreamProperies::kOrder, VT_UI4, "O", true  },
  { NEncodedStreamProperies::kPosStateBits, VT_UI4, "PB", true  },
  { NEncodedStreamProperies::kLitContextBits, VT_UI4, "LC", true  },
  { NEncodedStreamProperies::kLitPosBits, VT_UI4, "LP", true  },

  { NEncodingProperies::kNumPasses, VT_UI4, "Pass", false },
  { NEncodingProperies::kNumFastBytes, VT_UI4, "fb", false },
  { NEncodingProperies::kAlgorithm, VT_UI4, "a", false }
};

bool ConvertProperty(PROPVARIANT srcProp, VARTYPE varType, 
    NCOM::CPropVariant &destProp)
{
  if (varType == srcProp.vt)
  {
    destProp = srcProp;
    return true;
  }
  if (varType == VT_UI1)
  {
    if(srcProp.vt == VT_UI4)
    {
      UINT32 value = srcProp.ulVal;
      if (value > 0xFF)
        return false;
      destProp = BYTE(value);
      return true;
    }
  }
  return false;
}
    
const kNumNameToPropIDItems = sizeof(g_NameToPropID) / sizeof(g_NameToPropID[0]);

int FindPropIdFromStringName(const AString &name)
{
  for (int i = 0; i < kNumNameToPropIDItems; i++)
    if (name.CompareNoCase(g_NameToPropID[i].Name) == 0)
      return i;
  return -1;
}

HRESULT CHandler::SetCompressionMethod(CCompressionMethodMode &methodMode,
    CCompressionMethodMode &headerMethod)
{
  #ifndef EXCLUDE_COM
  CObjectVector<NRegistryInfo::CMethodInfo2> methodInfoVector;
  if (!NRegistryInfo::EnumerateAllMethods(methodInfoVector))
    return E_FAIL;
  #endif
 

  if (_methods.IsEmpty())
  {
    COneMethodInfo oneMethodInfo;
    oneMethodInfo.MethodName = kDefaultMethodName;
    oneMethodInfo.MatchFinderIsDefined = false;
    _methods.Add(oneMethodInfo);
  }

  for(int i = 0; i < _methods.Size(); i++)
  {
    COneMethodInfo &oneMethodInfo = _methods[i];
    if (oneMethodInfo.MethodName.IsEmpty())
      oneMethodInfo.MethodName = kDefaultMethodName;

    if (IsLZMethod(oneMethodInfo.MethodName))
    {
      if (!oneMethodInfo.MatchFinderIsDefined)
      {
        oneMethodInfo.MatchFinderName = GetSystemString(_matchFinder);
        oneMethodInfo.MatchFinderIsDefined = true;
      }
      if (IsLZMAMethod(oneMethodInfo.MethodName))
      {
        int j;
        for (j = 0; j < oneMethodInfo.CoderProperties.Size(); j++)
          if (oneMethodInfo.CoderProperties[j].PropID == NEncodedStreamProperies::kDictionarySize)
            break;
        if (j == oneMethodInfo.CoderProperties.Size())
        {
          CProperty property;
          property.PropID = NEncodedStreamProperies::kDictionarySize;
          property.Value = _defaultDicSize;
          oneMethodInfo.CoderProperties.Add(property);
        }
        for (j = 0; j < oneMethodInfo.EncoderProperties.Size(); j++)
          if (oneMethodInfo.EncoderProperties[j].PropID == NEncodingProperies::kAlgorithm)
            break;
        if (j == oneMethodInfo.EncoderProperties.Size())
        {
          CProperty property;
          property.PropID = NEncodingProperies::kAlgorithm;
          property.Value = _defaultAlgorithm;
          oneMethodInfo.EncoderProperties.Add(property);
        }
        for (j = 0; j < oneMethodInfo.EncoderProperties.Size(); j++)
          if (oneMethodInfo.EncoderProperties[j].PropID == NEncodingProperies::kNumFastBytes)
            break;
        if (j == oneMethodInfo.EncoderProperties.Size())
        {
          CProperty property;
          property.PropID = NEncodingProperies::kNumFastBytes;
          property.Value = (UINT32)_defaultFastBytes;
          oneMethodInfo.EncoderProperties.Add(property);
        }
      }
    }
    CMethodFull methodFull;
    methodFull.MethodInfoEx.NumInStreams = 1;
    methodFull.MethodInfoEx.NumOutStreams = 1;

    bool defined = false;

    #ifdef COMPRESS_LZMA
    if (oneMethodInfo.MethodName.CompareNoCase("LZMA") == 0)
    {
      defined = true;
      methodFull.MethodInfoEx.MethodID = k_LZMA;
    }
    #endif

    #ifdef COMPRESS_PPMD
    if (oneMethodInfo.MethodName.CompareNoCase("PPMD") == 0)
    {
      defined = true;
      methodFull.MethodInfoEx.MethodID = k_PPMD;
    }
    #endif

    #ifdef COMPRESS_BCJ_X86
    if (oneMethodInfo.MethodName.CompareNoCase("BCJ") == 0)
    {
      defined = true;
      methodFull.MethodInfoEx.MethodID = k_BCJ_X86;
    }
    #endif

    #ifdef COMPRESS_BCJ2
    if (oneMethodInfo.MethodName.CompareNoCase("BCJ2") == 0)
    {
      defined = true;
      methodFull.MethodInfoEx.MethodID = k_BCJ2;
      methodFull.MethodInfoEx.NumInStreams = 4;
      methodFull.MethodInfoEx.NumOutStreams = 1;
    }
    #endif

    #ifdef COMPRESS_DEFLATE
    if (oneMethodInfo.MethodName.CompareNoCase("Deflate") == 0)
    {
      defined = true;
      methodFull.MethodInfoEx.MethodID = k_Deflate;
    }
    #endif

    #ifdef COMPRESS_BZIP2
    if (oneMethodInfo.MethodName.CompareNoCase("BZip2") == 0)
    {
      defined = true;
      methodFull.MethodInfoEx.MethodID = k_BZip2;
    }
    #endif

    #ifdef COMPRESS_COPY
    if (oneMethodInfo.MethodName.CompareNoCase("Copy") == 0)
    {
      defined = true;
      methodFull.MethodInfoEx.MethodID = k_Copy;
    }

    #endif
    
    #ifdef EXCLUDE_COM
    
    if (defined)
    {
  
      methodFull.CoderProperties = oneMethodInfo.CoderProperties;
      methodFull.EncoderProperties = oneMethodInfo.EncoderProperties;
      methodFull.MatchFinderIsDefined = oneMethodInfo.MatchFinderIsDefined;
      methodFull.MatchFinderName = oneMethodInfo.MatchFinderName;
      methodMode.Methods.Add(methodFull);
      continue;
    }
    
    #else

    int j;
    for (j = 0; j < methodInfoVector.Size(); j++)
      if (methodInfoVector[j].Name.CompareNoCase(oneMethodInfo.MethodName) == 0)
        break;
    if (j == methodInfoVector.Size())
      return E_FAIL;
    const NRegistryInfo::CMethodInfo2 &methodInfo = methodInfoVector[j];
    if (!methodInfo.EncoderIsAssigned)
      return E_FAIL;

    methodFull.MethodInfoEx.MethodID = methodInfo.MethodID;
    methodFull.MethodInfoEx.NumInStreams = methodInfo.NumInStreams;
    methodFull.MethodInfoEx.NumOutStreams = methodInfo.NumOutStreams;

    methodFull.EncoderClassID = methodInfo.Encoder;
    methodFull.CoderProperties = oneMethodInfo.CoderProperties;
    methodFull.EncoderProperties = oneMethodInfo.EncoderProperties;
    methodFull.MatchFinderIsDefined = oneMethodInfo.MatchFinderIsDefined;
    if (oneMethodInfo.MatchFinderIsDefined)
    {
      NRegistryInfo::CMatchFinderInfo matchFinderInfo;
      if (!NRegistryInfo::GetMatchFinder(oneMethodInfo.MatchFinderName, matchFinderInfo))
        return E_INVALIDARG;
      methodFull.MatchFinderClassID = matchFinderInfo.ClassID;
    }
    defined = true;
    
    #endif
    if (!defined)
      return E_FAIL;
    
    methodMode.Methods.Add(methodFull);
  }
  methodMode.Binds = _binds;
  if (_compressHeaders)
    headerMethod.Methods.Add(methodMode.Methods.Back());
  return S_OK;
}

STDMETHODIMP CHandler::UpdateItems(IOutStream *outStream, UINT32 numItems,
    IUpdateCallBack *updateCallback)
{
  COM_TRY_BEGIN

  CRecordVector<bool> compressStatuses;
  CObjectVector<CUpdateItemInfo> updateItems;
  CRecordVector<UINT32> copyIndexes;
  
  CComPtr<IUpdateCallBack2> updateCallback2;
  updateCallback->QueryInterface(&updateCallback2);

  int index = 0;
  for(int i = 0; i < numItems; i++)
  {
    CUpdateItemInfo updateItemInfo;
    INT32 compress;
    INT32 existInArchive;
    INT32 indexInServer;
    CComBSTR name;
    bool isAnti;
    if (updateCallback2)
    {
      INT32 _anIsAnti;
      RETURN_IF_NOT_S_OK(updateCallback2->GetUpdateItemInfo2(i,
        &compress, // 1 - compress 0 - copy
        &existInArchive,
        &indexInServer,
        &updateItemInfo.Attributes,
        &updateItemInfo.CreationTime,
        NULL,
        &updateItemInfo.LastWriteTime,
        &updateItemInfo.Size, 
        &name,
        &_anIsAnti));
        isAnti = MyBoolToBool(_anIsAnti);
    }
    else
    {
      RETURN_IF_NOT_S_OK(updateCallback->GetUpdateItemInfo(i,
        &compress, // 1 - compress 0 - copy
        &existInArchive,
        &indexInServer,
        &updateItemInfo.Attributes,
        &updateItemInfo.CreationTime,
        NULL,
        &updateItemInfo.LastWriteTime,
        &updateItemInfo.Size, 
        &name));
      isAnti = false;
    }
    if (MyBoolToBool(compress))
    {
      updateItemInfo.IsAnti = isAnti;
      updateItemInfo.SetDirectoryStatusFromAttributes();

      if (name)
        updateItemInfo.Name = name;

      updateItemInfo.AttributesAreDefined = true;
      updateItemInfo.CreationTimeIsDefined = true;
      updateItemInfo.LastWriteTimeIsDefined = true;

      updateItemInfo.IndexInClient = i;

      if (isAnti)
      {
        updateItemInfo.AttributesAreDefined = false;
        updateItemInfo.CreationTimeIsDefined = false;
        updateItemInfo.LastWriteTimeIsDefined = false;
        updateItemInfo.Size = 0;
        if (MyBoolToBool(existInArchive) && !name)
        {
          const CFileItemInfo &item = _database.Files[indexInServer];
          updateItemInfo.Name = _database.Files[indexInServer].Name;
          updateItemInfo.IsDirectory = item.IsDirectory;
        }
      }

      if(MyBoolToBool(existInArchive))
      {
        // const CFolderInfo &aFolderInfo = m_Folders[indexInServer];
        updateItemInfo.Commented = false;
        if(updateItemInfo.Commented)
        {
          // updateItemInfo.CommentRange.Position = itemInfo.GetCommentPosition();
          // updateItemInfo.CommentRange.Size  = itemInfo.CommentSize;
        }
      }
      else
        updateItemInfo.Commented = false;
      compressStatuses.Add(true);
      updateItems.Add(updateItemInfo);
    }
    else
    {
      compressStatuses.Add(false);
      copyIndexes.Add(indexInServer);
    }
  }

  if (!copyIndexes.IsEmpty())
    for(int i = 0; i < _database.NumUnPackStreamsVector.Size(); i++)
      if (_database.NumUnPackStreamsVector[i] != 1)
        return E_NOTIMPL;

  CCompressionMethodMode methodMode, headerMethod;
  RETURN_IF_NOT_S_OK(SetCompressionMethod(methodMode, headerMethod));
  methodMode.MultiThread = _multiThread;
  methodMode.MultiThreadMult = _multiThreadMult;
  headerMethod.MultiThread = _multiThread;
  headerMethod.MultiThreadMult = _multiThreadMult;

  NArchive::N7z::CInArchiveInfo *inArchiveInfo;
  if (!_inStream)
    inArchiveInfo = 0;
  else
    inArchiveInfo = &_database.ArchiveInfo;

  return UpdateMain(_database, compressStatuses,
      updateItems, copyIndexes, outStream, _inStream, inArchiveInfo, 
      &methodMode, _compressHeaders ? &headerMethod: 0, 
      updateCallback, _solid);
  COM_TRY_END
}

static const kMaxNumberOfDigitsInInputNumber = 9;

static int ParseNumberString(const AString &srcString, int &number)
{
  AString numberString;
  int i = 0;
  for(; i < srcString.Length() && i < kMaxNumberOfDigitsInInputNumber; i++)
  {
    char c = srcString[i];
    if(!isdigit(c) && (c != '-' || i > 0))
      break;
    numberString += c;
  }
  if (i > 0)
    number = atoi(numberString);
  return i;
}

static UINT32 ParseUINT32String(const AString &srcString, UINT32 &number)
{
  int tempNumber;
  int pos = ParseNumberString(srcString, tempNumber);
  if (pos <= 0)
    return pos;
  if (tempNumber < 0)
    return 0;
  number = tempNumber;
  return pos;
}

static const kLogarithmicSizeLimit = 32;

static const char kByteSymbol = 'B';
static const char kKiloByteSymbol = 'K';
static const char kMegaByteSymbol = 'M';

HRESULT ParseDictionaryValues(const AString &srcStringSpec, 
    BYTE &logDicSize, UINT32 &dicSize)
{
  AString srcString = srcStringSpec;
  int number;
  srcString.MakeUpper();
  int numDigits = ParseNumberString(srcString, number);
  if (numDigits == 0 || srcString.Length() > numDigits + 1)
    return E_FAIL;
  if (srcString.Length() == numDigits)
  {
    if (number >= kLogarithmicSizeLimit)
      return E_INVALIDARG;
    logDicSize = number;
    dicSize = 1 << number;
    return S_OK;
  }
  switch (srcString[numDigits])
  {
  case kByteSymbol:
    /*
    if (number > (UINT32(1) << kMaxLogarithmicSize))
      return E_INVALIDARG;
    */
    dicSize = number;
    break;
  case kKiloByteSymbol:
    if (number >= (1 << (kLogarithmicSizeLimit - 10)))
      return E_INVALIDARG;
    dicSize = number << 10;
    break;
  case kMegaByteSymbol:
    if (number >= (1 << (kLogarithmicSizeLimit - 20)))
      return E_INVALIDARG;
    dicSize = number << 20;
    break;
  default:
    return E_INVALIDARG;
  }
  int i;
  for (i = 0; i < kLogarithmicSizeLimit; i++)
    if (dicSize <= (1 << i))
      break;
  logDicSize = i;
  return S_OK;
}

static inline UINT GetCurrentFileCodePage()
{
  return AreFileApisANSI() ? CP_ACP : CP_OEMCP;
}

static HRESULT SetBoolProperty(bool &dest, const PROPVARIANT &value)
{
  switch(value.vt)
  {
    case VT_EMPTY:
      dest = true;
      break;
    /*
    case VT_UI4:
      dest = (value.ulVal != 0);
      break;
    */
    case VT_BSTR:
    {
      UString valueString = value.bstrVal;
      valueString.MakeUpper();
      if (valueString.Compare(L"ON") == 0)
        dest = true;
      else if (valueString.Compare(L"OFF") == 0)
        dest = false;
      else
        return E_INVALIDARG;
      break;
    }
    default:
      return E_INVALIDARG;
  }
  return S_OK;
}

/*
static HRESULT SetComplexProperty(bool &boolStatus, UINT32 &number, 
    const PROPVARIANT &value)
{
  switch(value.vt)
  {
    case VT_EMPTY:
    case VT_BSTR:
    {
      RETURN_IF_NOT_S_OK(SetBoolProperty(boolStatus, value));
      return S_OK;
    }
    case VT_UI4:
      boolStatus = true;
      number = (value.ulVal);
      break;
    default:
      return E_INVALIDARG;
  }
  return S_OK;
}
*/

static HRESULT GetBindInfoPart(AString &srcString, UINT32 &coder, UINT32 &stream)
{
  stream = 0;
  int index = ParseUINT32String(srcString, coder);
  if (index == 0)
    return E_INVALIDARG;
  srcString.Delete(0, index);
  if (srcString[0] == 'S')
  {
    srcString.Delete(0);
    int index = ParseUINT32String(srcString, stream);
    if (index == 0)
      return E_INVALIDARG;
    srcString.Delete(0, index);
  }
  return S_OK;
}

static HRESULT GetBindInfo(AString &srcString, CBind &bind)
{
  RETURN_IF_NOT_S_OK(GetBindInfoPart(srcString, bind.OutCoder, bind.OutStream));
  if (srcString[0] != ':')
    return E_INVALIDARG;
  srcString.Delete(0);
  RETURN_IF_NOT_S_OK(GetBindInfoPart(srcString, bind.InCoder, bind.InStream));
  if (!srcString.IsEmpty())
    return E_INVALIDARG;
  return S_OK;
}

static void SplitParams(const UString &srcString, UStringVector &subStrings)
{
  subStrings.Clear();
  UString name;
  int len = srcString.Length();
  if (len == 0)
    return;
  for (int i = 0; i < len; i++)
  {
    wchar_t c = srcString[i];
    if (c == L':')
    {
      subStrings.Add(name);
      name.Empty();
    }
    else
      name += c;
  }
  subStrings.Add(name);
}

static void SplitParam(const UString &param, UString &name, UString &value)
{
  int eqPos = param.Find(L'=');
  if (eqPos >= 0)
  {
    name = param.Left(eqPos);
    value = param.Mid(eqPos + 1);
    return;
  }
  for(int i = 0; i < param.Length(); i++)
  {
    wchar_t c = param[i];
    if (c >= L'0' && c <= L'9')
    {
      name = param.Left(i);
      value = param.Mid(i);
      return;
    }
  }
  name = param;
}

static bool ParseNumberString(const UString &aString, UINT32 &aNumber)
{
  wchar_t *anEndPtr;
  aNumber = wcstoul(aString, &anEndPtr, 10);
  return (anEndPtr - aString == aString.Length());
}


HRESULT CHandler::SetParam(COneMethodInfo &oneMethodInfo, const UString &name, const UString &value)
{
  if (name.CompareNoCase(L"MF") == 0)
  {
    oneMethodInfo.MatchFinderIsDefined = true;
    oneMethodInfo.MatchFinderName = GetSystemString(value);
  }
  else
  {
    CProperty property;
    if (name.CompareNoCase(L"D") == 0 || name.CompareNoCase(L"MEM") == 0)
    {
      BYTE logDicSize;
      UINT32 dicSize;
      RETURN_IF_NOT_S_OK(ParseDictionaryValues(UnicodeStringToMultiByte(value), 
          logDicSize, dicSize));
      if (name.CompareNoCase(L"D") == 0)
        property.PropID = NEncodedStreamProperies::kDictionarySize;
      else
        property.PropID = NEncodedStreamProperies::kUsedMemorySize;
      property.Value = dicSize;
      oneMethodInfo.CoderProperties.Add(property);
    }
    else
    {
      int index = FindPropIdFromStringName(UnicodeStringToMultiByte(name));
      if (index < 0)
        return E_INVALIDARG;
      
      const CNameToPropID &nameToPropID = g_NameToPropID[index];
      property.PropID = nameToPropID.PropID;

      NCOM::CPropVariant propValue;

      UINT32 number;
      if (ParseNumberString(value, number))
        propValue = number;
      else
        propValue = value;
      
      if (!ConvertProperty(propValue, nameToPropID.VarType, property.Value))
        return E_INVALIDARG;
      
      if (nameToPropID.CoderProperties)
        oneMethodInfo.CoderProperties.Add(property);
      else
        oneMethodInfo.EncoderProperties.Add(property);
    }
  }
  return S_OK;
}

HRESULT CHandler::SetParams(COneMethodInfo &oneMethodInfo, const UString &srcString)
{
  UStringVector params;
  SplitParams(srcString, params);
  if (params.Size() > 0)
    oneMethodInfo.MethodName = UnicodeStringToMultiByte(params[0]);
  for (int i = 1; i < params.Size(); i++)
  {
    const UString &param = params[i];
    UString name, value;
    SplitParam(param, name, value);
    RETURN_IF_NOT_S_OK(SetParam(oneMethodInfo, name, value));
  }
  return S_OK;
}


STDMETHODIMP CHandler::SetProperties(const BSTR *names, const PROPVARIANT *values, INT32 numProperties)
{
  UINT codePage = GetCurrentFileCodePage();
  COM_TRY_BEGIN
  _methods.Clear();
  _binds.Clear();
  Init();
  int minNumber = 0;

  for (int i = 0; i < numProperties; i++)
  {
    AString name = UnicodeStringToMultiByte(UString(names[i]));
    name.MakeUpper();

    const PROPVARIANT &value = values[i];

    if (name.CompareNoCase("0") == 0 || 
        name.CompareNoCase("1") == 0 || 
        name.CompareNoCase("X") == 0)
    {
      if (value.vt == VT_EMPTY)
      {
        if (name.CompareNoCase("X") == 0)
        {
          _defaultAlgorithm = kAlgorithmForX;
          _defaultDicSize = kDicSizeForX;
          _defaultFastBytes = kFastBytesForX;
        }
        else if (name.CompareNoCase("0") == 0)
        {
          _defaultAlgorithm = kAlgorithmForFast;
          _matchFinder = kDefaultMatchFinderForFast;
          _defaultDicSize = kDicSizeForFast;
        }
        continue;
      }
    }

    if (name.IsEmpty())
      return E_INVALIDARG;
    if (name[0] == 'B')
    {
      name.Delete(0);
      CBind bind;
      RETURN_IF_NOT_S_OK(GetBindInfo(name, bind));
      _binds.Add(bind);
      continue;
    }

      
    int number;
    int index = ParseNumberString(name, number);
    AString realName = name.Mid(index);
    if (index == 0)
    {
      if (name.CompareNoCase("S") == 0)
      {
        RETURN_IF_NOT_S_OK(SetBoolProperty(_solid, value));
        continue;
      }
      else if (name.CompareNoCase("HC") == 0)
      {
        RETURN_IF_NOT_S_OK(SetBoolProperty(_compressHeaders, value));
        continue;
      }
      else if (name.CompareNoCase("MT") == 0)
      {
        _multiThreadMult = 200;
        RETURN_IF_NOT_S_OK(SetBoolProperty(_multiThread, value));
        // RETURN_IF_NOT_S_OK(SetComplexProperty(MultiThread, _multiThreadMult, value));
        continue;
      }
      number = 0;
    }
    if (number > 100)
      return E_FAIL;
    if (number < minNumber)
    {
      /*
      for (int i = number; i < minNumber; i++)
      {
        COneMethodInfo oneMethodInfo;
        oneMethodInfo.MatchFinderIsDefined = false;
        _methods.Insert(0, oneMethodInfo);
      }
      minNumber = number;
      */
      return E_INVALIDARG;
    }
    number -= minNumber;
    for(int j = _methods.Size(); j <= number; j++)
    {
      COneMethodInfo oneMethodInfo;
      oneMethodInfo.MatchFinderIsDefined = false;
      _methods.Add(oneMethodInfo);
    }

    COneMethodInfo &oneMethodInfo = _methods[number];

    if (realName.Length() == 0)
    {
      if (value.vt != VT_BSTR)
        return E_INVALIDARG;
      
      // oneMethodInfo.MethodName = UnicodeStringToMultiByte(UString(value.bstrVal));
      RETURN_IF_NOT_S_OK(SetParams(oneMethodInfo, value.bstrVal));
    }
    else if (realName.CompareNoCase("MF") == 0)
    {
      // if (value.vt != VT_UI4)
      if (value.vt != VT_BSTR)
        return E_INVALIDARG;
      oneMethodInfo.MatchFinderIsDefined = true;
      // oneMethodInfo.MatchFinderIndex = value.ulVal;
      oneMethodInfo.MatchFinderName = GetSystemString(value.bstrVal);
    }
    else
    {
      CProperty property;
      if (realName.CompareNoCase("D") == 0 || realName.CompareNoCase("MEM") == 0)
      {
        BYTE logDicSize;
        UINT32 dicSize;
        if (value.vt == VT_UI4)
        {
          logDicSize = value.ulVal;
          dicSize = 1 << logDicSize;
        }
        else if (value.vt == VT_BSTR)
        {
          RETURN_IF_NOT_S_OK(ParseDictionaryValues(UnicodeStringToMultiByte(value.bstrVal), 
              logDicSize, dicSize));
        }
        else 
          return E_FAIL;
        if (realName.CompareNoCase("D") == 0)
          property.PropID = NEncodedStreamProperies::kDictionarySize;
        else
          property.PropID = NEncodedStreamProperies::kUsedMemorySize;
        property.Value = dicSize;
        oneMethodInfo.CoderProperties.Add(property);
      }
      else
      {
        int index = FindPropIdFromStringName(realName);
        if (index < 0)
          return E_INVALIDARG;
        
        const CNameToPropID &nameToPropID = g_NameToPropID[index];
        property.PropID = nameToPropID.PropID;
        
        if (!ConvertProperty(value, nameToPropID.VarType, property.Value))
          return E_INVALIDARG;
        
        if (nameToPropID.CoderProperties)
          oneMethodInfo.CoderProperties.Add(property);
        else
          oneMethodInfo.EncoderProperties.Add(property);
      }
    }
  }

  return S_OK;
  COM_TRY_END
}  

