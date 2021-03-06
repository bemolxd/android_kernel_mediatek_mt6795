/*************************************************************************/ /*!
@File
@Title          Functions for creating debugfs directories and entries.
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include "pvr_debug.h"
#include "pvr_debugfs.h"

#define PVR_DEBUGFS_DIR_NAME "pvr"

static struct dentry *gpsPVRDebugFSEntryDir = NULL;

/* Lock used when adjusting refCounts and deleting entries */
static struct mutex gDebugFSLock;

/*************************************************************************/ /*!
 Statistic entry read functions
*/ /**************************************************************************/

typedef struct _PVR_DEBUGFS_DRIVER_STAT_
{
	//struct dentry			*psEntry;
	void				*pvData;
	PVRSRV_GET_NEXT_STAT_FUNC	*pfnGetNextStat;
	PVRSRV_INC_STAT_MEM_REFCOUNT_FUNC	*pfnIncStatMemRefCount;
	PVRSRV_DEC_STAT_MEM_REFCOUNT_FUNC	*pfnDecStatMemRefCount;
	IMG_UINT32			ui32RefCount;
	IMG_INT32			i32StatValue;
	IMG_CHAR			*pszStatFormat;
	void					*pvDebugFSEntry;
} PVR_DEBUGFS_DRIVER_STAT;
typedef struct _PVR_DEBUGFS_PRIV_DATA_
{
	struct seq_operations	*psReadOps;
	PVRSRV_ENTRY_WRITE_FUNC	*pfnWrite;
	void			*pvData;
	IMG_BOOL		bValid;
} PVR_DEBUGFS_PRIV_DATA;
typedef struct _PVR_DEBUGFS_DIR_DATA_ PVR_DEBUGFS_DIR_DATA;
typedef struct _PVR_DEBUGFS_DIR_DATA_
{
	struct dentry *psDir;
	PVR_DEBUGFS_DIR_DATA *psParentDir;
	IMG_UINT32	ui32RefCount;
} PVR_DEBUGFS_DIR_DATA;
typedef struct _PVR_DEBUGFS_ENTRY_DATA_
{
	struct dentry *psEntry;
	PVR_DEBUGFS_DIR_DATA *psParentDir;
	IMG_UINT32	ui32RefCount;
	PVR_DEBUGFS_DRIVER_STAT *psStatData;
} PVR_DEBUGFS_ENTRY_DATA;
static void _RefDirEntry(PVR_DEBUGFS_DIR_DATA *psDirEntry);
static void _UnrefAndMaybeDestroyDirEntry(PVR_DEBUGFS_DIR_DATA *psDirEntry);
#if 0
static void _RefDebugFSEntry(PVR_DEBUGFS_ENTRY_DATA *psDebugFSEntry);
#endif
static void _UnrefAndMaybeDestroyDebugFSEntry(PVR_DEBUGFS_ENTRY_DATA *psDebugFSEntry);
static IMG_BOOL _RefStatEntry(void *pvStatEntry);
static IMG_BOOL _UnrefAndMaybeDestroyStatEntry(void *pvStatEntry);

static void *_DebugFSStatisticSeqStart(struct seq_file *psSeqFile, loff_t *puiPosition)
{
	PVR_DEBUGFS_DRIVER_STAT *psStatData = (PVR_DEBUGFS_DRIVER_STAT *)psSeqFile->private;
	IMG_BOOL bResult = IMG_FALSE;

	if (psStatData)
	{
		if (psStatData->pvData)
		{
			/* take reference on psStatData (for duration of stat iteration) */
			if (!_RefStatEntry((void*)psStatData))
			{
				//PVR_DPF((PVR_DBG_ERROR, "%s: _RefStatEntry() returned IMG_FALSE", __FUNCTION__));
				return NULL;
			}
		}
	bResult = psStatData->pfnGetNextStat(psStatData->pvData,
					    (IMG_UINT32)(*puiPosition),
					    &psStatData->i32StatValue,
					    &psStatData->pszStatFormat);
	}

	return bResult ? psStatData : NULL;
}

static void _DebugFSStatisticSeqStop(struct seq_file *psSeqFile, void *pvData)
{
	PVR_DEBUGFS_DRIVER_STAT *psStatData = (PVR_DEBUGFS_DRIVER_STAT *)psSeqFile->private;

	if (psStatData)
	{
		/* drop ref taken on stat memory, and if it is now zero, be sure we don't try to read it again */
		if ((psStatData->ui32RefCount > 0) && (psStatData->pvData))
		{
			/* drop reference on psStatData (held for duration of stat iteration) */
			_UnrefAndMaybeDestroyStatEntry((void*)psStatData);
		}
	}
}

