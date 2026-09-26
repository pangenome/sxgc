//
//  utils.hpp
//
//  Copyright 2020 Marco Oliva. All rights reserved.
//

#ifndef utils_hpp
#define utils_hpp

#include <string>
#include <string_view>
#include <vector>
#include <mutex>
#include <set>
#include <limits>
#include <functional>
#include <unordered_set>
#include <map>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <list>
#include <forward_list>
#include <unordered_map>
#include <cstring>

#include <unistd.h>
#include <cstdint>
#include <cmath>

#include <spdlog/spdlog.h>

#include <sdsl/io.hpp>  // serialize and load
#include <type_traits>  // enable_if_t and is_fundamental

extern "C" {
#include <gsacak.h>
}

namespace pfpds
{

#define Dollar 2     // special char for the parsing algorithm, must be the highest special char
#define EndOfWord 1  // word delimiter for the plain dictionary file
#define EndOfDict 0  // end of dictionary delimiter

typedef uint64_t            long_type;
typedef uint32_t            short_type;
typedef int64_t             long_signed_type;
typedef int32_t             short_signed_type;
typedef uint8_t             char_type;
typedef long_type           size_type;
typedef long_signed_type    signed_type;
typedef long_type           hash_type;

constexpr std::size_t KILOBYTE      = 1024;
constexpr std::size_t MEGABYTE      = KILOBYTE * KILOBYTE;
constexpr std::size_t GIGABYTE      = KILOBYTE * MEGABYTE;

constexpr double KILOBYTE_DOUBLE    = 1024.0;
constexpr double MILLION_DOUBLE     = 1000000.0;
constexpr double MEGABYTE_DOUBLE    = KILOBYTE_DOUBLE * KILOBYTE_DOUBLE;
constexpr double GIGABYTE_DOUBLE    = KILOBYTE_DOUBLE * MEGABYTE_DOUBLE;

constexpr std::size_t MILLION       = 1000000;
constexpr std::size_t BILLION       = 1000 * MILLION;

namespace EXT
{

const std::string PARSE = ".parse";
const std::string DICT = ".dict";
const std::string DICT_COMPRESSED = ".dicz";
const std::string DICT_COMPRESSED_LENGTHS = ".dicz.len";
const std::string OCC = ".occ";
const std::string LAST = ".last";
const std::string SAI = ".sai";
const std::string N_PARSE = ".aup.parse";
const std::string N_DICT = ".aup.dict";
const std::string N_OCC = ".aup.occ";
const std::string N_LAST = ".aup.last";
const std::string N_SAI = ".aup.sai";
const std::string N_DICT_COMPRESSED = ".aup.dicz";
const std::string N_DICT_COMPRESSED_LENGTHS = ".aup.dicz.len";

}


//------------------------------------------------------------------------------

inline double
inMegabytes(std::size_t bytes)
{
    return bytes / MEGABYTE_DOUBLE;
}

inline double
inGigabytes(std::size_t bytes)
{
    return bytes / GIGABYTE_DOUBLE;
}

inline std::size_t pid()
{
#ifdef MSVC_COMPILER
    return _getpid();
#else
    return getpid();
#endif
}

//------------------------------------------------------------------------------

/*
  Temporary file names have the pattern "prefix_hostname_pid_counter", where
  - prefix is given as an argument to getName();
  - hostname is the name of the host;
  - pid is the process id; and
  - counter is a running counter starting from 0.
  The generated names are stored until the file is deleted with remove(). All
  remaining temporary files are deleted when the program exits (normally or
  with std::exit()).
  TempFile is thread-safe.
*/

namespace TempFile
{
extern const std::string DEFAULT_TEMP_DIR;
extern std::string temp_dir;

void setDirectory(const std::string& directory);
std::string getName(const std::string& name_part);
void remove(std::string& filename);  // Also clears the filename.
}

void truncate_file(std::string file_name, std::size_t new_size_in_bytes);

bool is_gzipped(std::ifstream& in);

//------------------------------------------------------------------------------

// Disk space statistics
namespace DiskWrites
{
void update(std::size_t num_of_bytes);
};

//------------------------------------------------------------------------------

template <typename data_type>
void gsacak_templated(data_type *s, long_type *SA, long_type n, long_type k = 0)
{
    // NOP
    spdlog::error("Not executing gsacak, wrong template used."); std::exit(1);
}

template<>
void gsacak_templated<uint8_t> (uint8_t *s, long_type *SA, long_type n, long_type k)
{
    gsacak(s, SA, nullptr, nullptr, n);
};

template<>
void gsacak_templated<char> (char *s, long_type *SA, long_type n, long_type k)
{
    gsacak((unsigned char*) s, SA, nullptr, nullptr, n);
};

template<>
void gsacak_templated<uint32_t> (uint32_t *s, long_type *SA, long_type n, long_type k)
{
    gsacak_int(s, SA, nullptr, nullptr, n, k);
};

//------------------------------------------------------------------------------

template<typename T>
void read_file(const char *filename, T*& ptr, size_t& length){
    struct stat filestat;
    FILE* fd;
    
    if ((fd = fopen(filename, "r")) == nullptr)
        spdlog::error("open() file " + std::string(filename) + " failed" );

    int fn = fileno(fd);
    if (fstat(fn, &filestat) < 0)
        spdlog::error("stat() file " + std::string(filename) + " failed" );

    if(filestat.st_size % sizeof(T) != 0)
        spdlog::error("invilid file " + std::string(filename));

    length = filestat.st_size / sizeof(T);
    ptr = new T[length];

    if ((fread(ptr, sizeof(T), length, fd)) != length)
        spdlog::error("fread() file " + std::string(filename) + " failed");

    fclose(fd);
}

template<typename T>
void read_file(const char *filename, std::vector<T>& ptr){
    struct stat filestat;
    FILE* fd;

    if ((fd = fopen(filename, "r")) == nullptr)
        spdlog::error("open() file " + std::string(filename) + " failed" );

    int fn = fileno(fd);
    if (fstat(fn, &filestat) < 0)
        spdlog::error("stat() file " + std::string(filename) + " failed" );

    if(filestat.st_size % sizeof(T) != 0)
        spdlog::error("invilid file " + std::string(filename));

    size_t length = filestat.st_size / sizeof(T);
    ptr.resize(length);

    if ((fread(&ptr[0], sizeof(T), length, fd)) != length)
        spdlog::error("fread() file " + std::string(filename) + " failed");

    fclose(fd);
}

//------------------------------------------------------------------------------

//********** begin my serialize edit from sdsl ********************
// Those are wrapper around most of the serialization functions of sdsl

//! Serialize each element of an std::vector
/*!
 * \param vec The vector which should be serialized.
 * \param out Output stream to which should be written.
 * \param v   Structure tree node. Note: If all elements have the same
 *            structure, then it is tried to combine all elements (i.e.
 *            make one node w with size set to the cumulative sum of all
 *           sizes of the children)
 */
// specialization for fundamental types
template <class T>
uint64_t
my_serialize_vector(const std::vector<T> &vec, std::ostream &out, sdsl::structure_tree_node *v, std::string name, typename std::enable_if<std::is_fundamental<T>::value>::type * = 0)
{
    if (vec.size() > 0)
    {
        sdsl::structure_tree_node *child = sdsl::structure_tree::add_child(v, name, "std::vector<" + sdsl::util::class_name(vec[0]) + ">");
        size_t written_bytes = 0;

        const T *p = &vec[0];
        typename std::vector<T>::size_type idx = 0;
        while (idx + sdsl::conf::SDSL_BLOCK_SIZE < (vec.size()))
        {
            out.write((char *)p, sdsl::conf::SDSL_BLOCK_SIZE * sizeof(T));
            written_bytes += sdsl::conf::SDSL_BLOCK_SIZE * sizeof(T);
            p += sdsl::conf::SDSL_BLOCK_SIZE;
            idx += sdsl::conf::SDSL_BLOCK_SIZE;
        }
        out.write((char *)p, ((vec.size()) - idx) * sizeof(T));
        written_bytes += ((vec.size()) - idx) * sizeof(T);

        sdsl::structure_tree::add_size(child, written_bytes);
        return written_bytes;
    }
    else
    {
        return 0;
    }
}

template <typename X>
uint64_t
my_serialize(const std::vector<X> &x,
             std::ostream &out, sdsl::structure_tree_node *v = nullptr,
             std::string name = "", typename std::enable_if<std::is_fundamental<X>::value>::type * = 0)
{
    return sdsl::serialize(x.size(), out, v, name) + my_serialize_vector(x, out, v, name);
}

//! Load all elements of a vector from a input stream
/*! \param vec  Vector whose elements should be loaded.
 *  \param in   Input stream.
 *  \par Note
 *   The vector has to be resized prior the loading
 *   of its elements.
 */
template <class T>
void my_load_vector(std::vector<T> &vec, std::istream &in, typename std::enable_if<std::is_fundamental<T>::value>::type * = 0)
{
    T *p = &vec[0];
    typename std::vector<T>::size_type idx = 0;
    while (idx + sdsl::conf::SDSL_BLOCK_SIZE < (vec.size()))
    {
        in.read((char *)p, sdsl::conf::SDSL_BLOCK_SIZE * sizeof(T));
        p += sdsl::conf::SDSL_BLOCK_SIZE;
        idx += sdsl::conf::SDSL_BLOCK_SIZE;
    }
    in.read((char *)p, ((vec.size()) - idx) * sizeof(T));
}

template <typename X>
void my_load(std::vector<X> &x, std::istream &in, typename std::enable_if<std::is_fundamental<X>::value>::type * = 0)
{
    typename std::vector<X>::size_type size;
    sdsl::load(size, in);
    x.resize(size);
    my_load_vector(x, in);
}

//------------------------------------------------------------------------------

/*!
 * op the operation that we want to measure
 */
#define _elapsed_time(op)                                                                                                       \
  ({                                                                                                                            \
    std::chrono::high_resolution_clock::time_point t_insert_start = std::chrono::high_resolution_clock::now();                  \
    op;                                                                                                                         \
    std::chrono::high_resolution_clock::time_point t_insert_end = std::chrono::high_resolution_clock::now();                    \
    spdlog::info("Elapsed time (s): {}", std::chrono::duration<double, std::ratio<1>>(t_insert_end - t_insert_start).count());  \
    std::chrono::duration<double, std::ratio<1>>(t_insert_end - t_insert_start).count();                                        \
  })

//------------------------------------------------------------------------------

//*********************** Kasai et al. LCP construction algorithm ***************************************
template<typename T, typename S, typename lcp_t>
void LCP_array(S* s, const std::vector<T>& isa, const std::vector<T>& sa, size_t n, std::vector<lcp_t>& lcp){
    lcp[0]  = 0;
    
    T l = 0;
    for (size_t i = 0; i < n; ++i){
        // if i is the last character LCP is not defined
        T k = isa[i];
        if(k > 0){
            T j = sa[k-1];
            // I find the longest common prefix of the i-th suffix and the j-th suffix.
            while(s[i+l] == s[j+l]) l++;
            // l stores the length of the longest common prefix between the i-th suffix and the j-th suffix
            lcp[k] = l;
            if(l>0) l--;
        }
    }
}

//*********************** Kasai et al. LCP construction algorithm rec text ***************************************
template<typename T, typename S, typename lcp_t>
void LCP_array_cyclic_text(S* s, const std::vector<T>& isa, const std::vector<T>& sa, size_t n, std::vector<lcp_t>& lcp){
    lcp[0] = 0;

    T l = 0;
    for (size_t i = 0; i < n; ++i){
        // if i is the last character LCP is not defined
        T k = isa[i];
        if(k > 0){
            T j = sa[k-1];
            // I find the longest common prefix of the i-th suffix and the j-th suffix.
            while(l <= n && s[(i+l) % n] == s[(j+l) % n]) l++;
            // l stores the length of the longest common prefix between the i-th suffix and the j-th suffix
            lcp[k] = l;
            if(l>0) l--;
        }
    }
}

//------------------------------------------------------------------------------

} // end namespace pfpds

#endif // utils_hpp