#include <stdio.h>
#include <string.h>
#include <math.h>
#include "dnscache.h"
#include "dnsparser.h"
#include "dnsgenerator.h"
#include "utils.h"
#include "querydnsbase.h"
#include "rwlock.h"
#include "hashtable.h"

#define	CACHE_INIT_COUNT	128
#define	CACHE_VERSION		2

#define	CACHE_END	'\x0A'
#define	CACHE_START	'\xFF'

static BOOL				Inited = FALSE;

static RWLock			CacheLock;

static FileHandle		CacheFileHandle;
static MappingHandle	CacheMappingHandle;
static char				*MapStart;

static ThreadHandle		TTLMinusOne_Thread;

static _32BIT_INT		CacheSize;
static int				ForceTTL;
static int				TTLMultiple;

struct _CacheEntry{
	_32BIT_INT	Offset;
	_32BIT_UINT	TTL;
	_32BIT_UINT	OriginalTTL;
	_32BIT_UINT	Length;
};

static volatile _32BIT_INT	*CacheEnd; /* Offset */

static HashTable	*CacheInfo;

struct _Header{
	_32BIT_UINT	Ver;
	_32BIT_INT	CacheSize;
	_32BIT_INT	End;
	HashTable	ht;
	char		Comment[128 - sizeof(_32BIT_UINT) - sizeof(_32BIT_INT) - sizeof(_32BIT_INT) - sizeof(HashTable)];
};

static void DNSCacheTTLMinusOne_Thread(void)
{
	register	int			loop;
	register	BOOL		GotMutex	=	FALSE;
	register	int			cc;
				NodeHead	*Node	=	NULL;
	register	struct _CacheEntry	*Entry	=	NULL;
	register	int			DataLength	=	CacheInfo -> NodeChunk.DataLength;

	while(Inited)
	{
		cc = (-1) * (CacheInfo -> NodeChunk.Used);

		for(loop = cc + 1; loop <= 0; ++loop)
		{
			Node = (NodeHead *)((CacheInfo -> NodeChunk.Data) + loop * DataLength);
			Entry = HashTable_GetDataByNode(Node);

			if( Node -> Next >= HASHTABLE_NODE_END && Entry -> TTL > 0 )
			{
				if(--(Entry -> TTL) == 0)
				{
/*
					static char Name[128];
*/

					if(GotMutex == FALSE)
					{
						RWLock_WrLock(CacheLock);
						GotMutex = TRUE;
					}

/*
					sscanf(MapStart + CacheInfo[loop].Offset + 1, "%127[^\1]", Name);
					printf("[C][%s] : Cache removed.\n", Name);
*/
					*(char *)(MapStart + Entry -> Offset) = 0xFD;

					HashTable_RemoveNode(CacheInfo, -1, Node);
				}
			}
		}

		if(GotMutex == TRUE)
		{
			if( CacheInfo -> NodeChunk.Used == 0 )
			{
				(*CacheEnd) = sizeof(struct _Header);
			} else {
				Node = (NodeHead *)((CacheInfo -> NodeChunk.Data) + (-1) * (CacheInfo -> NodeChunk.Used - 1) * DataLength);
				Entry = HashTable_GetDataByNode(Node);
				(*CacheEnd) = Entry -> Offset + Entry -> Length;
			}

			RWLock_UnWLock(CacheLock);
			GotMutex = FALSE;

		}

		SLEEP(1000);
	}

	TTLMinusOne_Thread = INVALID_THREAD;
}

static BOOL IsReloadable(void)
{
	struct _Header	*Header = (struct _Header *)MapStart;

	if( Header -> Ver != CACHE_VERSION )
	{
		ERRORMSG("The existing cache is not compatible with this version of program.\n");
		return FALSE;
	}

	if( Header -> CacheSize != CacheSize )
	{
		ERRORMSG("The size of the existing cache and the value of `CacheSize' should be equal.\n");
		return FALSE;
	}
	return TRUE;
}

static void ReloadCache(void)
{
	struct _Header	*Header = (struct _Header *)MapStart;

	INFO("Loading the existing cache ...\n");

	CacheInfo = &(Header -> ht);

	CacheInfo -> Slots.Data = MapStart + CacheSize - (CacheInfo -> Slots.DataLength) * (CacheInfo -> Slots.Used);
	CacheInfo -> NodeChunk.Data = CacheInfo -> Slots.Data - CacheInfo -> NodeChunk.DataLength;

	CacheEnd = &(Header -> End);

	INFO("Cache reloaded, containing %d entries.\n", CacheInfo -> NodeChunk.Used);
}