static void *_DebugFSStatisticSeqNext(struct seq_file *psSeqFile,
				      void *pvData,
				      loff_t *puiPosition)
{
	PVR_DEBUGFS_DRIVER_STAT *psStatData = (PVR_DEBUGFS_DRIVER_STAT *)psSeqFile->private;
	IMG_BOOL bResult = IMG_FALSE;

	if (puiPosition)
	{
	(*puiPosition)++;

		if (psStatData)
		{
			if (psStatData->pvData)
			{
	bResult = psStatData->pfnGetNextStat(psStatData->pvData,
					    (IMG_UINT32)(*puiPosition),
					    &psStatData->i32StatValue,
					    &psStatData->pszStatFormat);
			}
		}
	}
	return bResult ? psStatData : NULL;
}

static int _DebugFSStatisticSeqShow(struct seq_file *psSeqFile, void *pvData)
{
	PVR_DEBUGFS_DRIVER_STAT *psStatData = (PVR_DEBUGFS_DRIVER_STAT *)pvData;

	if (psStatData != NULL)
	{
		if (psStatData->pszStatFormat == NULL)
		{
			return -EINVAL;
		}

		seq_printf(psSeqFile, psStatData->pszStatFormat, psStatData->i32StatValue);
	}

	return 0;
}

static struct seq_operations gsDebugFSStatisticReadOps =
{
	.start = _DebugFSStatisticSeqStart,
	.stop = _DebugFSStatisticSeqStop,
	.next = _DebugFSStatisticSeqNext,
	.show = _DebugFSStatisticSeqShow,
};


/*************************************************************************/ /*!
 Common internal API
*/ /**************************************************************************/

static int _DebugFSFileOpen(struct inode *psINode, struct file *psFile)
{
	PVR_DEBUGFS_PRIV_DATA *psPrivData = (PVR_DEBUGFS_PRIV_DATA *)psINode->i_private;
	int iResult;

	iResult = seq_open(psFile, psPrivData->psReadOps);
	if (iResult == 0)
	{
		struct seq_file *psSeqFile = psFile->private_data;

		//PVR_DPF((PVR_DBG_ERROR, "%s: psPrivData=<%p>", __FUNCTION__, (void*)psPrivData));

		psSeqFile->private = psPrivData->pvData;

		{
//			PVR_DEBUGFS_DRIVER_STAT *psStatData = (PVR_DEBUGFS_DRIVER_STAT *)psSeqFile->private;
	//		PVR_DEBUGFS_ENTRY_DATA  *psDebugFSEntry;
			if (!psPrivData->bValid)
#if 0
			{
				if (psStatData->ui32RefCount == 0)
				{
					PVR_DPF((PVR_DBG_ERROR, "%s: psStatData->ui32RefCount == 0", __FUNCTION__));
					seq_release(psINode, psFile);
					return -EIO;
				}
				psDebugFSEntry = (PVR_DEBUGFS_ENTRY_DATA *)psStatData->pvDebugFSEntry;
				if (psDebugFSEntry->ui32RefCount == 0)
				{
					PVR_DPF((PVR_DBG_ERROR, "%s: psDebugFSEntry->ui32RefCount == 0", __FUNCTION__));
					seq_release(psINode, psFile);
					return -EIO;
				}
			}
			else
#else
			{
				//PVR_DPF((PVR_DBG_ERROR, "%s: psPrivData<%p>->bValid==IMG_FALSE", __FUNCTION__, (void*)psPrivData));
				seq_release(psINode, psFile);
				return -EIO;
			}
#endif
		}

	}

	return iResult;
}

static ssize_t _DebugFSFileWrite(struct file *psFile,
				 const char __user *pszBuffer,
				 size_t uiCount,
				 loff_t *puiPosition)
{
	struct inode *psINode = psFile->f_path.dentry->d_inode;
	PVR_DEBUGFS_PRIV_DATA *psPrivData = (PVR_DEBUGFS_PRIV_DATA *)psINode->i_private;

	if (psPrivData->pfnWrite == NULL)
	{
		return -EIO;
	}

	return psPrivData->pfnWrite(pszBuffer, uiCount, *puiPosition, psPrivData->pvData);
}

