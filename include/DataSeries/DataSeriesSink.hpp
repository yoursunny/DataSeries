// -*-C++-*-
/*
   (c) Copyright 2003-2011, Hewlett-Packard Development Company, LP

   See the file named COPYING for license details
*/

/** @file
    Class for writing DataSeries files.
*/

#ifndef DATASERIES_SINK_HPP
#define DATASERIES_SINK_HPP

#include <Lintel/Deque.hpp>
#include <Lintel/HashUnique.hpp>
#include <Lintel/PThread.hpp>

#include <DataSeries/Extent.hpp>
#include <DataSeries/ExtentField.hpp>

/** \brief Writes Extents to a DataSeries file.
  */
class DataSeriesSink {
public:
    // sundry statistics on this sink; expect to get one
    // non-compressed chunk (the fixed record pointers for the extent
    // type information); also, empty variable-sized extents are not
    // counted in the compress_* stats
    class Stats {
    public:
	Stats(const Stats &from) {
	    *this = from;
	}
	Stats & operator=(const Stats &from);
	// If anything other than plain old data gets put in here, 
	// then the assignment operator needs to be fixed.
        /** The total number of Extents written */
	uint32_t extents;
	uint32_t
            /** The number of Extents that were not compressed at all */
            compress_none,
            /** The number of Extents that were compressed using lzo */
            compress_lzo,
            /** The number of Extents that were compressed using gzip */
            compress_gzip, 
            /** The number of Extents that were compressed using bzip2 */
	    compress_bz2,
            /** The number of Extents that were compressed using lzf */
            compress_lzf;
	uint64_t
            /** The total number of bytes in the Extents before compression. */
            unpacked_size,
            /** The number of bytes in fixed size records before
                compression. */
            unpacked_fixed,
            /** The number of bytes in the Extents' string pools
                before compression. */
            unpacked_variable,
            /** The number of bytes in the Extents' string pools
                before any processing.  In particular it will
                include duplicate fields marked with pack_unique. */
	    unpacked_variable_raw,
            /** The total number of bytes after compression. */
            packed_size,
	    /** The number of records that were written */
	    nrecords;

	/** The time spent packing/compressing Extents.  This is
            supposed to be the thread time, rather than wall clock time.
            The pack_time statistic has been disabled pending figuring
	    out how to make it accurate.  For mor discussion, see
	    DataSeriesFile.C:get_thread_cputime() */
	double pack_time; 

        /** Initializes all statistics to 0. */
	Stats() {
	    reset();
	}
	~Stats();
        /** Reset all statistics to 0. */
	void reset();
	Stats & operator+=(const Stats &from);
	Stats & operator-=(const Stats &from);

	/// specify an extent type if this statistic was entirely for one
	/// type.
	void printText(std::ostream &to, const std::string &extent_type = "");

    private:
	uint32_t use_count; // how many updates are pending to this stats?
	// You should grab the lock before calling this
	void update(uint32_t unpacked_size, uint32_t unpacked_fixed,
		    uint32_t unpacked_variable_raw,
		    uint32_t unpacked_variable, uint32_t packed_size,
		    uint32_t packed_variable_size, size_t nrecords, double pack_time, 
		    unsigned char fixed_compress_mode,
		    unsigned char variable_compress_mode);
	void updateCompressMode(unsigned char compress_mode);
	friend class DataSeriesSink;
    };

    /** Create a new DataSeriesSInk (output file), but leave it closed

        \arg compression_modes Indicates which compression
            algorithms should be tried.  See \link Extent_compress Extent::compress \endlink

        \arg compression_level Should be between 1 to 9 inclusive. The default of
            9 gives the best compression in general.  See the documentation of the
            underlying compression libraries for detail. 
            
    */
    explicit DataSeriesSink(int compression_modes = Extent::compress_all,
                            int compression_level = 9);

    /** Create a new DataSeriesSink (output file), and open \arg filename

        \arg filename is the name of the file to write to.

        \arg compression_modes Indicates which compression
            algorithms should be tried.  See \link Extent_compress Extent::compress \endlink

        \arg compression_level Should be between 1 to 9 inclusive. The default of
            9 gives the best compression in general.  See the documentation of the
            underlying compression libraries for detail. 
            
    */
    explicit DataSeriesSink(const std::string &filename,
                            int compression_modes = Extent::compress_all,
                            int compression_level = 9);