static int CalculateSlotCount(void)
{
	int PreValue;
	if( CacheSize < 1048576 )
	{
		PreValue = CacheSize / 4979 - 18;
	} else {
		PreValue = pow(log((double)CacheSize), 2);
	}

	return ROUND(PreValue, 10) + 6;
}

static void ManuallyInitHashTable(HashTable *ht)
{
	int loop;

	ht -> Slots.Used = CalculateSlotCount();
	ht -> Slots.DataLength = sizeof(NodeHead);
	ht -> Slots.Data = MapStart + CacheSize - (ht -> Slots.DataLength) * (ht -> Slots.Used);
	ht -> Slots.Allocated = ht -> Slots.Used;

	for(loop = 0; loop != ht -> Slots.Allocated; ++loop)
	{
		((NodeHead *)Array_GetBySubscript(&(ht -> Slots), loop)) -> Next = HASHTABLE_NODE_END;
		((NodeHead *)Array_GetBySubscript(&(ht -> Slots), loop)) -> Prev = 0xAAAA0000 + loop;
	}

	ht -> NodeChunk.DataLength = sizeof(struct _CacheEntry) + sizeof(NodeHead);
	ht -> NodeChunk.Data = ht -> Slots.Data - ht -> NodeChunk.DataLength;
	ht -> NodeChunk.Used = 0;
	ht -> NodeChunk.Allocated = -1;

	ht -> RemovedNodes = -1;
}

static void CreateNewCache(void)
{
	struct _Header	*Header = (struct _Header *)MapStart;

	memset(MapStart, 0, CacheSize);

	Header -> Ver = CACHE_VERSION;
	Header -> CacheSize = CacheSize;
	CacheEnd = &(Header -> End);
	*CacheEnd = sizeof(struct _Header);
	memset(Header -> Comment, 0, sizeof(Header -> Comment));
	strcpy(Header -> Comment, "\nDo not edit this file.\n");

	CacheInfo = &(Header -> ht);

	ManuallyInitHashTable(CacheInfo);

}

static int InitCacheInfo(BOOL Reload)
{

	if( Reload == TRUE )
	{
		if( IsReloadable() )
		{
			ReloadCache();
		} else {
			if( ConfigGetBoolean(&ConfigInfo, "OverwriteCache") == FALSE )
			{
				return -1;
			} else {
				CreateNewCache();
				INFO("The existing cache has been overwritten.\n");
			}
		}
	} else {
		CreateNewCache();
	}
	return 0;
}

int DNSCache_Init(void)
{
	int			_CacheSize = ConfigGetInt32(&ConfigInfo, "CacheSize");
	BOOL		IgnoreTTL = ConfigGetBoolean(&ConfigInfo, "IgnoreTTL");
	const char	*CacheFile = ConfigGetString(&ConfigInfo, "CacheFile");
	BOOL		FileExists;
	int			InitCacheInfoState;

	ForceTTL = ConfigGetInt32(&ConfigInfo, "ForceTTL");

	TTLMultiple = ConfigGetInt32(&ConfigInfo, "MultipleTTL");

	if(TTLMultiple == 0) return 1;

	if( _CacheSize % sizeof(void *) != 0 )
	{
		CacheSize = ROUND_UP(_CacheSize, 8);
	} else {
		CacheSize = _CacheSize;
	}

	if( CacheSize < 102400 )
	{
		ERRORMSG("Cache size must not less than 102400 bytes.\n");
		return 1;
	}

	FileExists = FileIsReadable(CacheFile);

	CacheFileHandle = OPEN_FILE(CacheFile);
	if(CacheFileHandle == INVALID_FILE)
	{
		int ErrorNum = GET_LAST_ERROR();
		char ErrorMessage[320];

		GetErrorMsg(ErrorNum, ErrorMessage, sizeof(ErrorMessage));

		ERRORMSG("Cache initializing failed : %d : %s.\n", ErrorNum, ErrorMessage);

		return 2;
	}

	CacheMappingHandle = CREATE_FILE_MAPPING(CacheFileHandle, CacheSize);
	if(CacheMappingHandle == INVALID_MAP)
	{
		int ErrorNum = GET_LAST_ERROR();
		char ErrorMessage[320];

		GetErrorMsg(ErrorNum, ErrorMessage, sizeof(ErrorMessage));

		ERRORMSG("Cache initializing failed : %d : %s.\n", ErrorNum, ErrorMessage);
		return 3;
	}

	MapStart = (char *)MPA_FILE(CacheMappingHandle, CacheSize);
	if(MapStart == INVALID_MAPPING_FILE)
	{
		int ErrorNum = GET_LAST_ERROR();
		char ErrorMessage[320];

		GetErrorMsg(ErrorNum, ErrorMessage, sizeof(ErrorMessage));

		ERRORMSG("Cache initializing failed : %d : %s.\n", ErrorNum, ErrorMessage);
		return 4;
	}

	RWLock_Init(CacheLock);

	if( FileExists == FALSE )
	{
		InitCacheInfoState = InitCacheInfo(FALSE);
	} else {
		InitCacheInfoState = InitCacheInfo(ConfigGetBoolean(&ConfigInfo, "ReloadCache"));
	}

	if( InitCacheInfoState != 0 )
	{
		return 5;
	}

	Inited = TRUE;

	if(IgnoreTTL == FALSE)
		CREATE_THREAD(DNSCacheTTLMinusOne_Thread, NULL, TTLMinusOne_Thread);

	return 0;
}