static const struct file_operations gsPVRDebugFSFileOps =
{
	.owner = THIS_MODULE,
	.open = _DebugFSFileOpen,
	.read = seq_read,
	.write = _DebugFSFileWrite,
	.llseek = seq_lseek,
	.release = seq_release,
};


/*************************************************************************/ /*!
 Public API
*/ /**************************************************************************/

/*************************************************************************/ /*!
@Function       PVRDebugFSInit
@Description    Initialise PVR debugfs support. This should be called before
                using any PVRDebugFS functions.
@Return         int      On success, returns 0. Otherwise, returns an
                         error code.
*/ /**************************************************************************/
int PVRDebugFSInit(void)
{
	PVR_ASSERT(gpsPVRDebugFSEntryDir == NULL);

	mutex_init(&gDebugFSLock);

	gpsPVRDebugFSEntryDir = debugfs_create_dir(PVR_DEBUGFS_DIR_NAME, NULL);
	if (gpsPVRDebugFSEntryDir == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,
			 "%s: Cannot create '%s' debugfs root directory",
			 __FUNCTION__, PVR_DEBUGFS_DIR_NAME));

		return -ENOMEM;
	}

	return 0;
}

/*************************************************************************/ /*!
@Function       PVRDebugFSDeInit
@Description    Deinitialise PVR debugfs support. This should be called only
                if PVRDebugFSInit() has already been called. All debugfs
                directories and entries should be removed otherwise this
                function will fail.
@Return         void
*/ /**************************************************************************/
void PVRDebugFSDeInit(void)
{
	PVR_ASSERT(gpsPVRDebugFSEntryDir != NULL);

	debugfs_remove(gpsPVRDebugFSEntryDir);
	gpsPVRDebugFSEntryDir = NULL;
}

/*************************************************************************/ /*!
@Function       PVRDebugFSCreateEntryDir
@Description    Create a directory for debugfs entries that will be located
                under the root directory, as created by
                PVRDebugFSCreateEntries().
@Input          pszName      String containing the name for the directory.
@Input          psParentDir  The parent directory in which to create the new
                             directory. This should either be NULL, meaning it
                             should be created in the root directory, or a
                             pointer to a directory as returned by this
                             function.
@Output         ppsDir       On success, points to the newly created
                             directory.
@Return         int          On success, returns 0. Otherwise, returns an
                             error code.
*/ /**************************************************************************/
int PVRDebugFSCreateEntryDir(IMG_CHAR *pszName,
			     void *pvParentDir,
				 void **ppvNewDirHandle)
{
	PVR_DEBUGFS_DIR_DATA *psParentDir = (PVR_DEBUGFS_DIR_DATA*)pvParentDir;
	PVR_DEBUGFS_DIR_DATA *psNewDir;

	PVR_ASSERT(gpsPVRDebugFSEntryDir != NULL);

	if (pszName == NULL || ppvNewDirHandle == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s:   Invalid param  Exit", __FUNCTION__));
		return -EINVAL;
	}

	psNewDir = kmalloc(sizeof(*psNewDir), GFP_KERNEL);

	if (psNewDir == IMG_NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,
			 "%s: Cannot allocate memory for '%s' pvr_debugfs structure",
			 __FUNCTION__, pszName));
		return -ENOMEM;
	}

	psNewDir->psParentDir = psParentDir;
	psNewDir->psDir = debugfs_create_dir(pszName, (psNewDir->psParentDir) ? psNewDir->psParentDir->psDir : gpsPVRDebugFSEntryDir);

	if (psNewDir->psDir == NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,
			 "%s: Cannot create '%s' debugfs directory",
			 __FUNCTION__, pszName));

		kfree(psNewDir);
		return -ENOMEM;
	}

	*ppvNewDirHandle = (void*)psNewDir;
	psNewDir->ui32RefCount = 1;

	/* if parent directory is not gpsPVRDebugFSEntryDir, increment its refCount */
	if (psNewDir->psParentDir)
	{
		_RefDirEntry(psNewDir->psParentDir);
	}
	return 0;
}

