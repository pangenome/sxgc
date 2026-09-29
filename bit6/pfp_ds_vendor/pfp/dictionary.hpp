/* pfp-dictionary - prefix free parsing dictionary
    Copyright (C) 2020 Massimiliano Rossi

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see http://www.gnu.org/licenses/ .
*/
/*!
   \file dictionary.hpp
   \brief dictionary.hpp define and build the prefix-free dictionary data structure.
   \author Massimiliano Rossi
   \date 03/04/2020
*/


#ifndef _PFP_DICTIONARY_HH
#define _PFP_DICTIONARY_HH

#include <queue>
#ifdef SXI_MEMORY_DICTIONARY
#include "sa_workspace.hpp"
#include <numeric>
#endif

#include "utils.hpp"
#undef max

#include <sdsl/rmq_support.hpp>
#include <sdsl/int_vector.hpp>

namespace pfpds
{

template <typename data_type, class colex_comparator_type = std::less<data_type>>
class dictionary
{

public:
    
    bool saD_flag = false;
    bool isaD_flag = false;
    bool daD_flag = false;
    bool lcpD_flag = false;
    bool rmq_lcp_D_flag = false;
    bool colex_id_flag = false;
    bool colex_daD_flag = false;
    
    long_type w;
    
    const colex_comparator_type& colex_comparator;
    
    std::vector<data_type> d;
    sdsl::bit_vector b_d; // Starting position of each phrase in D
    sdsl::bit_vector::rank_1_type rank_b_d;
    sdsl::bit_vector::select_1_type select_b_d;
    
    // size depends on input
    sdsl::int_vector<0> saD;
    sdsl::int_vector<0> isaD;
    sdsl::int_vector<0> daD;
    sdsl::int_vector<0> lcpD;
    sdsl::rmq_succinct_sct<> rmq_lcp_D;
    sdsl::int_vector<0> colex_id;
    sdsl::int_vector<0> inv_colex_id;
    sdsl::int_vector<0> colex_daD;
    sdsl::rmq_succinct_sct<> rmq_colex_daD;
    sdsl::range_maximum_sct<>::type rMq_colex_daD;
    long_type alphabet_size = 0;
    
    dictionary(
    std::vector<data_type>& d_,
    long_type w,
    colex_comparator_type& colex_comparator,
    bool saD_flag_ = true,
    bool isaD_flag_ = true,
    bool daD_flag_ = true,
    bool lcpD_flag_ = true,
    bool rmq_lcp_D_flag_ = true,
    bool colex_id_flag_ = true,
    bool colex_daD_flag = true):
    d(d_), w(w), colex_comparator(colex_comparator)
    {
        assert(d.back() == 0);
        build(saD_flag_, isaD_flag_, daD_flag_, lcpD_flag_, rmq_lcp_D_flag_, colex_id_flag_, colex_daD_flag);
    }
    
    // ---- load support (bit6/pfp_ds_vendor; divergences from upstream
    // r-pfbwt pfp_ds are exactly this constructor and pfp.hpp's
    // defer_build_t one) ----
    // Construct WITHOUT reading .dict or building anything: the caller
    // (bit6/chi_rspace_dump.cpp --pfp-index) restores the only members the
    // pfp_lce_support query reads — b_d (+rank/select, rebuilt on load),
    // isaD, lcpD (+rmq_lcp_D rebuilt on load) — and must not call
    // n_phrases() (which reads d.size(); unused on the load path).
    // Rationale: the normal constructor reads the whole .dict (0.154n bytes
    // at yeast) and scans it to build b_d — the last O(|D|)-scaled
    // construction the folded dump would otherwise still perform.
    struct defer_build_t {};

    dictionary(
    long_type w_,
    colex_comparator_type& colex_comparator_,
    defer_build_t)
    : w(w_), colex_comparator(colex_comparator_)
    { }

