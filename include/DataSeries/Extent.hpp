
// -*-C++-*-
/*
  (c) Copyright 2003-2005, Hewlett-Packard Development Company, LP

  See the file named COPYING for license details
*/

/** @file
    Variable sized chunks of data; containers for the individual
    records that are stored within, typed, and aligned to 8 byte
    boundaries
*/

#ifndef DATASERIES_EXTENT_H
#define DATASERIES_EXTENT_H

extern "C" {
    char *dataseriesVersion();
}

#include <unistd.h>
#include <inttypes.h>
#include <cstring>

#if defined(__linux__) && defined(__GNUC__) && __GNUC__ >= 2 
#  ifdef __i386__
#    if defined __i486__ || defined __pentium__ || defined __pentiumpro__ || defined __pentium4__ || defined __k6__ || defined __k8__ || defined __athlon__ 
// ok, will get the good byteswap
#    else
// Note that in particular __i386__ is not enough to have defined here
// the swap routine is significantly worse than the i486 version; see
// /usr/include/bits/byteswap.h.  See the test_byteflip part of test.C
// for the performance test, it will fail unless you've selected the
// best of the routines
#      warning "Automatically defining __i486__ on the assumption you have at least that type of CPU"
#      warning "Not doing this gets a much slower byte swap routine"
#      define __i486__ 1
#    endif
#  endif
#  include <byteswap.h>
#endif

#if (defined(__FreeBSD__)||defined(__OpenBSD__)) && defined(__GNUC__) && __GNUC__ >= 2 && (defined(__i486__) || defined (__x86_64__))
#  define bswap_32(x)                                                   \
    (__extension__                                                      \
     ({ register uint32_t bswap32_out, bswap32_in = (x);                \
         __asm__("bswap %0" : "=r" (bswap32_out) : "0" (bswap32_in));   \
         bswap32_out; }))
#endif

#include <boost/enable_shared_from_this.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/utility.hpp>

#include <Lintel/CompilerMarkup.hpp>
#include <Lintel/TypeCompat.hpp>

#include <DataSeries/ExtentType.hpp>


// Defined and explained in DataSeriesSink.cpp
extern const int MAX_THREADS;

class ExtentSeries;

/** \brief Stores a sequence of records having the same type.
 *
 * Each @c Extent has two components
 *   - A buffer containing fixed size records.  The size of
 *     each individual record can be determined from the ExtentType,
 *     allowing random access.  Variable32 fields are represented
 *     by a 4 byte index into the string pool.
 *   - A string pool for variable32 fields. */
class Extent : boost::noncopyable, public boost::enable_shared_from_this<Extent> {
  public:
    typedef boost::shared_ptr<Extent> Ptr;

    typedef ExtentType::byte byte;
    typedef ExtentType::int32 int32;
    typedef ExtentType::uint32 uint32;
    typedef ExtentType::int64 int64;
    // G++-3 changed the way vectors work to remove the clear
    // similarity between them and an array; since they could be
    // implemented as a two level lookup (that's still constant time),
    // and using them to get to the byte array is now more difficult,
    // we implement our own to guarentee the assumption that we are
    // really representing a variable sized array of bytes, which is
    // what we want.  Later change to the C++ spec clarified that 
    // arrays have to be contiguous, so we could switch back at some point.
    class ByteArray {
      public:
        ByteArray() { beginV = endV = maxV = NULL; }
        ~ByteArray();
        size_t size() const { return endV - beginV; }
        void resize(size_t newsize, bool zero_it = true) {
            size_t oldsize = size();
            if (newsize <= oldsize) {
                // shrink
                endV = beginV + newsize;
            } else if (newsize < static_cast<size_t>(maxV - beginV)) {
                endV = beginV + newsize;
                if (zero_it) {
                    memset(beginV + oldsize,0,newsize - oldsize);
                }
            } else { 
                copyResize(newsize, zero_it);
            } 
        }
        void clear(); // frees allocated memory
        void reserve(size_t reserve_bytes);
        bool empty() const { return endV == beginV; }
        byte *begin() const { return beginV; };
        byte *begin(size_t offset) const { return beginV + offset; }
        byte *end() const { return endV; };
        byte &operator[] (size_t offset) const { return *(begin(offset)); }
    
        void swap(ByteArray &with) {
            swap(beginV,with.beginV);
            swap(endV,with.endV);
            swap(maxV,with.maxV);
        }
      