/*************************************************************************/ /*!
@Function       PVRDebugFSRemoveEntryDir
@Description    Remove a directory that was created by
                PVRDebugFSCreateEntryDir(). Any directories or files created
                under the directory being removed should be removed first.
@Input          pvDir        Pointer representing the directory to be removed.
@Return         void
*/ /**************************************************************************/
void PVRDebugFSRemoveEntryDir(void *pvDir)
{
	PVR_DEBUGFS_DIR_DATA *psDirEntry = (PVR_DEBUGFS_DIR_DATA*)pvDir;

	_UnrefAndMaybeDestroyDirEntry(psDirEntry);
}

/*************************************************************************/ /*!
@Function       PVRDebugFSCreateEntry
@Description    Create an entry in the specified directory.
@Input          pszName         String containing the name for the entry.
@Input          pvDir           Pointer from PVRDebugFSCreateEntryDir()
                                representing the directory in which to create
                                the entry or NULL for the root directory.
@Input          psReadOps       Pointer to structure containing the necessary
                                functions to read from the entry.
@Input          pfnWrite        Callback function used to write to the entry.
@Input          pvData          Private data to be passed to the read
                                functions, in the seq_file private member, and
                                the write function callback.
@Output         ppsEntry        On success, points to the newly created entry.
@Return         int             On success, returns 0. Otherwise, returns an
                                error code.
*/ /**************************************************************************/
int PVRDebugFSCreateEntry(const char *pszName,
			  void *pvDir,
			  struct seq_operations *psReadOps,
			  PVRSRV_ENTRY_WRITE_FUNC *pfnWrite,
			  void *pvData,
			  void **ppvNewEntry)
{
	PVR_DEBUGFS_PRIV_DATA *psPrivData;
	PVR_DEBUGFS_ENTRY_DATA *psDebugFSEntry;
	PVR_DEBUGFS_DIR_DATA *psDebugFSDir = (PVR_DEBUGFS_DIR_DATA*)pvDir;
	struct dentry *psEntry;
	umode_t uiMode;

	PVR_ASSERT(gpsPVRDebugFSEntryDir != NULL);

	psPrivData = kmalloc(sizeof(*psPrivData), GFP_KERNEL);
	if (psPrivData == NULL)
	{
		return -ENOMEM;
	}
	psDebugFSEntry = kmalloc(sizeof(*psDebugFSEntry), GFP_KERNEL);
	if (psDebugFSEntry == NULL)
	{
		kfree(psPrivData);
		return -ENOMEM;
	}

	psPrivData->psReadOps = psReadOps;
	psPrivData->pfnWrite = pfnWrite;
	psPrivData->pvData = (void*)pvData;
	psPrivData->bValid = IMG_TRUE;

	//PVR_DPF((PVR_DBG_ERROR, "%s: psPrivData<%p>=IMG_TRUE", __FUNCTION__, (void*)psPrivData));

	uiMode = S_IFREG;

	if (psReadOps != NULL)
	{
		uiMode |= S_IRUGO;
	}

	if (pfnWrite != NULL)
	{
		uiMode |= S_IWUSR;
	}

	psDebugFSEntry->psParentDir = psDebugFSDir;
	psDebugFSEntry->ui32RefCount = 1;
	psDebugFSEntry->psStatData = (PVR_DEBUGFS_DRIVER_STAT*)pvData;
	if (psDebugFSEntry->psStatData->pfnIncStatMemRefCount)
	{

	}
	if (psDebugFSEntry->psParentDir)
	{
		/* increment refCount of parent directory */
		_RefDirEntry(psDebugFSEntry->psParentDir);
	}

	psEntry = debugfs_create_file(pszName,
				      uiMode,
				      (psDebugFSDir != NULL) ? psDebugFSDir->psDir : gpsPVRDebugFSEntryDir,
				      psPrivData,
				      &gsPVRDebugFSFileOps);
	if (IS_ERR(psEntry))
	{
		PVR_DPF((PVR_DBG_ERROR,
			 "%s: Cannot create debugfs '%s' file",
			 __FUNCTION__, pszName));

		return PTR_ERR(psEntry);
	}

	/* take reference on inode (for allocation held in d_inode->i_private) - stops
	 * inode being removed until we have freed the memory allocated in i_private */
	igrab(psEntry->d_inode);

	psDebugFSEntry->psEntry = psEntry;
	//PVR_DPF((PVR_DBG_ERROR, "%s: psDebugFSEntry->psEntry->d_inode_i_private=<%p>", __FUNCTION__, (void*)psDebugFSEntry->psEntry->d_inode->i_private));
	*ppvNewEntry = (void*)psDebugFSEntry;

	return 0;
}