    dictionary(
    std::string filename,
    long_type w,
    colex_comparator_type& colex_comparator,
    bool saD_flag_ = true,
    bool isaD_flag_ = true,
    bool daD_flag_ = true,
    bool lcpD_flag_ = true,
    bool rmq_lcp_D_flag_ = true,
    bool colex_id_flag_ = true,
    bool colex_daD_flag = true)
    :
    w(w), colex_comparator(colex_comparator)
    {
        // Building dictionary from file
        std::string tmp_filename = filename + std::string(".dict");
        read_file(tmp_filename.c_str(), d);
        assert(d[0] == Dollar);
        // Prepending w dollars to d
        // 1. Count how many dollars there are
        int i = 0;
        int n_dollars = 0;
        while(i < d.size() && d[i++] == Dollar) { ++n_dollars; }
        std::vector<data_type> dollars(w-n_dollars, Dollar);
#ifdef SXI_MEMORY_DICTIONARY
        // Avoid vector's doubling policy when prepending the window padding.
        d.reserve(d.size() + dollars.size());
#endif
        d.insert(d.begin(), dollars.begin(), dollars.end());
        assert(d.back() == 0);
        
        build(saD_flag_, isaD_flag_, daD_flag_, lcpD_flag_, rmq_lcp_D_flag_, colex_id_flag_, colex_daD_flag);
    }
    
    long_type length_of_phrase(long_type id) const
    {
        assert(id > 0);
        return select_b_d(id+1)-select_b_d(id) - 1; // to remove the EndOfWord
    }
    
    // DA can be derived in O(1) from the existing phrase-boundary rank index.
    long_type phrase_at_sa(long_type i) const {
        if (daD_flag) return daD[i];
        const long_type pos = saD[i];
        return rank_b_d(pos) + (b_d[pos] ? 1 : 0) - 1;
    }

    long_type n_phrases() const { return rank_b_d(d.size()-1); }
    