        typedef byte * iterator;
      
        // glibc prefers to use mmap to allocate large memory chunks; we 
        // allocate and de-allocate those fairly regularly; so we increase
        // the threshold.  This function is automatically called once when
        // we have to resize a bytearray, if you disagree with the defaults,
        // you can call this manually and set the options yourself.
        // if you want to look at the retaining space code again, version 
        // d5bb884b572b07590a8131710f01513577e24813, prior to 2007-10-25
        // will have a copy of the old code.
        static void initMallocTuning();
      private:
        void swap(byte * &a, byte * &b) {
            byte *tmp = a;
            a = b;
            b = tmp;
        }
      
        void copyResize(size_t newsize, bool zero_it);
        byte *beginV, *endV, *maxV;
    };
  
    /// \cond INTERNAL_ONLY
    const ExtentType::Ptr type;
    /// \endcond

    /** Returns the type of the Extent; reference valid until extent is destroyed. */
    const ExtentType &getType() const FUNC_DEPRECATED {
        return *type;
    }

    /** Returns the type of the Extent */
    const ExtentType::Ptr getTypePtr() const {
        return type;
    }

    /** This constructor creates an @c Extent from raw bytes in
        the external format created by packData.  It will
        Extract the name of the appropriate @c ExtentType from
        the input bytes and look it up in the given @c ExtentTypeLibrary.
        If needs_bitflip is true, it indicates that the endianness
        of the host processor is opposite the endianness of the
        input data. */
    Extent(const ExtentTypeLibrary &library, Extent::ByteArray &packeddata, 
           const bool need_bitflip); // TODO: consider deprecating.
    /** Similar to the above constructor, except that the ExtentType is passed explicitly.
        deprecated, just call unpackData directly after making the extent. */
    Extent(const ExtentType &type, Extent::ByteArray &packeddata, const bool need_bitflip) FUNC_DEPRECATED;
    /** Creates an empty @c Extent. */
    Extent(const ExtentType &type) FUNC_DEPRECATED;

    /** Creates an empty @c Extent. */
    Extent(const ExtentType::Ptr type);

    /** Creates an empty @c Extent. */
    Extent(const std::string &xmltype); 
    
    /** Use ExtentSeries::newExtent() instead of this function.  Creates a new empty Extent. If
        series.extent() == NULL, the Extent creation will set the series' extent to the new
        extent.  */
    Extent(ExtentSeries &myseries) FUNC_DEPRECATED;

    /** Destroy the extent, verifies that there are no shared pointers to the extent */
    ~Extent();

    /** Swap the contents of two &c Extent. 

        Preconditions:
        - Both Extents must have the same type. 

        Provided the preconditions are met, this
        function does not throw. */
    void swap(Extent &with);

    /** Clears the contents of the Extent.  Note that
        there is no way to clear the type. */
    void clear() {
        fixeddata.clear();
        variabledata.clear();
        init();
    }
    /** Returns the total size of the @c Extent in bytes. */
    size_t size() {
        return fixeddata.size() + variabledata.size();
    }
    
    /** Returns the number of records in this Extent */
    size_t nRecords() {
        return fixeddata.size() / getTypePtr()->fixedrecordsize();
    }

    /// \cond INTERNAL_ONLY
    /** The following are defined by the file format and can't be changed.
        These determine the index of each compression algorithm in the
        compression_algs[] array */
    static const Extent::byte compress_mode_none = 0;
    static const Extent::byte compress_mode_lzo = 1;
    static const Extent::byte compress_mode_zlib = 2;
    static const Extent::byte compress_mode_bz2 = 3;
    static const Extent::byte compress_mode_lzf = 4;
    static const Extent::byte compress_mode_snappy = 5;
    static const Extent::byte compress_mode_lz4 = 6;
    static const Extent::byte compress_mode_lz4hc = 7;
    /// \endcond

    // Should be equal to the number of constants compress_mode_{name}
    // specified above.  Careful: because of the way the compression bit flags
    // are stored in ints, this cannot ever be greater than 16.
    static const int num_comp_algs = 8;

    struct compression_alg 
    {
        const char* name;
        int compress_flag;
        bool (*packFunc)( byte*, int32, ByteArray&, int );
        bool (*unpackFunc)( byte*, byte*, int32, int32& );
    };

    /* This array contains the available compression algorithms */
    static compression_alg compression_algs[];