/*************************************************************************/ /*!
@Function       PVRDebugFSRemoveEntry
@Description    Removes an entry that was created by PVRDebugFSCreateEntry().
@Input          psEntry  Pointer representing the entry to be removed.
@Return         void
*/ /**************************************************************************/
void PVRDebugFSRemoveEntry(void *pvDebugFSEntry)
{
	PVR_DEBUGFS_ENTRY_DATA *psDebugFSEntry = pvDebugFSEntry;
 
	_UnrefAndMaybeDestroyDebugFSEntry(psDebugFSEntry);
}

/*************************************************************************/ /*!
@Function       PVRDebugFSCreateStatisticEntry
@Description    Create a statistic entry in the specified directory.
@Input          pszName         String containing the name for the entry.
@Input          pvDir           Pointer from PVRDebugFSCreateEntryDir()
                                representing the directory in which to create
                                the entry or NULL for the root directory.
@Input          pfnGetNextStat  A callback function used to get the next
                                statistic when reading from the statistic
                                entry.
@Input          pvData          Private data to be passed to the provided
                                callback function.
@Return         void *          On success, a pointer representing the newly
                                created statistic entry. Otherwise, NULL.
*/ /**************************************************************************/
void *PVRDebugFSCreateStatisticEntry(const char *pszName,
				     void *pvDir,
				     PVRSRV_GET_NEXT_STAT_FUNC *pfnGetNextStat,
					 PVRSRV_INC_STAT_MEM_REFCOUNT_FUNC	*pfnIncStatMemRefCount,
					 PVRSRV_INC_STAT_MEM_REFCOUNT_FUNC	*pfnDecStatMemRefCount,
				     void *pvData)
{
	PVR_DEBUGFS_DRIVER_STAT *psStatData;
	PVR_DEBUGFS_ENTRY_DATA * psDebugFSEntry;
	IMG_UINT32 ui32R;
	int iResult;

	if (pszName == NULL || pfnGetNextStat == NULL)
	{
		return NULL;
	}
	if ((pfnIncStatMemRefCount != NULL || pfnDecStatMemRefCount != NULL) && pvData == NULL)
	{
		return NULL;
	}

	psStatData = kzalloc(sizeof(*psStatData), GFP_KERNEL);
	if (psStatData == NULL)
	{
		return NULL;
	}
	psStatData->pvData = (void*)pvData;
	psStatData->pfnGetNextStat = pfnGetNextStat;
	psStatData->pfnIncStatMemRefCount = pfnIncStatMemRefCount;
	psStatData->pfnDecStatMemRefCount = pfnDecStatMemRefCount;
	psStatData->ui32RefCount = 1;

	iResult = PVRDebugFSCreateEntry(pszName,
					pvDir,
					&gsDebugFSStatisticReadOps,
					NULL,
					psStatData,
					(void*)&psDebugFSEntry);
	if (iResult != 0)
	{
		kfree(psStatData);
		return NULL;
	}
	psStatData->pvDebugFSEntry = (void*)psDebugFSEntry;

	if (pfnIncStatMemRefCount)
	{
		/* call function to take reference on the memory holding the stat */
		ui32R = psStatData->pfnIncStatMemRefCount((void*)psStatData->pvData);
	}

	psDebugFSEntry->ui32RefCount = 1;

	return psStatData;
}

/*************************************************************************/ /*!
@Function       PVRDebugFSRemoveStatisticEntry
@Description    Removes a statistic entry that was created by
                PVRDebugFSCreateStatisticEntry().
@Input          pvEntry  Pointer representing the statistic entry to be
                         removed.
@Return         void
*/ /**************************************************************************/
void PVRDebugFSRemoveStatisticEntry(void *pvStatEntry)
{
	/* drop reference on pvStatEntry*/
	_UnrefAndMaybeDestroyStatEntry(pvStatEntry);
}

