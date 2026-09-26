/*!
   \file parse.hpp
   \brief parse.hpp define and build the prefix-free parse data structure.
   \author Massimiliano Rossi
   \date 03/04/2020
*/


#ifndef _PFP_PARSE_HH
#define _PFP_PARSE_HH

#include <sdsl/rmq_support.hpp>
#include <sdsl/int_vector.hpp>
#include "utils.hpp"

namespace pfpds
{

// TODO: Extend it to non-integer alphabets
class parse
{
public:
    std::vector<uint32_t> p;
    
    // size depends on input
    sdsl::int_vector<0> saP;
    sdsl::int_vector<0> isaP;
    sdsl::int_vector<0> lcpP;

    sdsl::rmq_succinct_sct<> rmq_lcp_P;
    bool saP_flag = false;
    bool isaP_flag = false;
    bool lcpP_flag = false;
    bool rmq_lcp_P_flag = false;
    
    long_type alphabet_size;
    
    parse(
    std::vector<uint32_t>& p_,
    long_type alphabet_size_,
    bool saP_flag_ = true,
    bool isaP_flag_ = true,
    bool lcpP_flag_ = true,
    bool rmq_lcp_P_flag_ = true):
    p(p_),
    alphabet_size(alphabet_size_)
    {
        assert(p.back() == 0);
        build(saP_flag_, isaP_flag_, lcpP_flag_, rmq_lcp_P_flag_);
    }
    
    parse(
    std::string filename,
    long_type alphabet_size_,
    bool saP_flag_ = true,
    bool isaP_flag_ = false,
    bool lcpP_flag_ = false,
    bool rmq_lcp_P_flag_ = false ):
    alphabet_size(alphabet_size_)
    {
        // Building parse from file
        std::string tmp_filename = filename + std::string(".parse");
        read_file(tmp_filename.c_str(), p);
        p.push_back(0); // this is the terminator for the sacak algorithm
        
        build(saP_flag_, isaP_flag_, lcpP_flag_, rmq_lcp_P_flag_);
    }
    
    void build(bool saP_flag_, bool isaP_flag_, bool lcpP_flag_, bool rmq_lcp_P_flag_)
    {
        long_type bytes_saP = 0;
        long_type max_sa = p.size() + 1;
        while (max_sa != 0) { max_sa >>= 8; bytes_saP++; }
        
        // SA
        if(saP_flag_)
        {
            _elapsed_time(
            spdlog::info("Using 8 bytes for computing SA of the parsing");
            std::vector<long_type> tmp_saP(p.size(), 0);
            sacak_int(&p[0], &tmp_saP[0], p.size(), alphabet_size);
            
            spdlog::info("Using {} bytes for storing SA of the parsing", bytes_saP);
            saP = sdsl::int_vector<>(tmp_saP.size(), 0ULL, bytes_saP * 8);

            for (long_type i = 0; i < tmp_saP.size(); i++) { saP[i] = tmp_saP[i]; }
            );

            saP_flag = true;
        }
        
        // ISA
        if(isaP_flag_)
        {
            _elapsed_time(
            assert(saP_flag);
            spdlog::info("Using {} bytes for ISA of the parsing", bytes_saP);
            isaP = sdsl::int_vector<>(p.size(), 0ULL, bytes_saP * 8);
            for (long_type i = 0; i < saP.size(); i++) { isaP[saP[i]] = i; }
            );
            isaP_flag = true;
        }
    
        // LCP
        if(lcpP_flag_)
        {
            _elapsed_time(
            assert(saP_flag and isaP_flag);
            spdlog::info("Using {} bytes for LCP of the parsing", bytes_saP);
            lcpP = sdsl::int_vector<>(p.size(), 0ULL, bytes_saP * 8);

            // Kasai et al. LCP construction algorithm
            lcpP[0]  = 0;
            long_type l = 0;
            for (long_type i = 0; i < lcpP.size(); ++i)
            {
                // if i is the last character LCP is not defined
                long_type k = isaP[i];
                if(k > 0)
                {
                    long_type j = saP[k-1];
                    // I find the longest common prefix of the i-th suffix and the j-th suffix.
                    while(p[i+l] == p[j+l]) { l++; }
                    // l stores the length of the longest common prefix between the i-th suffix and the j-th suffix
                    lcpP[k] = l;
                    if(l>0) { l--; }
                }
            }
            );
            lcpP_flag = true;
        }
        
        // RMQ over LCP
        if(rmq_lcp_P_flag_)
        {
            _elapsed_time(
            assert(lcpP_flag);
            spdlog::info("Computing RMQ over LCP of the parsing");
            rmq_lcp_P = sdsl::rmq_succinct_sct<>(&lcpP);
            rmq_lcp_P_flag = true;
            );
        }
        
    }
    
};

}

#endif /* end of include guard: _PFP_PARSE_HH */