BOOL Cache_IsInited(void)
{
	return Inited;
}

static _32BIT_INT DNSCache_GetAviliableZone(_32BIT_UINT Length, NodeHead **Out)
{
	_32BIT_INT	Itr = HASHTABLE_FINDUNUSEDNODE_START;
	NodeHead	*Node;
	struct _CacheEntry *Entry;
	_32BIT_UINT RoundedLength = ROUND_UP(Length, 8);

	while( TRUE )
	{
		Itr = HashTable_FindUnusedNode(CacheInfo, &Node, Itr, NULL, FALSE);
		if( Itr < 0 )
		{
			Itr = HashTable_CreateNewNode(CacheInfo, &Node, MapStart + (*CacheEnd) + RoundedLength);
			if( Itr < 0 )
			{
				return -1;
			}

			Entry = HashTable_GetDataByNode(Node);

			Entry -> Offset = (*CacheEnd);
			Entry -> Length = RoundedLength;
			(*CacheEnd) += RoundedLength;

			break;
		}

		Entry = HashTable_GetDataByNode(Node);

		if( Entry -> Length == RoundedLength )
		{
			HashTable_FetchNode(CacheInfo, Node);
			break;
		}
	}

	memset(MapStart + Entry -> Offset + Length, 0xFE, RoundedLength - Length);

	*Out = Node;
	return Itr;
}

static struct _CacheEntry *DNSCache_FindFromCache(char *Content, size_t Length, struct _CacheEntry *Start)
{
	struct _CacheEntry	*Entry = Start;

	do{
		Entry = HashTable_Get(CacheInfo, Content, Entry);
		if( Entry == NULL )
		{
			return NULL;
		}
		if( memcmp(Content, MapStart + Entry -> Offset + 1, Length) == 0 )
		{
			return Entry;
		}
	} while( TRUE );

}

static char *DNSCache_GenerateTextFromRawRecord(char *DNSBody, char *DataBody, char *Buffer, DNSRecordType ResourceType)
{
	int loop2;

	const ElementDescriptor *Descriptor;
	int DescriptorCount;

	if(Buffer == NULL) return NULL;

	DescriptorCount = DNSGetDescriptor(ResourceType, &Descriptor);

	if(DescriptorCount != 0)
	{
		char InnerBuffer[512];
		DNSDataInfo Data;
		for(loop2 = 0; loop2 != DescriptorCount; ++loop2)
		{
			Data = DNSParseData(DNSBody, DataBody, InnerBuffer, sizeof(InnerBuffer), Descriptor, DescriptorCount, loop2 + 1);
			switch(Data.DataType)
			{
				case DNS_DATA_TYPE_INT:
					if(Data.DataLength == 1)Buffer += sprintf(Buffer, "%d", (int)*(char *)InnerBuffer);
					if(Data.DataLength == 2)Buffer += sprintf(Buffer, "%d", (int)*(_16BIT_INT *)InnerBuffer);
					if(Data.DataLength == 4)Buffer += sprintf(Buffer, "%u", *(_32BIT_INT *)InnerBuffer);
					break;
				case DNS_DATA_TYPE_UINT:
					if(Data.DataLength == 1)Buffer += sprintf(Buffer, "%d", (int)*(unsigned char *)InnerBuffer);
					if(Data.DataLength == 2)Buffer += sprintf(Buffer, "%d", (int)*(_16BIT_UINT *)InnerBuffer);
					if(Data.DataLength == 4)Buffer += sprintf(Buffer, "%u", *(_32BIT_UINT *)InnerBuffer);
					break;
				case DNS_DATA_TYPE_STRING:
					Buffer += sprintf(Buffer, "%s", InnerBuffer);
					break;
				default:
					break;
			}
			*Buffer++ = '\0';
		}
	}
	return Buffer;
}