static void _RefDirEntry(PVR_DEBUGFS_DIR_DATA *psDirEntry)
{
	mutex_lock(&gDebugFSLock);

	if (psDirEntry->ui32RefCount > 0)
	{
		/* Increment refCount */
		psDirEntry->ui32RefCount++;
	}
	mutex_unlock(&gDebugFSLock);
}
static void _UnrefAndMaybeDestroyDirEntry(PVR_DEBUGFS_DIR_DATA *psDirEntry)
{
	mutex_lock(&gDebugFSLock);

	if (psDirEntry->ui32RefCount > 0)
	{
		/* Decrement refCount and free if now zero */
		if (--psDirEntry->ui32RefCount == 0)
		{
			/* if parent directory is not gpsPVRDebugFSEntryDir, decrement its refCount */
			debugfs_remove(psDirEntry->psDir);
			if (psDirEntry->psParentDir)
			{
				mutex_unlock(&gDebugFSLock);
				_UnrefAndMaybeDestroyDirEntry(psDirEntry->psParentDir);
				mutex_lock(&gDebugFSLock);
			}
			kfree(psDirEntry);
	}
	}
	mutex_unlock(&gDebugFSLock);
}

static void _UnrefAndMaybeDestroyDebugFSEntry(PVR_DEBUGFS_ENTRY_DATA *psDebugFSEntry)
{
	mutex_lock(&gDebugFSLock);
	/* Decrement refCount of psDebugFSEntry, and free if now zero */
	PVR_ASSERT(psDebugFSEntry != IMG_NULL);

	if (psDebugFSEntry->ui32RefCount > 0)
	{
		if (--psDebugFSEntry->ui32RefCount == 0)
		{
			struct dentry *psEntry = psDebugFSEntry->psEntry;

			if (psEntry)
			{
				/* Free any private data that was provided to debugfs_create_file() */
				if (psEntry->d_inode->i_private != NULL)
				{
					PVR_DEBUGFS_PRIV_DATA *psPrivData = (PVR_DEBUGFS_PRIV_DATA*)psDebugFSEntry->psEntry->d_inode->i_private;

					//PVR_DPF((PVR_DBG_ERROR, "%s: psPrivData<%p>=IMG_FALSE", __FUNCTION__, (void*)psPrivData));
					psPrivData->bValid = IMG_FALSE;
					kfree(psEntry->d_inode->i_private);
				}
				debugfs_remove(psEntry);
			}
			/* decrement refcount of parent directory */
			if (psDebugFSEntry->psParentDir)
			{
				mutex_unlock(&gDebugFSLock);
				_UnrefAndMaybeDestroyDirEntry(psDebugFSEntry->psParentDir);
				mutex_lock(&gDebugFSLock);
			}

			/* now free the memory allocated for psDebugFSEntry */
			kfree(psDebugFSEntry);
		}
	}
	mutex_unlock(&gDebugFSLock);
}

static IMG_BOOL _RefStatEntry(void *pvStatEntry)
{
	PVR_DEBUGFS_DRIVER_STAT *psStatData = (PVR_DEBUGFS_DRIVER_STAT *)pvStatEntry;
	IMG_BOOL bResult = IMG_FALSE;

	mutex_lock(&gDebugFSLock);

	bResult = (psStatData->ui32RefCount > 0);
	if (bResult)
	{
		/* Increment refCount of psStatData */
		psStatData->ui32RefCount++;
	}
	mutex_unlock(&gDebugFSLock);

	return bResult;
}

static IMG_BOOL _UnrefAndMaybeDestroyStatEntry(void *pvStatEntry)
{
	PVR_DEBUGFS_DRIVER_STAT *psStatData = (PVR_DEBUGFS_DRIVER_STAT *)pvStatEntry;
	IMG_BOOL bResult;

	mutex_lock(&gDebugFSLock);

	bResult = (psStatData->ui32RefCount > 0);
	/* Decrement refCount of psStatData, and free if now zero */
	PVR_ASSERT(pvStatEntry != IMG_NULL);

	if (bResult)
	{
		if (--psStatData->ui32RefCount == 0)
		{
			if (psStatData->pvDebugFSEntry)
			{
				mutex_unlock(&gDebugFSLock);
				_UnrefAndMaybeDestroyDebugFSEntry((PVR_DEBUGFS_ENTRY_DATA*)psStatData->pvDebugFSEntry);
			}
			if (psStatData->pfnDecStatMemRefCount)
			{
				/* call function to drop reference on the memory holding the stat */
				psStatData->pfnDecStatMemRefCount((void*)psStatData->pvData);
			}
		}
		else
		{
			mutex_unlock(&gDebugFSLock);
		}
	}
	else
	{
		mutex_unlock(&gDebugFSLock);
	}

	return bResult;
}