    /** automatically calls close() if close has not already been called. */
    ~DataSeriesSink();

    typedef boost::function<void (off64_t, Extent &)> ExtentWriteCallback;
    /** Sets a callback function for when extents are written out. */
    void setExtentWriteCallback(const ExtentWriteCallback &callback);

    /** Opens a closed data series file with the specified filename */
    void open(const std::string &filename);

    /** Blocks until all queued extents have been written and closes the file.  If to_update is not
        NULL, it will copy the final statistics for the file into that object before returning.  An
        @c ExtentTypeLibrary must have been written using \link DataSeriesSink::writeExtentLibrary
        writeExtentLibrary \endlink. */
    void close(bool do_fsync = false, Stats *to_update = NULL);

    /** Rotates the data series file to a new file, and writes out the Extent Library to the
        beginning of that file.  This function is only valid to call inside of an extent write
        callback.  If you need to rotate the file outside of a continuing stream of extents, then
        you can do so using close(), open(), and writeExtentLibrary().  The reason this function
        needs to be separate is close() needs to drain the set of extents, and shut down the
        processing pipeline.  rotate() needs to keep the processing pipeline in progress and slot
        in a new extent to get written at the head of the pipeline.   The call can optionally
        fsync the old file, and can optionally update specified statistics before they are reset. */
    void rotate(const std::string &new_filename, const ExtentTypeLibrary &library, 
                bool do_fsync = false, Stats *to_update = NULL);

    /** Writes the ExtentTypes that are used in the file.  This function
        must be called exactly once before calling writeExtent.  The
        @c ExtentTypeLibrary must contain all of the types of Extents
        that will be used.  It is ok if it contains ExtentTypes that
        are not used. */
    void writeExtentLibrary(const ExtentTypeLibrary &lib);

    /** Add an @c Extent to the write queue.

        writeExtent is thread safe, although obviously the precise
        order of writing the extents can't be guarenteed if two
        writeExtent calls are executing at the same time.

        \arg e writeExtent is destructive, the contents of e will be
            moved (not copied) to the queue, so e will be empty on return.
        
        \arg toUpdate If toUpdate is not NULL, it will be updated
            with statistics about writing e, when e is actually written.
            
        \warning A copy of the pointer toUpdate is saved, so the pointee must
            not be destroyed, without either closing the file or calling
            \link DataSeriesSink::removeStatsUpdate removeStatsUpdate \endlink
            first.  Also the stats are updated under a per sink lock, so should
            not be shared across multiple sinks.
            
        \pre \link DataSeriesSink::writeExtentLibrary writeExtentLibrary
            \endlink must have been called and the @c ExtentTypeLibrary passed
            to it must contain the type of e. */
    // TODO: consider whether to drop toUpdate; we have stats associated with
    // each sink, you can get it safely with getStats, and this approach leads to way
    // more potential unsafety in the locking/sharing rules, including the need for
    // getStats() to have an option to take a Stats object.
    void writeExtent(Extent &e, Stats *toUpdate);

    /** Block until all Extents in the queue have been written.
        If another thread is writing extents at the same time, this could
        wait forever. */
    void flushPending() {
        worker_info.flushPending(mutex);
    }

    /** Returns combined stats for all the Extents that have been written so far.  (Meaning
        actually written, not just queued).  You are only guaranteed to have all the statistics for
        writing this file after you call close().  If you have passed a stats object into the sink
        when writing extents, you can get a copy of it by specifying that pointer, and using the
        same lock a copy of the stats will be made */
    Stats getStats(Stats *from = NULL);

    /** Updates the stats object adding the unpacked vdata size in protected by the appropriate
        mutex */
    void updateUnpackedVariableRaw(Stats &stats, size_t unpacked_vdata_size);

    /** If you are going to delete a Stats that you previously used to
        write to a DataSeriesFile before you close the file, you need
        to call this. */
    void removeStatsUpdate(Stats *would_update);

    static void verifyTail(ExtentType::byte *data, bool need_bitflip,
			   const std::string &filename);
    
    /** Sets the number of threads that each @c DataSeriesSink uses to
        compress Extents.
        compressor_count == -1 ==> use # cpus. 0 ==> no threading;
        Only affects \link DataSeriesSink DataSeriesSinks \endlink
        created after a call. */
    static void setCompressorCount(int compressor_count = -1);

    const std::string &getFilename() const {
	return filename;
    }

