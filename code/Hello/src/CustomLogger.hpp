#ifndef SRC_netcustomlog4cplus_HPP_
#define SRC_netcustomlog4cplus_HPP_

#include "log4cplus/fileappender.h"
#include "log4cplus/logger.h"
#include "log4cplus/loggingmacros.h"
#include "log4cplus/socketappender.h"

namespace customlog4cplus
{

class CustomDailyRollingFileAppender : public log4cplus::DailyRollingFileAppender {
	static const long DEFAULT_ROLLING_LOG_SIZE = 10 * 1024 * 1024L;
	static const long MINIMUM_ROLLING_LOG_SIZE = 200*1024L;
public:
  // Ctors
	CustomDailyRollingFileAppender(const log4cplus::tstring& filename,
			log4cplus::DailyRollingFileSchedule schedule = log4cplus::DAILY,
			long maxFileSize_ = 10*1024*1024, // 10 MB
			int maxBackupIndex_ = 10,
			bool immediateFlush = true,
			bool createDirs = false,
			bool rollOnClose = true,
			const log4cplus::tstring& datePattern = log4cplus::tstring());
  // Dtor
//	virtual ~CustomDailyRollingFileAppender(){}//父类的资源会自己析构
  // Methods
//	virtual void close();
protected:
	virtual void append(const log4cplus::spi::InternalLoggingEvent& event)override;

	 // Data
	long maxFileSize;
private:
//	LOG4CPLUS_PRIVATE void init(DailyRollingFileSchedule schedule);
};

class FixDailyRollingFileAppender : public log4cplus::FileAppenderBase {
	static const long DEFAULT_ROLLING_LOG_SIZE = 10 * 1024 * 1024L;
	static const long MINIMUM_ROLLING_LOG_SIZE = 20*1024L;
public:
	  // Ctors
	FixDailyRollingFileAppender(
				const log4cplus::tstring& filename,const log4cplus::tstring& fileExt_,
				log4cplus::DailyRollingFileSchedule schedule = log4cplus::HOURLY,
				long maxFileSize_ = 10*1024*1024, int maxBackupIndex_ = 10,int maxHistory_ = 48,
				int startTryCleanFile_ = 1,int maxTryCleanFile_ = 3,
				bool immediateFlush = true,bool createDirs = false,bool rollOnClose = false,
				const log4cplus::tstring& datePattern = log4cplus::tstring());
  // Dtor
	virtual ~FixDailyRollingFileAppender();

  // Methods
	virtual void close();

	// Data
	log4cplus::tstring filenamepre;

	long maxFileSize;

	log4cplus::tstring fileExt = ".data";
	bool boFileCanOpen = false;

	long maxHistory = 48;//schedule

	int startTryCleanFile = 1;
	int maxTryCleanFile = 3;
protected:
	virtual void append(const log4cplus::spi::InternalLoggingEvent& event);
	void rollover(bool alreadyLocked = false);
	log4cplus::helpers::Time calculateNextRolloverTime(const log4cplus::helpers::Time& t) const;
	log4cplus::tstring getFilename(const log4cplus::helpers::Time& t) const;
	int getRolloverPeriodDuration() const;
	void clean();

  // Data
	log4cplus::DailyRollingFileSchedule schedule;
	log4cplus::tstring scheduledFilename;
	log4cplus::helpers::Time nextRolloverTime;
	int maxBackupIndex;
	bool rollOnClose;
	log4cplus::tstring datePattern;

private:
	LOG4CPLUS_PRIVATE void init(log4cplus::DailyRollingFileSchedule schedule);
};

class CustomTimeBasedRollingFileAppender : public log4cplus::TimeBasedRollingFileAppender {
	static const long DEFAULT_ROLLING_LOG_SIZE = 10 * 1024 * 1024L;
	static const long MINIMUM_ROLLING_LOG_SIZE = 200*1024L;
public:
  // Ctors
	CustomTimeBasedRollingFileAppender(
			const log4cplus::tstring& filename_,
			const log4cplus::tstring& filenamePattern_,//%d{yyyy-MM-dd-HH}.data
			long maxFileSize_ , // 10 MB
			long maxFileIndex_,
			int maxHistory_ = 24 * 7,//
			const log4cplus::tstring& fileExt_ = ".data",
			bool cleanHistoryOnStart_ = false,
			bool bRoutineCleanHistory_ = false,
			bool immediateFlush_ = true,//true
			bool createDirs_ = false,//true
			bool rollOnClose_ = false);
  // Dtor
//	virtual ~CustomDailyRollingFileAppender(){}//父类的资源会自己析构
  // Methods
//	virtual void close();
protected:
	virtual void append(const log4cplus::spi::InternalLoggingEvent& event)override;
	void rollover(bool alreadyLocked = false);

	 // Data
	long maxFileSize;
	long maxFileIndex;
	bool routineCleanHistory;
	log4cplus::tstring fileExt = ".data";
	log4cplus::tstring tmpFilename;
	bool boFileOpen = false;
private:
//	LOG4CPLUS_PRIVATE void init(DailyRollingFileSchedule schedule);
	LOG4CPLUS_PRIVATE void init();
	void clean(log4cplus::helpers::Time time);
};



} // end namespace log4cplus

#endif