static int DNSCache_AddAItemToCache(char *DNSBody, char *RecordBody)
{
	char		Buffer[512];
	char		*BufferItr = Buffer;


	Buffer[0] = CACHE_START;
	DNSGetHostName(DNSBody, RecordBody, Buffer + 1);
	BufferItr += strlen(Buffer);
	BufferItr += sprintf(BufferItr, "\1%d\1%d", (int)DNSGetRecordType(RecordBody), (int)DNSGetRecordClass(RecordBody));

	*BufferItr++ = '\0';

	BufferItr = DNSCache_GenerateTextFromRawRecord(DNSBody, DNSGetResourceDataPos(RecordBody), BufferItr, (DNSRecordType)DNSGetRecordType(RecordBody));
	if(BufferItr == NULL) return -1;

	*BufferItr = CACHE_END;
	if(DNSCache_FindFromCache(Buffer + 1, BufferItr - Buffer, NULL) == NULL)
	{
		_32BIT_INT	Subscript;
		NodeHead	*Node;
		struct _CacheEntry	*Entry = NULL;

		Subscript = DNSCache_GetAviliableZone(BufferItr - Buffer + 1, &Node);
		if(Subscript >= 0)
		{
			Entry = HashTable_GetDataByNode(Node);

			memcpy(MapStart + Entry -> Offset, Buffer, BufferItr - Buffer + 1);

			Entry -> OriginalTTL = DNSGetTTL(RecordBody);

			if(ForceTTL < 0)
			{
				Entry -> TTL = Entry -> OriginalTTL * TTLMultiple;
			} else {
				Entry -> TTL = ForceTTL;
			}

			HashTable_AddByNode(CacheInfo, Buffer + 1, Subscript, Node);
		} else {
			return -1;
		}
	}

	return 0;
}

int DNSCache_AddItemsToCache(char *DNSBody)
{
	int loop;
	int AnswerCount;
	if(Inited == FALSE) return 0;
	if(TTLMultiple < 1) return 0;
	AnswerCount = DNSGetAnswerCount(DNSBody);
	RWLock_WrLock(CacheLock);

	for(loop = 1; loop != AnswerCount + 1; ++loop)
	{
		if( DNSCache_AddAItemToCache(DNSBody, DNSGetAnswerRecordPosition(DNSBody, loop)) != 0 )
			return -1;
	}

	RWLock_UnWLock(CacheLock);

	return 0;
}

static int DNSCache_GenerateDatasFromCache(	__in			char				*Datas,
											__inout			ExtendableBuffer	*Buffer,
											__in	const	ElementDescriptor	*Descriptor,
											__in			int					CountOfDescriptor
											)
{
	int		TotleLength = 0;
	char	*HereSaved;
	int		SingleLength;
	int		loop;

	for(loop = 0; loop != CountOfDescriptor && *Datas != CACHE_END; ++loop)
	{
		SingleLength = DNSGenerateData(Datas, NULL, 0, Descriptor + loop);

		HereSaved = ExtendableBuffer_Expand(Buffer, SingleLength);
		if( HereSaved == NULL )
		{
			return 0;
		}

		DNSGenerateData(Datas, HereSaved, SingleLength, Descriptor + loop);

		TotleLength += SingleLength;

		/* move to next record */
		for(;*Datas != '\0'; ++Datas);
		++Datas;
	}

	return TotleLength;
}