    void build(bool saD_flag_, bool isaD_flag_, bool daD_flag_, bool lcpD_flag_, bool rmq_lcp_D_flag_, bool colex_id_flag_, bool colex_daD_flag_){
      
        // Get alphabet size
        alphabet_size = (*std::max_element(d.begin(), d.end())) + 1;
        
        // Building the bitvector with a 1 in each starting position of each phrase in D
        // Also compute length of the longest phrase
        b_d.resize(d.size());
        long_type max_phrase_length = 0;
        for(long_type i = 0; i < b_d.size(); ++i) { b_d[i] = false; } // bug in resize
        b_d[0] = true; // Mark the first phrase
        long_type prev_start = 0;
        for(long_type i = 1; i < d.size(); ++i )
        {
            if (d[i-1] == EndOfWord)
            {
                b_d[i] = true;
                max_phrase_length = std::max(max_phrase_length, i - prev_start - 2);
                prev_start = i;
            }
        }
        b_d[d.size()-1] = true; // This is necessary to get the length of the last phrase
        
        rank_b_d = sdsl::bit_vector::rank_1_type(&b_d);
        select_b_d = sdsl::bit_vector::select_1_type(&b_d);
        
        // SA
        if(saD_flag_)
        {
            _elapsed_time(
#ifdef SXI_MEMORY_DICTIONARY
            long_type bytes_saD = 1;
            for (long_type v = (d.size() + 1) >> 8; v; v >>= 8) ++bytes_saD;
            if (d.size() <= INT32_MAX && alphabet_size <= INT32_MAX) {
                spdlog::info("MEMORY_SA workspace_bytes=4 entries={}", d.size());
                std::vector<uint32_t> tmp(d.size(), 0);
                if (sxi_sa32(d.data(), tmp.data(), d.size(), alphabet_size) < 0)
                    throw std::runtime_error("32-bit gSACA-K failed");
                saD = sdsl::int_vector<>(d.size(), 0ULL, bytes_saD * 8);
                for (long_type i = 0; i < d.size(); ++i) saD[i] = tmp[i];
            } else {
                spdlog::info("MEMORY_SA workspace_bytes=8 entries={}", d.size());
                std::vector<long_type> tmp(d.size(), 0);
                gsacak_templated<data_type>(d.data(), tmp.data(), d.size(), alphabet_size);
                saD = sdsl::int_vector<>(d.size(), 0ULL, bytes_saD * 8);
                for (long_type i = 0; i < d.size(); ++i) saD[i] = tmp[i];
            }
            spdlog::info("Using {} bytes for storing SA of the dictionary", bytes_saD);
#else
            spdlog::info("Using 8 bytes for computing SA of the dictionary");
            std::vector<long_type> tmp_saD(d.size(), 0);
            gsacak_templated<data_type>(&d[0], &tmp_saD[0], d.size(), alphabet_size);

            long_type bytes_saD = 0;
            long_type max_sa = d.size() + 1;
            while (max_sa != 0) { max_sa >>= 8; bytes_saD++; }
            spdlog::info("Using {} bytes for storing SA of the dictionary", bytes_saD);
            saD = sdsl::int_vector<>(tmp_saD.size(), 0ULL, bytes_saD * 8);

            for (long_type i = 0; i < tmp_saD.size(); i++) { saD[i] = tmp_saD[i]; }
#endif
            );
            saD_flag = true;
        }
    
        // DA
        if(daD_flag_)
        {
            _elapsed_time(
            assert(saD_flag);
            long_type bytes_daD = 0;
            long_type ps = n_phrases(); assert(ps != 0);
            while (ps != 0) { ps >>= 8; bytes_daD++; }
            spdlog::info("Using {} bytes for DA of the dictionary", bytes_daD);
            daD = sdsl::int_vector<>(d.size(), 0ULL, bytes_daD * 8);
            for (long_type i = 0; i < saD.size(); i++)
            {
                long_type out = rank_b_d.rank(saD[i]);
                if (b_d[saD[i]]) { out += 1; }
                daD[i] = out - 1;
            }
            );
            daD_flag = true;
        }
        
        // ISA
        if (isaD_flag_)
        {
            _elapsed_time(
            assert(saD_flag);
            long_type bytes_isaD = 0;
            long_type max_sa = d.size() + 1;
            while (max_sa != 0) { max_sa >>= 8; bytes_isaD++; }
            spdlog::info("Using {} bytes for ISA of the dictionary", bytes_isaD);
            isaD = sdsl::int_vector<>(d.size(), 0ULL, bytes_isaD * 8);
            for (long_type i = 0; i < saD.size(); i++) { isaD[saD[i]] = i; }
            );
            isaD_flag = true;
        }
        
        // LCP
        if (lcpD_flag_)
        {
            _elapsed_time(
            assert(saD_flag and isaD_flag);
            long_type bytes_lcpD = 0;
            long_type mpl = max_phrase_length;
            while (mpl != 0) { mpl >>= 8; bytes_lcpD++; }
            spdlog::info("Using {} bytes for LCP of the dictionary", bytes_lcpD);
            lcpD = sdsl::int_vector<>(d.size(), 0ULL, bytes_lcpD * 8);
    
            // Kasai et al. LCP construction algorithm
            lcpD[0]  = 0;
            long_type l = 0;
            for (long_type i = 0; i < lcpD.size(); ++i)
            {
                // if i is the last character LCP is not defined
                long_type k = isaD[i];
                if(k > 0)
                {
                    long_type j = saD[k-1];
                    // I find the longest common prefix of the i-th suffix and the j-th suffix.
                    while(d[i+l] == d[j+l] and d[i + l] != EndOfWord) { l++; }
                    // l stores the length of the longest common prefix between the i-th suffix and the j-th suffix
                    lcpD[k] = l;
                    if(l>0) { l--; }
                }
            }
            );
            lcpD_flag = true;
        }
        
        // RMQ over LCP
        if(rmq_lcp_D_flag_)
        {
            _elapsed_time(
            assert(lcpD_flag);
            spdlog::info("Computing RMQ over LCP of dictionary");
            rmq_lcp_D = sdsl::rmq_succinct_sct<>(&lcpD);
            );
            rmq_lcp_D_flag = true;
        }
    
        // co-lex document array of the dictionary.
        if(colex_daD_flag_ or colex_id_flag_)
        {
            _elapsed_time(
            assert(daD_flag || !colex_daD_flag_);
            // allocating space for colex DA
            if (colex_daD_flag_)
            {
                long_type bytes_colex_daD = 0;
                long_type ps = n_phrases(); assert(ps != 0);
                while (ps != 0) { ps >>= 8; bytes_colex_daD++; }
                spdlog::info("Using {} bytes for colex DA of the dictionary", bytes_colex_daD);
                colex_daD = sdsl::int_vector<>(d.size(), 0ULL, bytes_colex_daD * 8);
            }
            
            // allocating space for colex_id and inv_colex_id
            if (colex_daD_flag_ or colex_id_flag_)
            {
                long_type bytes_inv_colex_id = 0;
                long_type ps = n_phrases(); assert(ps != 0);
                while (ps != 0) { ps >>= 8; bytes_inv_colex_id++; }
                spdlog::info("Using {} bytes for colex id and inverse colex id array of the dictionary", bytes_inv_colex_id);
                colex_id = sdsl::int_vector<>(n_phrases(), 0ULL, bytes_inv_colex_id * 8);
                inv_colex_id = sdsl::int_vector<>(n_phrases(), 0ULL, bytes_inv_colex_id * 8);
            }
            
            compute_colex_da(colex_id_flag_, colex_daD_flag_);
            
            if (colex_daD_flag_)
            {
                rmq_colex_daD = sdsl::rmq_succinct_sct<>(&colex_daD);
                rMq_colex_daD = sdsl::range_maximum_sct<>::type(&colex_daD);
            }
            );

            if (colex_daD_flag_) { colex_daD_flag = true; }
            if (colex_id_flag_) { colex_id_flag = true; }
        }
    }
    