    /* Compress_all is set to the bitwise or of all the compress flags in compression_algs */
    static const int compress_all = ~( INT_MIN >> ( sizeof(INT_MIN)*8 - num_comp_algs ) );


    /** \defgroup Extent_compress Extent::compress
        The compress_flag ints are used to indicate which compression
        algorithms will be tried when converting an Extent
        to its external representation.  Multiple flags can
        be |'ed together and the algorithm that
        yields the best compression will be chosen.  Which
        algorithms are available depends on which libraries
        are found when DataSeries is compiled.  If an algorithm
        which is not supported is requested, it will be ignored.

        \brief converts an Extent to the external representation which is stored
        in files.

        \arg into Output buffer to write the result to.

        \arg compression_modes Specifies which compression algorithms will be
        tried.  The one that yields the smallest result will be selected.

        \arg compression_level must be between 1 and 9 inclusive.  In general larger
        values give better compression but require more time/space.  See the
        documentation of the individual compression schemes for details.
             
        \arg header_packed If header_packed is not null, *header_packed will recieve
        the pre-compression size of the Extent header.
        \arg fixed_packed If fixed_packed is not null, *fixed_packed will recieve
        the pre-compression size in bytes of the fixed size records.
        \arg variable_packed If variable_packed is not null, *variable_packed
        will recieve the pre-compression size of the string pool.
    
        \return a "checksum" calculated from the underlying checksums in the packed extent */
    uint32_t packData(Extent::ByteArray &into, 
                      uint32_t compression_modes = compress_all,
                      uint32_t compression_level = 9,
                      uint32_t *header_packed = NULL, 
                      uint32_t *fixed_packed = NULL, 
                      uint32_t *variable_packed = NULL); 

    /** Loads an Extent from the external representation.

        \arg need_bitflip Indicates whether the endianness of
        from is the same as the that of the host machine.

        Preconditions:
        - The type of the data must be the type of this Extent.

        Note you can't unpack the same data twice, it may modify the
        input data */
    void unpackData(Extent::ByteArray &from, bool need_bitflip);

    /** Returns true if position is inside the fixed data for this extent, otherwise false */
    bool insideExtentFixed(byte *position) const {
        return position >= fixeddata.begin() && position < fixeddata.end();
    }

    /** Returns the total size in bytes that an Extent created
        using @param from will need.  (This will be the result of size() after
        unpacking.)

        \param from The byte array to use as input
        \param need_bitflip Do we need to flip the byte order when unpacking?
        \param type What type does the raw data represent?
        
        Preconditions:
        - from must be in the external representation of Extents. 
    */
    static uint32_t unpackedSize(Extent::ByteArray &from, bool need_bitflip,
                                 const ExtentType::Ptr type);
    static uint32_t unpackedSize(Extent::ByteArray &from, bool need_bitflip,
                                 const ExtentType &type) FUNC_DEPRECATED {
        return unpackedSize(from, need_bitflip, type.shared_from_this());
    }
    
    /** Returns the name of the type of the Extent stored in @param from
        
        \param from What byte array should we get the type for?
        
        Preconditions:
        - from must be in the external representation of Extents. */
    static const std::string getPackedExtentType(const Extent::ByteArray &from);

    /** All of the pack and unpack functions should be used only through pointers
        which are entered into the compression_alg[] array, and called in
        compressBytes() or uncompressBytes() */

    /// \cond INTERNAL_ONLY
    // most pack routines return false if packing the data with a 
    // particular compression algorithm wouldn't gain anything over leaving
    // the data uncompressed (compressBytes does an overall check for this
    // after compression; if into.size > 0, will only code up to that size
    // for the BZ2 and Zlib packing options (LZO doesn't allow this)
    static bool packBZ2(byte *input, int32 inputsize, 
                        Extent::ByteArray &into, int compression_level);
    static bool packZLib(byte *input, int32 inputsize, 
                         Extent::ByteArray &into, int compression_level);
    static bool packLZO(byte *input, int32 inputsize, 
                        Extent::ByteArray &into, int compression_level);
    static bool packLZF(byte *input, int32 inputsize,
                        Extent::ByteArray &into, int compression_level);
    static bool packSnappy(byte *input, int32 inputsize,
                           Extent::ByteArray &into, int compression_level);
    static bool packLZ4(byte *input, int32 inputsize,
                        Extent::ByteArray &into, int compression_level);
    static bool packLZ4HC(byte *input, int32 inputsize,
                          Extent::ByteArray &into, int compression_level);


