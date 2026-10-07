#include <JSystem/JKernel/JKRFileFinder.hpp>
#include <JSystem/JKernel/JKRArchive.hpp>
#include <dolphin/dvd.h>

JKRArcFinder::JKRArcFinder(JKRArchive* archive, long startindex, long entries)
    : JKRFileFinder()
{
	mArchive = archive;

	mIsAvailable = entries > 0;
	mStartIndex  = startindex;
	mEndIndex    = startindex + entries - 1;
	mNextIndex   = mStartIndex;

	findNextFile();
}

bool JKRArcFinder::findNextFile()
{
	if (mIsAvailable) {
		mIsAvailable = (mNextIndex <= mEndIndex);
		if (mIsAvailable) {
			JKRArchive::SDirEntry dirEntry;
			mIsAvailable         = mArchive->getDirEntry(&dirEntry, mNextIndex);
			mBase.mFileName      = dirEntry.mName;
			mBase.mFileIndex     = mNextIndex;
			mBase.mFileID        = dirEntry.mID;
			mBase.mFileTypeFlags = dirEntry.mFlags;
			mIsDir               = FLAG_OFF(mBase.mFileTypeFlags, 2);
			mNextIndex++;
		}
	}
	return mIsAvailable;
}

JKRDvdFinder::JKRDvdFinder(const char* directory) {
    mIsDvdOpen = DVDOpenDir(directory, &mDir);
    mIsAvailable = mIsDvdOpen;
    findNextFile();
}

JKRDvdFinder::~JKRDvdFinder() {
		if (mIsDvdOpen) {
			DVDCloseDir(&mDir);
		}
}

bool JKRDvdFinder::findNextFile() {
    if (mIsAvailable) {
        DVDDirEntry directoryEntry;
        mIsAvailable = DVDReadDir(&mDir, &directoryEntry);

        if (mIsAvailable) {
            mIsDir = directoryEntry.isDir != 0;
            mBase.mFileName = directoryEntry.name;
            mBase.mFileIndex = directoryEntry.entryNum;
            mBase.mFileID = 0;

            // only matches with enum
            // TODO: placeholder
            enum EntryTypeFlags {
                EntryTypeFlags1 = 1,
                EntryTypeFlags2 = 2,
            };
            mBase.mFileTypeFlags = mIsDir ? EntryTypeFlags2 : EntryTypeFlags1;
        }
    }

    return mIsAvailable;
}
