
#include <unistd.h>
#include <cmath>
#include <stdexcept>

#include <log4cplus/fileappender.h>
#include <log4cplus/layout.h>
#include <log4cplus/streams.h>
#include <log4cplus/helpers/loglog.h>
#include <log4cplus/helpers/stringhelper.h>
#include <log4cplus/helpers/timehelper.h>
#include <log4cplus/helpers/property.h>
#include <log4cplus/helpers/fileinfo.h>
#include <log4cplus/spi/loggingevent.h>
#include "labor/Labor.hpp"
#include "CustomLogger.hpp"

namespace netcustomlog4cplus
{

using namespace log4cplus;

static helpers::Time
round_time (helpers::Time const & t_, helpers::chrono::seconds seconds)
{
    time_t const t = helpers::to_time_t (t_);
    return helpers::from_time_t (
        t - static_cast<time_t>(std::fmod (
                static_cast<double>(t),
                static_cast<double>(seconds.count ()))));
}

static helpers::Time
round_time_and_add (helpers::Time const & t, helpers::chrono::seconds const & seconds)
{
    return round_time (t, seconds) + seconds;
}

static helpers::Time calculateNextRolloverTime(const helpers::Time& t, DailyRollingFileSchedule schedule)
{
    namespace chrono = helpers::chrono;

    struct tm next;
    switch(schedule)
    {
    case DailyRollingFileSchedule::MONTHLY:
    {
        helpers::localTime (&next, t);
        next.tm_mon += 1;
        next.tm_mday = 1; // Round up to next month start
        next.tm_hour = 0;
        next.tm_min = 0;
        next.tm_sec = 0;
        next.tm_isdst = -1;

        helpers::Time ret;
        try
        {
            ret = helpers::from_struct_tm (&next);
        }
        catch (std::runtime_error const & e)
        {
            helpers::getLogLog().error(
                LOG4CPLUS_TEXT("calculateNextRolloverTime()-")
                LOG4CPLUS_TEXT(" from_struct_tm() returned error: ")
                + LOG4CPLUS_C_STR_TO_TSTRING (e.what ()));
            ret = round_time (t, chrono::seconds (24 * 60 * 60))
                + chrono::seconds (2678400);
        }
        return ret;
    }

    case DailyRollingFileSchedule::WEEKLY:
    {
        helpers::localTime (&next, t);
        next.tm_mday += (7 - next.tm_wday + 1);
        next.tm_hour = 0;
        next.tm_min = 0;
        next.tm_sec = 0;
        next.tm_isdst = -1;

        helpers::Time ret;
        try
        {
            ret = helpers::from_struct_tm (&next);
        }
        catch (std::runtime_error const & e)
        {
            helpers::getLogLog().error(
                LOG4CPLUS_TEXT("calculateNextRolloverTime()-")
                LOG4CPLUS_TEXT(" from_struct_tm() returned error: ")
                + LOG4CPLUS_C_STR_TO_TSTRING (e.what ()));
            ret = round_time (t, chrono::seconds (24 * 60 * 60))
                + chrono::seconds (7 * 24 * 60 * 60);
        }
        return ret;
    }

    default:
        helpers::getLogLog ().error (
            LOG4CPLUS_TEXT ("calculateNextRolloverTime()-")
            LOG4CPLUS_TEXT (" unhandled or invalid schedule value"));
        [[fallthrough]];

    case DailyRollingFileSchedule::DAILY:
    {
        helpers::localTime(&next, t);
        next.tm_mday += 1;
        next.tm_hour = 0;
        next.tm_min = 0;
        next.tm_sec = 0;
        next.tm_isdst = -1;

        helpers::Time ret;
        try
        {
            ret = helpers::from_struct_tm (&next);
        }
        catch (std::runtime_error const & e)
        {
            helpers::getLogLog().error(
                LOG4CPLUS_TEXT("calculateNextRolloverTime()-")
                LOG4CPLUS_TEXT(" from_struct_tm() returned error: ")
                + LOG4CPLUS_C_STR_TO_TSTRING (e.what ()));
            ret = round_time (t, chrono::seconds (60 * 60))
                + chrono::seconds (24 * 60 * 60);
        }

        return ret;
    }

    case DailyRollingFileSchedule::TWICE_DAILY:
    {
        helpers::localTime(&next, t);
        if (next.tm_hour < 12)
            next.tm_hour = 12;
        else
            next.tm_hour = 24;
        next.tm_min = 0;
        next.tm_sec = 0;
        next.tm_isdst = -1;

        helpers::Time ret;
        try
        {
            ret = helpers::from_struct_tm (&next);
        }
        catch (std::runtime_error const & e)
        {
            helpers::getLogLog().error(
                LOG4CPLUS_TEXT("calculateNextRolloverTime()-")
                LOG4CPLUS_TEXT(" from_struct_tm() returned error: ")
                + LOG4CPLUS_C_STR_TO_TSTRING (e.what ()));
            ret = round_time (t, chrono::seconds (60 * 60))
                + chrono::seconds (12 * 60 * 60);
        }

        return ret;
    }

    case DailyRollingFileSchedule::HOURLY:
    {
        helpers::localTime(&next, t);
        next.tm_hour += 1;
        next.tm_min = 0;
        next.tm_sec = 0;
        next.tm_isdst = -1;

        helpers::Time ret;
        try
        {
            ret = helpers::from_struct_tm (&next);
        }
        catch (std::runtime_error const & e)
        {
            helpers::getLogLog().error(
                LOG4CPLUS_TEXT("calculateNextRolloverTime()-")
                LOG4CPLUS_TEXT(" from_struct_tm() returned error: ")
                + LOG4CPLUS_C_STR_TO_TSTRING (e.what ()));
            ret = round_time (t, chrono::seconds (60 * 60))
                + chrono::seconds (60 * 60);
        }

        return ret;
    }

    case DailyRollingFileSchedule::MINUTELY:
        return round_time_and_add (t, chrono::seconds (60));
    };
}

static bool file_exist (tstring const & src)
{
   if( (access( src.c_str(), 0 )) != -1 )
   {
	   return true;
   }
   return false;
}

static long file_remove (tstring const & src)
{
#if defined (UNICODE) && defined (_WIN32)
    if (_wremove (src.c_str ()) == 0)
        return 0;
    else
        return errno;

#else
    if (std::remove (LOG4CPLUS_TSTRING_TO_STRING (src).c_str ()) == 0)
        return 0;
    else
        return errno;

#endif
}

static tstring file_index_last(const tstring& filename, unsigned int maxBackupIndex,tstring fileExt = ".data")
{
	tstring lastFileName(filename + fileExt);
    tostringstream target_oss;
    // Map {(maxBackupIndex - 1), ..., 2, 1} to {maxBackupIndex, ..., 3, 2}
    for (unsigned int i = 0; i < maxBackupIndex ; ++i)
    {
    	target_oss.str("");
    	if (i > 0)
    	{
    		target_oss << filename << LOG4CPLUS_TEXT(".") << (i) << fileExt;
    	}
    	else
    	{
    		target_oss << filename  << fileExt;//<< LOG4CPLUS_TEXT(".") << (i);
    	}
        tstring const target (target_oss.str ());
        if (file_exist(target))
        {
        	lastFileName = target;
        }
    }
    return lastFileName;
}

static tstring file_index_next(const tstring& filename, unsigned int maxBackupIndex,tstring fileExt = ".data")
{
    tostringstream target_oss;
    // Map {(maxBackupIndex - 1), ..., 2, 1} to {maxBackupIndex, ..., 3, 2}
    for (unsigned int i = 0; i < maxBackupIndex ; ++i)
    {
    	target_oss.str("");
    	if (i > 0)
    	{
    		target_oss << filename << LOG4CPLUS_TEXT(".") << (i) << fileExt;
    	}
    	else
    	{
    		target_oss << filename << fileExt;//<< LOG4CPLUS_TEXT(".") << (i);
    	}
        tstring const target (target_oss.str ());
        if (!file_exist(target))
        {
        	return target;
        }
    }
    return "";
}

static bool file_index_remove(const tstring& filename, unsigned int maxBackupIndex,tstring fileExt = ".data")
{
	bool boHasRemoveFile(false);
    tostringstream target_oss;
    // Map {(maxBackupIndex - 1), ..., 2, 1} to {maxBackupIndex, ..., 3, 2}
    bool boContinue(true);
    for (unsigned int i = 0; i < maxBackupIndex && boContinue; ++i)
    {
    	target_oss.str("");
    	if (i > 0)
    	{
    		target_oss << filename << LOG4CPLUS_TEXT(".") << (i) << fileExt;
    	}
    	else
    	{
    		target_oss << filename << fileExt;//<< LOG4CPLUS_TEXT(".") << (i);
    	}
        tstring const target (target_oss.str ());
        if (file_exist(target))
        {
//        	LOG4_TRACE("%s() target(%s)",__FUNCTION__,target.c_str());
        	file_remove(target);
        	boHasRemoveFile = true;
        }
        else
        {
        	boContinue = false;
        }
    }
    return boHasRemoveFile;
}

CustomDailyRollingFileAppender::CustomDailyRollingFileAppender(const tstring& filename,
			log4cplus::DailyRollingFileSchedule schedule ,
			long maxFileSize_ , // 10 MB
			int maxBackupIndex_,
			bool immediateFlush,
			bool createDirs,
			bool rollOnClose,
			const tstring& datePattern)
:DailyRollingFileAppender(filename, schedule, immediateFlush, maxBackupIndex_,createDirs,rollOnClose,datePattern)
{
	if (maxFileSize_ < MINIMUM_ROLLING_LOG_SIZE)
	{
		maxFileSize_ = MINIMUM_ROLLING_LOG_SIZE;
	}
	maxFileSize = maxFileSize_;
	maxBackupIndex = (std::max)(maxBackupIndex_, 1);
}

void CustomDailyRollingFileAppender::append(const spi::InternalLoggingEvent& event)
{
	bool bHasRollover(false);
	{//check time
		if(event.getTimestamp() >= nextRolloverTime) {
			rollover(true);
			bHasRollover = true;
		}
	//			FileAppender::append(event);
	}
	{//check size
		// Seek to the end of log file so that tellp() below returns the
		// right size.
		if (useLockFile)
			out.seekp (0, std::ios_base::end);

		if (!bHasRollover)//already rollover once ,don't has to Rotate log file before appending to it.
		{
			// Rotate log file if needed before appending to it.
			if (out.tellp() > maxFileSize)
				rollover(true);
		}
		FileAppender::append(event);
		// Rotate log file if needed after appending to it.
		if (out.tellp() > maxFileSize)
			rollover(true);
	}
}


FixDailyRollingFileAppender::FixDailyRollingFileAppender(
			const tstring& filename_,const tstring& fileExt_,
			log4cplus::DailyRollingFileSchedule schedule ,
			long maxFileSize_ ,int maxBackupIndex_,int maxHistory_,
			int startTryCleanFile_,int maxTryCleanFile_,
			bool immediateFlush_,bool createDirs_,bool rollOnClose_,
			const tstring& datePattern_)
:FileAppenderBase(filename_, std::ios_base::app, immediateFlush_, createDirs_)//std::ios_base::out | std::ios_base::ate | std::ios_base::app
, maxBackupIndex(maxBackupIndex_), rollOnClose(rollOnClose_)
, datePattern(datePattern_)
{
	if (maxFileSize_ < MINIMUM_ROLLING_LOG_SIZE)
	{
		maxFileSize_ = MINIMUM_ROLLING_LOG_SIZE;
	}
	maxFileSize = maxFileSize_;
	maxBackupIndex = (std::max)(maxBackupIndex_, 1);
	maxHistory = maxHistory_;
	filenamepre = filename_;
	fileExt = fileExt_;
	startTryCleanFile = startTryCleanFile_;
	maxTryCleanFile = maxTryCleanFile_;
	init(schedule);
}

void
FixDailyRollingFileAppender::init(DailyRollingFileSchedule sch)
{
//	LOG4_TRACE("%s() ",__FUNCTION__);
    this->schedule = sch;

    helpers::Time now = helpers::truncate_fractions (helpers::now ());
	scheduledFilename = getFilename(now);
	nextRolloverTime = calculateNextRolloverTime(now);

	log4cplus::tstring tmpFilename = file_index_last(scheduledFilename,maxBackupIndex,fileExt);//+ fileExt
//		LOG4_TRACE("tmpFilename(%s) maxBackupIndex(%d) fileExt(%s)",tmpFilename.c_str(),maxBackupIndex,fileExt.c_str());
	if (tmpFilename.size() > 0)
	{
		filename = tmpFilename;
		boFileCanOpen = true;
	}
	else
	{
//			LOG4_WARN("scheduledFilename(%s) open failed.",scheduledFilename.c_str());
		boFileCanOpen = false;
	}

	if (boFileCanOpen)
	{
//		LOG4_TRACE("open filename(%s) bufferSize(%u) maxFileSize(%u) useLockFile(%d)",filename.c_str(),bufferSize,maxFileSize,useLockFile);
//		LOG4_TRACE("maxFileSize(%u) tellp(%u)",maxFileSize,out.tellp());
		FileAppenderBase::init();
	}
	if (startTryCleanFile)
	{
		clean();
	}
//	LOG4_TRACE("%s() ",__FUNCTION__);
}



FixDailyRollingFileAppender::~FixDailyRollingFileAppender()
{
    destructorImpl();
//    LOG4_TRACE("%s() ",__FUNCTION__);
}

void
FixDailyRollingFileAppender::close()
{
//	LOG4_TRACE("%s() ",__FUNCTION__);
    if (rollOnClose)
        rollover();
    FileAppenderBase::close();
}

// This method does not need to be locked since it is called by
// doAppend() which performs the locking
void FixDailyRollingFileAppender::append(const spi::InternalLoggingEvent& event)
{
//	LOG4_TRACE("%s() filename(%s) lockFileName(%s) localeName(%s) ",__FUNCTION__,
//			filename.c_str(),lockFileName.c_str(),localeName.c_str());
	bool bHasRollover(false);
	{//check time
		if(event.getTimestamp() >= nextRolloverTime) {
			rollover(true);
			bHasRollover = true;
			clean();
		}
	}
	{//check size
		// Seek to the end of log file so that tellp() below returns the
		// right size.
		if (useLockFile)
			out.seekp (0, std::ios_base::end);
//		LOG4_TRACE("maxFileSize(%u) tellp(%u)",maxFileSize,out.tellp());
		if (!bHasRollover)//already rollover once ,don't has to Rotate log file before appending to it.
		{
			// Rotate log file if needed before appending to it.
			if (maxFileSize > 0 && out.tellp() > maxFileSize)
			{
				rollover(true);
				if (boFileCanOpen)
				{
					clean();
				}
			}
		}
		if (boFileCanOpen)
		{
//			LOG4_TRACE("%s() boFileOpen filename(%s) lockFileName(%s) localeName(%s) ",__FUNCTION__,
//						filename.c_str(),lockFileName.c_str(),localeName.c_str());
			FileAppenderBase::append(event);
			// Rotate log file if needed after appending to it.
			if (maxFileSize > 0 && out.tellp() > maxFileSize)
			{
				rollover(true);
			}
		}
	}
}



void FixDailyRollingFileAppender::rollover(bool alreadyLocked)
{
//	LOG4_TRACE("%s() ",__FUNCTION__);
    helpers::LockFileGuard guard;

    if (useLockFile && ! alreadyLocked)
    {
        try
        {
            guard.attach_and_lock (*lockFile);
        }
        catch (std::runtime_error const &)
        {
            return;
        }
    }

    helpers::Time now = helpers::truncate_fractions (helpers::now ());
	scheduledFilename = getFilename(now);
	nextRolloverTime = calculateNextRolloverTime(now);
	log4cplus::tstring tmpFilename = file_index_next(scheduledFilename,maxBackupIndex,fileExt);//+ fileExt
//		LOG4_TRACE("tmpFilename(%s) maxBackupIndex(%d) fileExt(%s)",tmpFilename.c_str(),maxBackupIndex,fileExt.c_str());
	if (tmpFilename.size() > 0)
	{
		filename = tmpFilename;
		boFileCanOpen = true;
	}
	else
	{
//			LOG4_WARN("scheduledFilename(%s) open failed.",scheduledFilename.c_str());
		boFileCanOpen = false;
	}

    // Close the current file
    out.close();
    // reset flags since the C++ standard specified that all the flags
    // should remain unchanged on a close
    out.clear();
//    LOG4_TRACE("%s() ",__FUNCTION__);
    if (boFileCanOpen)
    {
    	// Open a new file, e.g. "log".
//    	LOG4_TRACE("open filename(%s)",filename.c_str());
		open(std::ios_base::app);//std::ios_base::out | std::ios_base::ate | std::ios_base::app
	//    loglog_opening_result (loglog, out, filename);
    }
}

helpers::Time FixDailyRollingFileAppender::calculateNextRolloverTime(const helpers::Time& t) const
{
	return helpers::truncate_fractions (
		netcustomlog4cplus::calculateNextRolloverTime (t, schedule));
}


tstring FixDailyRollingFileAppender::getFilename(const helpers::Time& t) const
{
//	LOG4_TRACE("%s() ",__FUNCTION__);
    tchar const * pattern = 0;
    if (datePattern.empty())
    {
        switch (schedule)
        {
        case DailyRollingFileSchedule::MONTHLY:
            pattern = LOG4CPLUS_TEXT("%Y-%m");
            break;

        case DailyRollingFileSchedule::WEEKLY:
            pattern = LOG4CPLUS_TEXT("%Y-%W");
            break;

        default:
            helpers::getLogLog ().error (
                LOG4CPLUS_TEXT ("DailyRollingFileAppender::getFilename()-")
                LOG4CPLUS_TEXT (" invalid schedule value"));
            // Fall through.

        case DailyRollingFileSchedule::DAILY:
            pattern = LOG4CPLUS_TEXT("%Y-%m-%d");
            break;

        case DailyRollingFileSchedule::TWICE_DAILY:
            pattern = LOG4CPLUS_TEXT("%Y-%m-%d-%p");
            break;

        case DailyRollingFileSchedule::HOURLY:
            pattern = LOG4CPLUS_TEXT("%Y-%m-%d-%H");
            break;

        case DailyRollingFileSchedule::MINUTELY:
            pattern = LOG4CPLUS_TEXT("%Y-%m-%d-%H-%M");
            break;
        };
    }
    else
        pattern = datePattern.c_str();

    tstring result = filenamepre + helpers::getFormattedTime (pattern, t, false);
//	LOG4_TRACE("%s() filenamepre11(%s) result(%s)",__FUNCTION__,filenamepre.c_str(),result.c_str());
    return result;
}

int FixDailyRollingFileAppender::getRolloverPeriodDuration() const
{
    switch (schedule)
    {
    case DailyRollingFileSchedule::MONTHLY:
        return 31*24*3600;
    case DailyRollingFileSchedule::WEEKLY:
        return 7*24*3600;
    default:
        helpers::getLogLog ().error (
            LOG4CPLUS_TEXT ("TimeBasedRollingFileAppender::getRolloverPeriodDuration()-")
            LOG4CPLUS_TEXT (" invalid schedule value"));
        // Fall through.
    case DailyRollingFileSchedule::DAILY:
        return 24*3600;
    case DailyRollingFileSchedule::HOURLY:
        return 3600;
    case DailyRollingFileSchedule::MINUTELY:
        return 60;
    }
}

void FixDailyRollingFileAppender::clean()
{
	if (maxHistory > 0)
	{
		bool boContinueRemoveFile(true);
		helpers::Time now = helpers::truncate_fractions (helpers::now ());
		for(int i = 0; i < maxTryCleanFile && boContinueRemoveFile;++i)
		{
			helpers::Time timeToRemove = now - helpers::chrono::seconds (getRolloverPeriodDuration() * (i + maxHistory));//时间周期
			tstring filenameToRemove = getFilename(timeToRemove);//scheduledFilename
	//		LOG4_TRACE("%s() filenameToRemove(%s)",__FUNCTION__,filenameToRemove.c_str());
			boContinueRemoveFile = file_index_remove(filenameToRemove,maxBackupIndex,fileExt);
		}
	}
}


CustomTimeBasedRollingFileAppender::CustomTimeBasedRollingFileAppender(
			const tstring& filename_,
			const tstring& filenamePattern_,//// %Y-%m-%d-%H-%M
			long maxFileSize_ , // 10 MB
			long maxFileIndex_,
			int maxHistory_,
			const tstring& fileExt_,
			bool cleanHistoryOnStart_,
			bool routineCleanHistory_,
			bool immediateFlush_,//true
			bool createDirs_,//true
			bool rollOnClose_)//false
:TimeBasedRollingFileAppender(filename_,filenamePattern_, maxHistory_,cleanHistoryOnStart_,immediateFlush_,createDirs_,rollOnClose_)
{
	maxFileIndex = maxFileIndex_,
	schedule = DailyRollingFileSchedule::HOURLY;
	if (maxFileSize_ < MINIMUM_ROLLING_LOG_SIZE)
	{
		maxFileSize_ = MINIMUM_ROLLING_LOG_SIZE;
	}
	maxFileSize = maxFileSize_;
	routineCleanHistory = routineCleanHistory_;
	fileExt = fileExt_;
	init();
}

void CustomTimeBasedRollingFileAppender::init()
{
    if (filenamePattern.empty())
    {
//        getErrorHandler()->error( LOG4CPLUS_TEXT("Invalid filename/filenamePattern values") );
        return;
    }
    helpers::Time now = helpers::truncate_fractions (helpers::now ());
    {
    	tmpFilename = helpers::getFormattedTime (filenamePattern, now, false);
    	tmpFilename = file_index_last(tmpFilename,maxFileIndex);//+ fileExt
		if (tmpFilename.size() > 0)
		{
			filename = tmpFilename + fileExt;
			boFileOpen = true;
			{
			    FileAppenderBase::init();
			    nextRolloverTime = calculateNextRolloverTime(now);
			}
		}
	}

    if (cleanHistoryOnStart)
	{
		clean(now + maxHistory * getRolloverPeriodDuration());
	}
	lastHeartBeat = now;
}

void CustomTimeBasedRollingFileAppender::append(const spi::InternalLoggingEvent& event)
{
	if (!boFileOpen)
	{
		return;
	}
	bool bHasRollover(false);
	{//check time
		if(event.getTimestamp() >= nextRolloverTime) {
			rollover(true);
			bHasRollover = true;
		}
	}
	{//check size
		// Seek to the end of log file so that tellp() below returns the
		// right size.
		if (useLockFile)
			out.seekp (0, std::ios_base::end);

		if (!bHasRollover)//already rollover once ,don't has to Rotate log file before appending to it.
		{
			// Rotate log file if needed before appending to it.
			if (out.tellp() > maxFileSize)//maxFileIndex > 0 &&
			{
				rollover(true);
			}
		}
		if (boFileOpen)
		{
			FileAppenderBase::append(event);
			// Rotate log file if needed after appending to it.
			if (out.tellp() > maxFileSize)
				rollover(true);
		}
	}
}


void CustomTimeBasedRollingFileAppender::rollover(bool alreadyLocked)
{
	helpers::LockFileGuard guard;

	if (useLockFile && ! alreadyLocked)
	{
		try
		{
			guard.attach_and_lock (*lockFile);
		}
		catch (std::runtime_error const &)
		{
			return;
		}
	}

	// Close the current file
	out.close();
	// reset flags since the C++ standard specified that all the flags
	// should remain unchanged on a close
	out.clear();

	helpers::Time now = helpers::truncate_fractions (helpers::now ());

	if (routineCleanHistory)
	{
		clean(now);
	}
	{
		tmpFilename = helpers::getFormattedTime (filenamePattern, now, false);
		tmpFilename = file_index_next(tmpFilename,maxFileIndex);//+ fileExt
		if (tmpFilename.size() > 0)
		{
			filename = tmpFilename + fileExt;
			open(std::ios_base::out | std::ios_base::ate | std::ios_base::app);
			boFileOpen = true;
		}
		else
		{
			boFileOpen = false;
		}
	}
	nextRolloverTime = calculateNextRolloverTime(now);
}

void CustomTimeBasedRollingFileAppender::clean(helpers::Time time)
{
	helpers::Time::duration interval = std::chrono::hours{31*24}; // ~1 month
	if (lastHeartBeat != helpers::Time{})
	{
		interval = time - lastHeartBeat + std::chrono::seconds{1};
	}

	helpers::Time::duration period = getRolloverPeriodDuration();
	long periods = static_cast<long>(interval.count() / period.count());

	helpers::LogLog & loglog = helpers::getLogLog();
	for (long i = 0; i < periods; i++)
	{
		long periodToRemove = (-maxHistory - 1) - i;
		helpers::Time timeToRemove = time + periodToRemove * period;
		tstring filenameToRemove = helpers::getFormattedTime(filenamePattern, timeToRemove, false);
		loglog.debug(LOG4CPLUS_TEXT("Removing file ") + filenameToRemove);
		file_remove(filenameToRemove);
	}

	lastHeartBeat = time;
}


}