    // The unpack functions return true iff uncompression completed successfully
    // without errors.
    static bool unpackBZ2( byte* output, byte *input, 
                           int32 input_size, int32 &output_size );
    static bool unpackZLib( byte* output, byte *input, 
                            int32 input_size, int32 &output_size );
    static bool unpackLZO( byte* output, byte *input, 
                           int32 input_size, int32 &output_size );
    static bool unpackLZF( byte* output, byte *input, 
                           int32 input_size, int32 &output_size );
    static bool unpackSnappy( byte* output, byte *input, 
                              int32 input_size, int32 &output_size );
    static bool unpackLZ4( byte* output, byte *input, 
                           int32 input_size, int32 &output_size );


    static inline uint32_t flip4bytes(uint32 v) {
#if defined(bswap_32)
        return bswap_32(v);
#else
        // fastest method on PIII & PA2.0-- see test.C for a bunch of variants
        // that we chose the best will be verified by test.C
        return ((v >> 24) & 0xFF) | ((v>>8) & 0xFF00) |
                ((v & 0xFF00) << 8) | ((v & 0xFF) << 24);
#endif
    }
    static inline void flip4bytes(byte *data) {
        *(uint32_t *)data = flip4bytes(*(uint32_t *)data);
    }
    // 
    static inline void flip8bytes(byte *data) {
#if defined(bswap_64)
        *(uint64_t *)data = bswap_64(*(uint64_t *)data);
#else
        uint32 a = *(uint32 *)data;
        uint32 b = *(uint32 *)(data + 4);
        *(uint32 *)(data + 4) = flip4bytes(a);
        *(uint32 *)data = flip4bytes(b);
#endif
    }
    /// \endcond
        
    // returns true if it successfully read the extent; returns false 
    // on eof (with into.size() == 0); aborts otherwise
    // updates offset to the end of the extent
    static bool preadExtent(int fd, off64_t &offset, Extent::ByteArray &into, bool need_bitflip);

    // returns true if it read amount bytes, returns false if it read
    // 0 bytes and eof_ok; aborts otherwise
    static bool checkedPread(int fd, off64_t offset, byte *into, int amount, 
                             bool eof_ok = false);

    // The verification checks are the dataseries internal checksums
    // that verify that the files were read correctly.  The
    // pre_uncompress check verifies the checksum over the compressed
    // data.  The post_uncompress check verifies the data after it has
    // been uncompressed, and after some of the transforms
    // (e.g. relative packing) have been applied.  The
    // unpack_variable32 check verifies that
    // Variable32Field::selfcheck() passes for all of the variable32
    // fields.

    // set the environment variable to one or more of:
    // DATASERIES_READ_CHECKS=preuncompress,postuncompress,variable32,all,none
    // This function is automatically called before unpacking the first extent
    // if it hasn't already been called.
    static void setReadChecksFromEnv(bool default_with_env_unset = false);

    /// \cond INTERNAL_ONLY
    // be smart before directly accessing these!  here because making
    // them private and using friend class ExtentSeries::iterator
    // didn't work.
    Extent::ByteArray fixeddata;
    Extent::ByteArray variabledata;

    /// For read-in extents, this will be the filename, for just created
    /// extents this will be in_memory_str.
    std::string extent_source;

    /// Default extent_source
    static const std::string in_memory_str;

    /// For read-in extents, this will be the offset, for just created
    /// extents this will be -1.
    int64_t extent_source_offset;

    // This function is here to verify that we got the right
    // flip4bytes when we compiled DataSeries, it's used by
    // DataSeries/src/test.C
    static void run_flip4bytes(uint32_t *buf, unsigned buflen);

  private:
    // you are responsible for deleting the return buffer
    static Extent::ByteArray *compressBytes(byte *input, int32 input_size,
                                            int compression_modes,
                                            int compression_level, byte *mode);

    static int32 uncompressBytes(byte *into, byte *from,
                                 byte compression_mode, int32 intosize,
                                 int32 fromsize);

    void compactNulls(Extent::ByteArray &fixed_coded);
    void uncompactNulls(Extent::ByteArray &fixed_coded, int32_t &size);
    friend class ExtentSeries;
    void createRecords(unsigned int nrecords); // will leave iterator pointing at the current record
    void init();
    /// \endcond
};

   

#endif