static int DNSCache_GetRawRecordsFromCache(	__in	char				*Name,
											__in	DNSRecordType		Type,
											__in	DNSRecordClass		Class,
											__inout ExtendableBuffer	*Buffer,
											__out	int					*RecordsLength
											)
{
	char	*HereSaved = NULL;
	int		SingleLength;
	char	*CacheItr;
	struct _CacheEntry *Entry = NULL;

	int		DatasLen;

	int RecordCount = 0;

	const ElementDescriptor *Descriptor;
	int CountOfDescriptor;

	char Name_Type_Class[256];

	*RecordsLength = 0;

	sprintf(Name_Type_Class, "%s\1%d\1%d", Name, Type, Class);

	do
	{
		Entry = DNSCache_FindFromCache(Name_Type_Class, strlen(Name_Type_Class) + 1, Entry);
		if( Entry == NULL )
		{
			break;
		}

		if( Entry -> TTL != 0 )
		{
			CountOfDescriptor = DNSGetDescriptor((DNSRecordType)Type, &Descriptor);
			if(CountOfDescriptor != 0)
			{
				++RecordCount;

				CacheItr = MapStart + Entry -> Offset + 1;

				SingleLength = DNSGenResourceRecord(NULL, 0, Name, Type, Class, 0, NULL, 0, FALSE);

				HereSaved = ExtendableBuffer_Expand((ExtendableBuffer *)Buffer, SingleLength);

				if(ForceTTL < 0)
					DNSGenResourceRecord(HereSaved, SingleLength, Name, Type, Class, Entry -> TTL / TTLMultiple, NULL, 0, FALSE);
				else
					DNSGenResourceRecord(HereSaved, SingleLength, Name, Type, Class, Entry -> OriginalTTL, NULL, 0, FALSE);

				for(; *CacheItr != '\0'; ++CacheItr);
				/* Then *CacheItr == '\0' */
				++CacheItr;
				/* Then the data position */

				DatasLen = DNSCache_GenerateDatasFromCache(CacheItr, (ExtendableBuffer *)Buffer, Descriptor, CountOfDescriptor);

				SET_16_BIT_U_INT(ExtendableBuffer_GetData(Buffer) + ExtendableBuffer_GetEndOffset(Buffer) - DatasLen - 2, DatasLen);
				(*RecordsLength) += (SingleLength + DatasLen);

			}
		}
	} while ( TRUE );

	return RecordCount;
}

static struct _CacheEntry *DNSCache_GetCNameFromCache(__in char *Name, __out char *Buffer)
{
	char Name_Type_Class[256];
	struct _CacheEntry *Entry = NULL;

	sprintf(Name_Type_Class, "%s\1%d\1%d", Name, DNS_TYPE_CNAME, 1);

	do
	{
		Entry = DNSCache_FindFromCache(Name_Type_Class, strlen(Name_Type_Class) + 1, Entry);
		if( Entry == NULL )
		{
			return NULL;
		}

		if(Entry -> TTL != 0 )
		{
			strcpy(Buffer, MapStart + Entry -> Offset + 1 + strlen(Name_Type_Class) + 1);
			return Entry;
		}
	} while( TRUE );

}

int DNSCache_GetByQuestion(__in char *Question, __inout ExtendableBuffer *Buffer, __out int *RecordsLength)
{
	int		SingleLength	=	0;
	char	Name[260];
	char	CName[260];
	char	*HereSaved;

	struct _CacheEntry *Entry;

	int		RecordsCount	=	0;

	DNSRecordType	Type;
	DNSRecordClass	Class;

	if(Inited == FALSE) return -1;
	if(TTLMultiple < 1) return -2;

	*RecordsLength = 0;

	DNSGetHostName(Question, DNSJumpHeader(Question), Name);

	Type = (DNSRecordType)DNSGetRecordType(DNSJumpHeader(Question));
	Class = (DNSRecordClass)DNSGetRecordClass(DNSJumpHeader(Question));


	RWLock_RdLock(CacheLock);

	/* If the intended type is not DNS_TYPE_CNAME, then first find its cname */
	if(Type != DNS_TYPE_CNAME)
	{
		while( (Entry = DNSCache_GetCNameFromCache(Name, CName)) != NULL )
		{
			++RecordsCount;

			SingleLength = DNSGenResourceRecord(NULL, 0, Name, DNS_TYPE_CNAME, 1, 0, CName, strlen(CName) + 1, TRUE);
			(*RecordsLength) += SingleLength;

			HereSaved = ExtendableBuffer_Expand(Buffer, SingleLength);

			DNSGenResourceRecord(HereSaved, SingleLength, Name, DNS_TYPE_CNAME, 1, Entry -> TTL, CName, strlen(CName) + 1, TRUE);

			strcpy(Name, CName);
		}
	}

	RecordsCount += DNSCache_GetRawRecordsFromCache(Name, Type, Class, Buffer, &SingleLength);

	RWLock_UnRLock(CacheLock);

	if( RecordsCount == 0 || SingleLength == 0 )
	{
		return 0;
	}

	(*RecordsLength) += SingleLength;

	return RecordsCount;
}

void DNSCacheClose(void)
{
	if(Inited == TRUE)
	{
		Inited = FALSE;
		RWLock_WrLock(CacheLock);
		UNMAP_FILE(MapStart, CacheSize);
		DESTROY_MAPPING(CacheMappingHandle);
		CLOSE_FILE(CacheFileHandle);
		RWLock_UnWLock(CacheLock);
		RWLock_Destroy(CacheLock);

	}
}