    void compute_colex_da(bool colex_id_flag_, bool colex_daD_flag_)
    {

#ifdef SXI_MEMORY_DICTIONARY
        if (n_phrases() > UINT32_MAX)
            throw std::runtime_error("colex phrase IDs exceed uint32_t");
        // Sort IDs against the original dictionary, without a second copy of D
        // or one vector allocation per phrase. The ordering matches reverse words.
        std::vector<uint32_t> ids(n_phrases());
        std::iota(ids.begin(), ids.end(), uint32_t(0));
        std::sort(ids.begin(), ids.end(), [this](uint32_t a, uint32_t b) {
            long_type ab = select_b_d(long_type(a)+1), ae = select_b_d(long_type(a)+2)-1;
            long_type bb = select_b_d(long_type(b)+1), be = select_b_d(long_type(b)+2)-1;
            while (ae > ab && be > bb) {
                const data_type ac = d[--ae], bc = d[--be];
                if (ac != bc) return colex_comparator(ac, bc);
            }
            return ae == ab && be != bb;
        });
        for (long_type i = 0; i < ids.size(); ++i) {
            colex_id[i] = ids[i];
            inv_colex_id[ids[i]] = i;
        }
        for (long_type i = 0; i < colex_daD.size(); ++i)
            colex_daD[i] = inv_colex_id[daD[i] % inv_colex_id.size()];
#else
        // ---------- just sort the reversed phrases
        std::vector<std::pair<std::vector<data_type>,uint32_t>> rev_dict(n_phrases());
        long_type i = 0;
        long_type rank = 0;
        while(i < d.size()-1)
        {
            while((i < d.size()-1) and (d[i] != EndOfWord)) { rev_dict[rank].first.emplace_back(d[i++]); }
            i++;
            std::reverse(rev_dict[rank].first.begin(), rev_dict[rank].first.end());
            rev_dict[rank].second = rank;
            rank++;
        }
        
        std::sort(rev_dict.begin(),rev_dict.end(),
                  [this] (std::pair<std::vector<data_type>,uint32_t>& l, std::pair<std::vector<data_type>,uint32_t>& r)
                  {
                      std::pair<typename std::vector<data_type>::iterator, typename std::vector<data_type>::iterator> mismatch;
                      if (l.first.size() < r.first.size())
                      {
                          mismatch = std::mismatch(l.first.begin(), l.first.end(), r.first.begin());
                          if (mismatch.first == l.first.end())
                          {
                              return true;
                          }
                          return this->colex_comparator(*(mismatch.first), *(mismatch.second));
                      }
                      else
                      {
                          mismatch = std::mismatch(r.first.begin(), r.first.end(), l.first.begin());
                          if (mismatch.first == r.first.end())
                          {
                              return false;
                          }
                          return this->colex_comparator(*(mismatch.second), *(mismatch.first));
                      }
                  } );
        
        for (i = 0; i < colex_id.size(); i++) { colex_id[i] = rev_dict[i].second; }
        for (i = 0; i < colex_id.size(); i++) { inv_colex_id[colex_id[i]] = i; }
        
        for (i = 0; i < colex_daD.size(); ++i)
        {
            colex_daD[i] = inv_colex_id[daD[i] % inv_colex_id.size()];
        }
#endif
    }
};


//template <>
//void dictionary<uint8_t>::compute_colex_da(bool colex_id_flag_, bool colex_daD_flag_){
//
//    for (long_type i = 0, j = 0; i < d.size(); ++i)
//    {
//        if (d[i + 1] == EndOfWord)
//        {
//            colex_id[j] = j;
//            inv_colex_id[j++] = i;
//        }
//    }
//
//    // buckets stores the start and the end of each bucket.
//    std::queue<std::pair<long_type,long_type>> buckets;
//    // the first bucket is the whole array.
//    buckets.push({0,colex_id.size()});
//
//    // for each bucket
//    while(not buckets.empty())
//    {
//        auto bucket = buckets.front(); buckets.pop();
//        long_type start = bucket.first;
//        long_type  end = bucket.second;
//        if ((start < end) && (end - start > 1))
//        {
//            std::vector<uint32_t> count(256, 0);
//            for (long_type i = start; i < end; ++i)
//            {
//                count[d[inv_colex_id[i]]]++;
//            }
//
//            std::vector<uint32_t> psum(256, 0);
//            for (long_type i = 1; i < 256; ++i)
//            {
//                psum[i] = psum[i - 1] + count[i - 1];
//            }
//
//            std::vector<long_type > tmp(end - start, 0);
//            std::vector<long_type > tmp_id(end - start, 0);
//            for (long_type i = start; i < end; ++i)
//            {
//                long_type index = psum[d[inv_colex_id[i]]]++;
//                tmp[index] = std::min(inv_colex_id[i] - 1, (long_type) d.size() - 1);
//                tmp_id[index] = colex_id[i];
//            }
//
//            // Recursion
//            long_type tmp_start = 0;
//            for (long_type  i = 0; i < 256; ++i)
//            {
//                for (long_type  j = 0; j < count[i]; ++j)
//                {
//                    inv_colex_id[start + j] = tmp[tmp_start];
//                    colex_id[start + j] = tmp_id[tmp_start++];
//                }
//                end = start + count[i];
//                if (i > EndOfWord) { buckets.push({start, end}); }
//                start = end;
//            }
//        }
//
//    }
//
//    // computing inverse colex id
//    for (long_type  i = 0; i < colex_id.size(); ++i) { inv_colex_id[colex_id[i]] = i; }
//
//    if (colex_daD_flag_)
//    {
//        for (long_type  i = 0; i < colex_daD.size(); ++i)
//        {
//            colex_daD[i] = inv_colex_id[daD[i] % inv_colex_id.size()];
//        }
//    }
//
//}

}

#endif /* end of include guard: _PFP_DICTIONARY_HH */