    void setMaxBytesInProgress(size_t nbytes) {
        worker_info.setMaxBytesInProgress(mutex, nbytes);
    }

private:
    struct toCompress {
	Extent extent;
	Stats *to_update;
	bool in_progress;
	uint32_t checksum;
	Extent::ByteArray compressed;
	toCompress(Extent &e, Stats *_to_update)
	    : extent(e.type), to_update(_to_update), 
	      in_progress(false), checksum(0) 
	{
	    extent.swap(e);
	}
	void wipeExtent() {
	    Extent tmp(extent.type);
	    extent.swap(tmp);
	}
	bool readyToWrite() {
	    return compressed.size() > 0 && !in_progress;
	}
    };

    // Structure for shared information among all workers.
    struct WorkerInfo {
        // protected by the standard mutex, users should have separate access to it.
        bool keep_going;
        size_t bytes_in_progress, max_bytes_in_progress;
        Deque<toCompress *> pending_work;

        std::vector<PThread *> compressors;
        PThread *writer;
        PThreadCond available_queue_cond, available_work_cond, available_write_cond;
        WorkerInfo(size_t max_bytes_in_progress)
            : keep_going(false), bytes_in_progress(0), max_bytes_in_progress(max_bytes_in_progress),
              pending_work(), compressors(), writer(), available_queue_cond(),
              available_work_cond(), available_write_cond()
        { }

        bool canQueueWork() {
            return bytes_in_progress < max_bytes_in_progress &&
                pending_work.size() < 2 * compressors.size();
        }
        void startThreads(PThreadScopedLock &lock, DataSeriesSink *sink);
        void stopThreads(PThreadScopedLock &lock);
        void setMaxBytesInProgress(PThreadMutex &mutex, size_t nbytes);
        void flushPending(PThreadMutex &mutex);
        bool frontReadyToWrite() { // Assume lock is held
            if (pending_work.empty()) {
                return false;
            } else {
                return pending_work.front()->readyToWrite();
            }
        }
        bool isQuiesced() {
            return keep_going == false && bytes_in_progress == 0 && pending_work.empty()
                && compressors.empty() && writer == NULL;
        }
    };

    // Structure for the writer.
    struct WriterInfo {
        int fd;
        bool wrote_library, in_callback;
        off64_t cur_offset; // set to -1 when sink is closed
        uint32_t chained_checksum; 
        ExtentSeries index_series;
        Extent index_extent;
        Int64Field field_extentOffset;
        Variable32Field field_extentType;
        boost::function<void (off64_t, Extent &)> extent_write_callback;

        WriterInfo()
            : fd(-1), wrote_library(false), in_callback(false), cur_offset(-1), chained_checksum(0),
              index_series(ExtentType::getDataSeriesIndexTypeV0()), 
              index_extent(*index_series.getType()),
              field_extentOffset(index_series,"offset"),
              field_extentType(index_series,"extenttype"), 
              extent_write_callback()
        { }
        void writeOutPending(PThreadScopedLock &lock, WorkerInfo &worker_info);
        void checkedWrite(const void *buf, int bufsize);
        bool isQuiesced() {
            return fd == -1 && wrote_library == false && cur_offset == -1
                && index_series.getExtent() == NULL && chained_checksum == 0
                && index_extent.size() == 4;
        }
    };

    void checkedWrite(const void *buf, int bufsize) {
        writer_info.checkedWrite(buf, bufsize);
    }
    void writeExtentType(ExtentType &et);

    void queueWriteExtent(Extent &e, Stats *to_update);
    void lockedProcessToCompress(PThreadScopedLock &lock, toCompress *work);

    static int compressor_count;

    Stats stats;
    PThreadMutex mutex; // this mutex is ordered after Stats::getMutex(), so grab it second if you need both.
    HashUnique<const ExtentType *, lintel::PointerHash<ExtentType>,
               lintel::PointerEqual<ExtentType> > valid_types;
    const int compression_modes;
    const int compression_level;

    WriterInfo writer_info;
    WorkerInfo worker_info;
				   
    std::string filename;
    friend class DataSeriesSinkPThreadCompressor;
    void compressorThread();
    friend class DataSeriesSinkPThreadWriter;
    void writerThread();
};

inline DataSeriesSink::Stats 
operator-(const DataSeriesSink::Stats &a, const DataSeriesSink::Stats &b) {
    DataSeriesSink::Stats ret = a;
    ret -= b;
    return ret;
}

#endif
